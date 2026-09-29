#pragma once
// Turns a RenderSnapshot into audio. One instance is driven by the device
// callback (live); export and tests use a separate instance (offline).

#include <array>
#include <cstdint>
#include <vector>

#include "Metronome.h"
#include "Snapshot.h"
#include "Transport.h"

namespace gil {

class Renderer {
public:
    static constexpr int kMaxBlock = 1024;

    // Non-real-time: allocates buffers. Keeps the transport position.
    void prepare(double sampleRate);

    // Real-time. Applies transport commands, renders tracks, metronome and the
    // browser preview into interleaved output, and publishes position/meters.
    void processLive(const RenderSnapshot& snap, SharedState& shared, float* out, uint32_t frames,
                     uint32_t channels) noexcept;

    // Renders the timeline from the current position into interleaved stereo.
    // Never publishes meters or plays the preview; looping and the metronome
    // are opt-in (exports leave them off).
    void renderOffline(const RenderSnapshot& snap, SharedState& shared, float* outStereo, int64_t frames,
                       bool loop = false, bool metronome = false) noexcept;

    // Transport state. Only one thread drives a renderer at a time: the audio
    // thread while a device runs, otherwise the engine under its edit mutex.
    void syncTempo(const RenderSnapshot& snap) noexcept;
    void drainCommands(SharedState& shared) noexcept;
    void applyCommand(const TransportCommand& command) noexcept;
    void publishTransport(SharedState& shared) const noexcept;

    void setPosition(int64_t samples) noexcept { position_ = samples < 0 ? 0 : samples; }
    int64_t position() const noexcept { return position_; }
    void setPlaying(bool playing) noexcept { playing_ = playing; }
    bool playing() const noexcept { return playing_; }

private:
    struct ChunkFlags {
        bool live;  // smooth parameter changes and publish meters
        bool loop;
        bool metronome;
    };

    struct Segment {
        int64_t position;
        int length;
        int offset;
    };
    struct Tick {
        int offset;
        bool accent;
    };
    static constexpr int kMaxSegments = 16;
    static constexpr int kMaxTicks = 64;

    void renderChunk(const RenderSnapshot& snap, SharedState& shared, int frames, ChunkFlags flags) noexcept;
    void renderClips(const TrackRender& track, const Segment& segment, int64_t clipFade) noexcept;
    void scheduleTicks(const RenderSnapshot& snap, int64_t position, int length, int offset) noexcept;
    void mixPreview(SharedState& shared, int frames) noexcept;

    double sampleRate_ = 48000.0;
    double samplesPerBeat_ = 0.0;
    int64_t position_ = 0;
    bool playing_ = false;

    std::vector<float> trackLeft_, trackRight_, masterLeft_, masterRight_;
    std::array<Segment, kMaxSegments> segments_{};
    int numSegments_ = 0;
    std::array<Tick, kMaxTicks> ticks_{};
    int numTicks_ = 0;

    Metronome metronome_;
    SmoothedValue masterGain_;
    bool masterNeedsSnap_ = true;

    uint32_t previewSerial_ = 0;
    const AudioSource* previewSource_ = nullptr;
    int64_t previewPosition_ = 0;
};

}  // namespace gil
