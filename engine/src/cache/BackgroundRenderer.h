#pragma once
// Background freezing, phase 2 (docs/engine/background-freeze.md): what hasn't
// played since it last changed is rendered into the cache in the background,
// with CPU nobody else wants, so that it plays from the cache the first time.
//
// The background renderer is a second Renderer, on a thread of its own at the
// lowest priority the system has, over a snapshot of its own (the shadow
// snapshot: the engine builds it from the live one, with every device swapped
// for a shadow instance in the same state, and render state of its own). It
// runs the same cache logic as the live renderer, in each cache point's
// background lane: a strip whose blocks are good plays them (its shadows stand
// idle), its shadows start a warm-up before blocks run out, and what they put
// out is kept once they are warm. What it renders is never heard.
//
// Where it renders (the planner): from a warm-up before the first frame some
// strip has no good block for, looking first just ahead of the playhead (while
// it plays: what will be heard next), then on from the playhead to the end of
// the song, then from the start. It keeps rendering on while there is more to
// render close ahead, and jumps over what is cached.
//
// How much (the governor): only what the system's scheduler gives an idle
// thread; on top, it halves its share while the live callbacks take more than
// 60% of their time, and stops for 5 s above 80%.
//
// Threads: the engine's main thread builds the shadow snapshot and changes the
// shadows (their state, new ones) only with the renderer parked (park(): it is
// between chunks and stays there until unpark()).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Renderer.h"
#include "Snapshot.h"

namespace sub {

class BackgroundRenderer {
public:
    // What it watches of the engine (all outlive it).
    struct Watch {
        const CacheSettings& settings;
        const std::atomic<int64_t>& playhead;  // the live playhead (samples)
        const std::atomic<bool>& playing;
        const std::atomic<float>& load;        // the live callbacks' share of their time (smoothed)
        const std::atomic<bool>& deviceRunning;
        // What it tells the cache store: it is reading blocks (between the start
        // and the end of a chunk), and how many chunks it has finished (the store
        // frees what it unpublished once no renderer can still be reading it).
        std::atomic<bool>& busy;
        std::atomic<uint64_t>& epoch;
        // After each chunk: a round of the cache store's work (it renders far faster
        // than real time: what it captured is taken, and empty blocks handed out).
        std::function<void()> afterChunk;
    };

    // `thread`: false for none (only renderNow() renders: tests).
    BackgroundRenderer(Watch watch, double sampleRate, bool thread);
    ~BackgroundRenderer();
    BackgroundRenderer(const BackgroundRenderer&) = delete;
    BackgroundRenderer& operator=(const BackgroundRenderer&) = delete;

    // Main thread. Takes the renderer off its thread between chunks: true when it
    // is parked (until unpark()); false while it finishes a chunk (it parks at
    // its end: try again).
    bool park();
    void unpark();
    // While parked: what it renders from now on (null: nothing), and where the
    // song ends (samples: it renders up to there, and a warm-up on).
    void adopt(std::shared_ptr<const RenderSnapshot> snapshot, int64_t songEnd);
    const RenderSnapshot* snapshot() const noexcept { return snapshot_.get(); }

    // Renders up to `frames` frames of background work now, on the calling
    // thread (the thread waits meanwhile); returns the frames rendered (0:
    // nothing to render).
    int64_t renderNow(int64_t frames);
    uint64_t framesRendered() const noexcept { return framesRendered_.load(std::memory_order_relaxed); }

    static constexpr int kChunk = Renderer::kMaxBlock;

private:
    // Per strip of the snapshot (its tracks, then the master): whether what it
    // renders can be kept (it has shadows, brought up to date, and so has all
    // that feeds it), and how long it renders before that (from a start: a
    // warm-up and its latency, on top of the longest of what feeds it).
    struct Chain {
        bool keep = false;
        int64_t lead = 0;
        int64_t own = 0;  // its own part: a warm-up and its latency
    };
    // Per strip: stretches the planner doesn't look at for its gaps (until the
    // snapshot changes): gaps it rendered but couldn't keep (its devices started
    // where notes were held, while the cache played: they only come clean
    // where the notes end). The live renderer fills those.
    struct Skipped {
        int64_t from, to;
    };

    void run();
    // One chunk where the planner says (holding renderMutex_); false: nothing to render.
    bool step();
    // Where to render next: true if positioned (renderer_ at the start, window set).
    bool plan();
    void computeChains();
    // The first frame in [from, to) some strip it can keep has no good block for
    // (in order of play, Linear); `to` if none.
    int64_t firstGap(int64_t from, int64_t to) const;
    // How long before `gap` it starts: the longest lead of the strips without a
    // good block there.
    int64_t prerollAt(int64_t gap) const;
    int64_t longestLead() const;
    // How long before `gap` strip `strip` must start: its own part, and the lead
    // of what feeds it unless that plays from its cache meanwhile.
    int64_t leadAt(size_t strip, int64_t gap, int depth = 0) const;
    void skipGap(int64_t gap);  // the strips without a good block at `gap`: not looked at there
    // The first frame in [from, to) strip `strip` has no good block for, but
    // where it was skipped; `to` if none.
    int64_t stripGap(const StripCacheRender& cache, size_t strip, int64_t from, int64_t to) const;
    uint64_t framesLost() const;  // what its lanes had no block for, so far

    Watch watch_;
    Renderer renderer_;
    std::shared_ptr<const RenderSnapshot> snapshot_;
    int64_t songEnd_ = 0;
    std::vector<Chain> chains_;
    std::vector<std::vector<Skipped>> skipped_;
    bool positioned_ = false;
    int64_t windowEnd_ = 0;    // it renders on up to here, then plans again
    int64_t nextCheck_ = 0;    // where it looks again whether there is more ahead to render
    int64_t lastGap_ = -1;     // where it last planned to render from, and what its lanes had lost then
    uint64_t lostAtPlan_ = 0;
    bool ahead_ = false;       // ... ahead of the playhead, which played
    std::chrono::steady_clock::duration stuckWait_{};     // the budget is spent: it waits, longer each time
    std::chrono::steady_clock::time_point stuckUntil_{};  // it renders nothing until then
    std::atomic<uint64_t> framesRendered_{0};

    std::mutex renderMutex_;   // held while rendering a chunk, and while parked
    std::atomic<bool> parkRequested_{false};
    std::mutex stateMutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace sub
