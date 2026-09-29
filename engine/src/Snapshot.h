#pragma once
// The immutable render graph handed to the audio thread. The engine builds a new
// RenderSnapshot on the edit side whenever the arrangement changes and publishes
// it with a single atomic pointer swap. All positions are already converted to
// samples, so the audio thread never deals with beats or seconds.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "AudioSource.h"
#include "Processor.h"
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
    std::shared_ptr<const AudioSource> source;
    int64_t start = 0;          // timeline position, samples
    int64_t length = 0;         // samples
    int64_t sourceOffset = 0;   // first source frame played
    float gain = 1.f;
};

struct TrackRender {
    uint32_t id = 0;
    std::shared_ptr<TrackParams> params;
    std::vector<ClipRender> clips;  // sorted by start
    int64_t maxClipLength = 0;      // bounds the binary search window
    std::vector<std::shared_ptr<Processor>> inserts;
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
    std::vector<TrackRender> tracks;

    double samplesPerBeat() const { return sampleRate * 60.0 / tempo; }
};

}  // namespace gil
