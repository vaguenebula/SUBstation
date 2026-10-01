#pragma once
// State shared between the API threads and the audio thread.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "AudioSource.h"
#include "rt/RtUtils.h"

namespace gil {

struct TransportCommand {
    enum class Type : uint8_t { Play, Stop, Locate };
    Type type = Type::Stop;
    double beat = 0.0;  // Locate target
    double countInBeats = 0.0;  // Play: the metronome counts in this long before the playhead moves
};

// A note played by hand (e.g. clicking the piano roll), sent straight to a
// track's instrument. velocity 0 is a note-off.
struct PreviewNote {
    uint32_t trackId = 0;
    uint8_t key = 60;
    uint8_t velocity = 0;
};

struct SharedState {
    // API -> audio thread. Pushed only while holding the engine's edit mutex, so
    // there is exactly one producer.
    SpscQueue<TransportCommand, 256> commands;
    SpscQueue<PreviewNote, 256> previewNotes;
    std::atomic<bool> metronome{false};
    std::atomic<const AudioSource*> previewSource{nullptr};
    std::atomic<uint32_t> previewSerial{0};
    std::atomic<float> previewGain{0.8f};

    // Audio thread -> API.
    std::atomic<bool> playing{false};
    std::atomic<bool> countingIn{false};  // playing, but the count-in comes first
    std::atomic<int64_t> positionSamples{0};
    std::atomic<double> positionBeats{0.0};
    std::atomic<bool> previewActive{false};
    std::atomic<float> cpuLoad{0.f};
    // Peak level of each open input channel (beyond this many, none).
    static constexpr size_t kMaxInputMeters = 256;
    std::array<std::atomic<float>, kMaxInputMeters> inputPeaks{};

    // The latest master output (mono: the mean of left and right), for the
    // oscilloscope. A ring the audio thread overwrites; scopeWritten counts the
    // samples written so far. Readers may see a sample being overwritten, which
    // only matters to a picture.
    static constexpr size_t kScopeSize = 8192;
    std::array<std::atomic<float>, kScopeSize> scope{};
    std::atomic<uint64_t> scopeWritten{0};

    void pushScope(const float* left, const float* right, int frames) noexcept {
        uint64_t written = scopeWritten.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; ++i, ++written) {
            scope[written & (kScopeSize - 1)].store(0.5f * (left[i] + right[i]), std::memory_order_relaxed);
        }
        scopeWritten.store(written, std::memory_order_release);
    }
};

}  // namespace gil
