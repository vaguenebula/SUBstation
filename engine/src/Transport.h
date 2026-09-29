#pragma once
// State shared between the API threads and the audio thread.

#include <atomic>
#include <cstdint>

#include "AudioSource.h"
#include "rt/RtUtils.h"

namespace gil {

struct TransportCommand {
    enum class Type : uint8_t { Play, Stop, Locate };
    Type type = Type::Stop;
    double beat = 0.0;  // Locate target
};

struct SharedState {
    // API -> audio thread. Pushed only while holding the engine's edit mutex, so
    // there is exactly one producer.
    SpscQueue<TransportCommand, 256> commands;
    std::atomic<bool> metronome{false};
    std::atomic<float> masterGain{1.f};
    std::atomic<const AudioSource*> previewSource{nullptr};
    std::atomic<uint32_t> previewSerial{0};
    std::atomic<float> previewGain{0.8f};

    // Audio thread -> API.
    std::atomic<bool> playing{false};
    std::atomic<int64_t> positionSamples{0};
    std::atomic<double> positionBeats{0.0};
    std::atomic<float> masterPeakLeft{0.f};
    std::atomic<float> masterPeakRight{0.f};
    std::atomic<bool> previewActive{false};
    std::atomic<float> cpuLoad{0.f};
};

}  // namespace gil
