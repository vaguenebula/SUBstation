#pragma once
// A reversed copy of a decoded file being written on a thread of its own
// (EngineBridge::startReversed), as a 32-bit float WAV file: from the source's
// end a chunk at a time, so a long file neither holds up the window nor needs a
// second copy of itself in memory; then decoded there. Followed as the engine's
// renders are (RenderTask): progress, done, cancel(). Cancelled or failed, what
// was written of it goes.

#include "audio/RenderTask.h"

#include <QString>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace sub {
class AudioSource;
class Engine;
}  // namespace sub

namespace sub::app {

inline constexpr qint64 kReverseChunk = qint64{1} << 18;  // frames written at a time

class ReverseJob : public RenderTask {
    Q_OBJECT

public:
    // Starts writing the reversed copy of `source` to `path` (its thread runs at once).
    ReverseJob(sub::Engine& engine, std::shared_ptr<const sub::AudioSource> source, const QString& path,
               qint64 chunkFrames = kReverseChunk, QObject* parent = nullptr);
    ~ReverseJob() override;  // cancelled, and waited for

    qint64 frames() const { return frames_; }
    double seconds() const { return seconds_; }

    // How far it got: 0..1 (writing is most of it; decoding the copy the rest).
    double progress() const override;
    bool done() const override { return done_.load(std::memory_order_acquire); }
    bool cancelled() const override { return cancel_.load(std::memory_order_relaxed); }

    // Waits for it: the copy, decoded, or null if it was cancelled (its file
    // gone). Throws EditError if it couldn't be written.
    std::shared_ptr<sub::AudioSource> finish();

protected:
    void cancelJob() override { cancel_.store(true, std::memory_order_relaxed); }

private:
    void run();

    sub::Engine& engine_;
    std::shared_ptr<const sub::AudioSource> source_;
    qint64 frames_;
    double seconds_;
    qint64 chunk_;
    std::string utf8Path_;
    std::atomic<qint64> written_{0};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> done_{false};
    std::shared_ptr<sub::AudioSource> loaded_;  // the thread's, until done
    std::string error_;                       // the thread's, until done
    std::thread thread_;
};

}  // namespace sub::app
