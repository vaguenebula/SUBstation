#pragma once
// A render running on a thread of its own (Engine::startExport,
// Engine::startTrackRender): exporting the arrangement, freezing a track. The
// UI polls its progress, may cancel it, and finishes it from the thread that
// started it, which gives live output back (it is silent meanwhile).
//
// The render takes the engine's snapshot as it was when it started (holding it,
// and so its processors, sources and buffers) and renders it without the
// engine's lock, so the UI's calls (meters, the playhead) don't wait for it.

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace sub {

class Engine;

class RenderJob {
public:
    ~RenderJob();  // not finished: cancelled, and finished (its errors dropped)
    RenderJob(const RenderJob&) = delete;
    RenderJob& operator=(const RenderJob&) = delete;

    const std::string& path() const noexcept { return path_; }
    // How far it got: 0..1 of what it renders.
    double progress() const noexcept;
    // The thread rendering has ended (finish() returns at once).
    bool done() const noexcept { return rendered_.load(std::memory_order_acquire); }
    // Stops it soon. Its file goes (what was written of it).
    void cancel() noexcept { cancel_.store(true, std::memory_order_relaxed); }
    bool cancelled() const noexcept { return cancel_.load(std::memory_order_relaxed); }
    // Waits for the render, gives live output back, and returns the frames
    // written: none (nullopt) if it was cancelled before the end. Throws what
    // went wrong (its file gone then too). From the thread that started it.
    std::optional<int64_t> finish();

private:
    friend class Engine;
    RenderJob(Engine* engine, std::string path, int64_t total) : engine_(engine), path_(std::move(path)), total_(total) {}
    // `body` renders on the job's thread: it reports its frames with advance(),
    // stops (returning nullopt) when stopping(), and returns the frames written.
    void start(std::function<std::optional<int64_t>(RenderJob&)> body);
    void advance(int64_t frames) noexcept { done_.fetch_add(frames, std::memory_order_relaxed); }
    bool stopping() const noexcept { return cancelled(); }
    void end();  // waits for the thread, and gives the engine back (once)

    Engine* engine_;  // null once ended (or once the engine went first)
    std::string path_;
    int64_t total_;
    std::atomic<int64_t> done_{0};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> rendered_{false};
    std::optional<int64_t> frames_;
    std::exception_ptr error_;
    std::thread thread_;
};

}  // namespace sub
