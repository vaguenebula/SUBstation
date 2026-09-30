#pragma once
// The immutable render graph handed to the audio thread. The engine builds a new
// RenderSnapshot on the edit side whenever the arrangement changes and publishes
// it with a single atomic pointer swap. All positions are already converted to
// samples, so the audio thread never deals with beats or seconds.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "AudioSource.h"
#include "Processor.h"
#include "Warp.h"
#include "rt/RtUtils.h"

namespace gil {

// Per-track state that changes continuously and therefore lives outside the
// snapshot. Shared between all snapshots that contain the track.
struct TrackParams {
    // Written by the API, read by the audio thread.
    std::atomic<float> gain{1.f};
    std::atomic<float> pan{0.f};
    std::atomic<bool> mute{false};
    std::atomic<bool> solo{false};

    // Written by the audio thread, read and reset by the API.
    std::atomic<float> peakLeft{0.f};
    std::atomic<float> peakRight{0.f};

    // Audio-thread-only fader smoothing; persists across snapshots.
    SmoothedValue gainLeft;
    SmoothedValue gainRight;
    double smoothingSampleRate = 0.0;
};

struct ClipRender {
    enum class Playback : uint8_t {
        Direct,     // source frames 1:1 (unwarped, or warped at its own tempo, no pitch change)
        Resample,   // Re-Pitch: speed and pitch change together
        Stretch,    // time stretcher: speed and pitch independent
    };

    std::shared_ptr<const AudioSource> source;
    int64_t start = 0;          // timeline position, samples
    int64_t length = 0;         // timeline samples
    int64_t sourceOffset = 0;   // first source frame played
    double rate = 1.0;          // source frames per timeline frame (project tempo / clip tempo)
    float gain = 1.f;
    float panLeft = 1.f;        // clip pan as balance gains
    float panRight = 1.f;
    Playback playback = Playback::Direct;
    StretchConfig stretchConfig = StretchConfig::Standard;
    float transpose = 0.f;      // semitones (Stretch only)
    bool preserveFormants = false;
    uint64_t key = 0;           // identifies the clip across snapshots (stretch voice continuity)

    // Source position (fractional frames) heard at timeline sample `t`.
    double sourceAt(int64_t t) const noexcept { return sourceOffset + static_cast<double>(t - start) * rate; }
};

// Delays a track's output so that it lines up with the track whose devices add
// the most latency (plug-in delay compensation). Rendering-thread state; the
// edit side allocates it and keeps it across snapshots.
class DelayLine {
public:
    explicit DelayLine(int capacity)
        : left_(static_cast<size_t>(std::max(capacity, 1))), right_(static_cast<size_t>(std::max(capacity, 1))) {}
    int capacity() const noexcept { return static_cast<int>(left_.size()); }

    // Real-time. When the delay changes it starts again from silence rather than
    // replaying stale audio.
    void process(float* left, float* right, int frames, int delay) noexcept {
        delay = std::clamp(delay, 0, capacity() - 1);
        if (delay != delay_) {
            std::fill(left_.begin(), left_.end(), 0.f);
            std::fill(right_.begin(), right_.end(), 0.f);
            delay_ = delay;
            write_ = 0;
        }
        if (delay == 0) return;
        const int size = capacity();
        for (int i = 0; i < frames; ++i) {
            left_[write_] = left[i];
            right_[write_] = right[i];
            int read = write_ - delay;
            if (read < 0) read += size;
            left[i] = left_[read];
            right[i] = right_[read];
            if (++write_ == size) write_ = 0;
        }
    }

private:
    std::vector<float> left_, right_;
    int write_ = 0;
    int delay_ = 0;
};

// A note of a MIDI clip, already cut to its clip: the renderer sends a note-on
// at `start` and a note-off at `end`.
struct NoteRender {
    int64_t start = 0;  // timeline samples
    int64_t end = 0;    // > start
    uint8_t key = 60;
    uint8_t velocity = 100;
};

struct TrackRender {
    uint32_t id = 0;
    std::shared_ptr<TrackParams> params;
    std::vector<ClipRender> clips;  // sorted by start
    int64_t maxClipLength = 0;      // bounds the binary search window
    std::vector<NoteRender> notes;  // sorted by start
    std::vector<std::shared_ptr<Processor>> inserts;
    int latency = 0;                    // samples the enabled inserts add
    int compensation = 0;               // samples the track is delayed by to line up with the slowest one
    std::shared_ptr<DelayLine> delay;   // for the live renderer (offline renders bring their own)
};

struct RenderSnapshot {
    double sampleRate = 48000.0;
    double tempo = 120.0;
    int timeSigNum = 4;
    int timeSigDen = 4;
    bool loopEnabled = false;
    int64_t loopStart = 0;
    int64_t loopEnd = 0;
    int64_t clipFadeSamples = 0;
    int maxLatency = 0;  // the output lags the timeline by this much (tracks and metronome alike)
    std::vector<TrackRender> tracks;
    WarpVoiceSet warpVoices;  // stretchers for the live renderer (offline renders bring their own)

    double samplesPerBeat() const { return sampleRate * 60.0 / tempo; }
};

}  // namespace gil
