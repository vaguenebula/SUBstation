#pragma once

// Display values played back at the audio's own pace, for an editor that draws a device's
// displays as they move (a sweep, an LFO's phase). The engine publishes them a block at a time
// (a 1024-frame block brings four values at 256 frames a value, then nothing for 21 ms) while the
// screen ticks every 16 ms: drawing the latest value each tick would move what is drawn in uneven
// jerks. A playhead runs through the frames at their rate, about the largest recent batch behind
// the newest, interpolating between them; a little faster when further behind, a little slower
// when nearer (never backwards), and jumping only when it falls far behind (or waited at the
// newest for frames that then came). Streams published together (displays at the same rate) are
// a frame's `Streams` values, so one playhead serves them all. Pure arithmetic on a fixed ring.
//
//   DisplayPlayback<3> playback;
//   playback.append({phase, sweep, level});  // each frame that came this tick, then
//   playback.endBatch(count);
//   playback.advance(tickSeconds(), sampleRate / samplesPerValue);
//   const double sweep = playback.logValue(1);

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace sub::ui {

template <int Streams>
class DisplayPlayback {
public:
    static constexpr int kStreams = Streams;
    static constexpr int kCapacity = 64;    // frames kept
    static constexpr int kMaxTarget = 16;   // the target lag's most, in frames
    static constexpr double kWindow = 0.5;  // seconds of batches the target lag looks back on
    using Frame = std::array<float, Streams>;

    // A frame, in the order they arrived; then endBatch(how many came this tick, 0: none).
    void append(const Frame& frame) {
        frames_[static_cast<size_t>(count_ % kCapacity)] = frame;
        ++count_;
    }
    void endBatch(int batch) {
        if (batch <= 0)
            return;
        batches_[size_t(batchNext_)] = {0.0, batch};
        batchNext_ = (batchNext_ + 1) % int(batches_.size());
        batchCount_ = std::min(batchCount_ + 1, int(batches_.size()));
    }

    // Moves the playhead on by `dtSeconds` at `framesPerSecond`.
    void advance(double dtSeconds, double framesPerSecond) {
        dtSeconds = std::max(0.0, dtSeconds);
        for (int i = 0; i < batchCount_; ++i) batches_[size_t(i)].age += dtSeconds;
        if (empty())
            return;
        const double last = double(newest());
        const double lagWanted = target();
        const double step = dtSeconds * std::max(0.0, framesPerSecond);
        // Frames came while it waited at the newest (after a pause): it starts again a target behind them.
        if (waiting_ && head_ < last)
            head_ = std::max(head_, last - lagWanted - step);
        waiting_ = false;
        // A little faster when it would end further behind than the target, a little slower when nearer
        // (the lag after this tick's move: before it, the lag runs a tick's worth more).
        const double behind = (last - (head_ + step) - lagWanted) / lagWanted;
        head_ += step * std::clamp(1.0 + 0.1 * behind, 0.5, 1.5);
        if (last - head_ > 2.0 * lagWanted + 1.0)
            head_ = last - lagWanted;
        if (head_ >= last) {
            head_ = last;
            waiting_ = true;
        }
    }

    // A stream's value at the playhead: linear between the frames either side; a phase (0..1) the
    // short way round; in log (for positive values: sweeps).
    double value(int stream) const {
        return interpolate(stream, [](double a, double b, double t) { return a + (b - a) * t; });
    }
    double phase(int stream) const {
        return interpolate(stream, [](double a, double b, double t) {
            double d = b - a;
            d -= std::round(d);  // the short way round
            const double v = a + d * t;
            return v - std::floor(v);
        });
    }
    double logValue(int stream) const {
        return interpolate(stream, [](double a, double b, double t) {
            if (a > 0.0 && b > 0.0)
                return std::exp(std::log(a) + (std::log(b) - std::log(a)) * t);
            return a + (b - a) * t;
        });
    }

    void snapToNewest() {
        head_ = empty() ? 0.0 : double(newest());
        waiting_ = true;
    }
    void clear() {
        count_ = 0;
        head_ = 0.0;
        waiting_ = true;
        batchCount_ = batchNext_ = 0;
    }

    bool empty() const { return count_ == 0; }
    qint64 newest() const { return count_ - 1; }  // the newest frame's number (from 0)
    double head() const { return head_; }         // where the playhead is, in frames
    double lag() const { return empty() ? 0.0 : double(newest()) - head_; }
    // The lag it keeps to: the largest batch of the last kWindow seconds (at least 1, at most kMaxTarget).
    int target() const {
        int most = 1;
        for (int i = 0; i < batchCount_; ++i) {
            if (batches_[size_t(i)].age <= kWindow)
                most = std::max(most, batches_[size_t(i)].size);
        }
        return std::min(most, kMaxTarget);
    }
    // Frame `index`'s value of `stream` (held to the frames kept).
    float at(qint64 index, int stream) const {
        if (empty())
            return 0.0f;
        index = std::clamp(index, std::max<qint64>(0, count_ - kCapacity), newest());
        return frames_[static_cast<size_t>(index % kCapacity)][size_t(std::clamp(stream, 0, Streams - 1))];
    }

private:
    struct Batch {
        double age = 0.0;
        int size = 0;
    };
    template <typename Mix>
    double interpolate(int stream, Mix mix) const {
        if (empty())
            return 0.0;
        const double h = std::clamp(head_, double(std::max<qint64>(0, count_ - kCapacity)), double(newest()));
        const auto i = static_cast<qint64>(std::floor(h));
        return mix(double(at(i, stream)), double(at(std::min(i + 1, newest()), stream)), h - double(i));
    }

    std::array<Frame, kCapacity> frames_{};
    qint64 count_ = 0;     // frames appended
    double head_ = 0.0;    // the playhead (a frame number)
    bool waiting_ = true;  // at the newest, waiting for more
    std::array<Batch, 32> batches_{};  // the recent batches (a ring)
    int batchCount_ = 0, batchNext_ = 0;
};

}  // namespace sub::ui
