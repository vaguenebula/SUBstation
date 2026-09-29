#pragma once
// Warping: playing a clip at another speed (to follow the project tempo) and/or
// another pitch.
//
//  * Re-Pitch clips are resampled, like a turntable: faster is higher.
//    Stateless, so it needs nothing but the source.
//  * Every other mode uses a phase-vocoder time stretcher (Signalsmith Stretch,
//    MIT), which keeps speed and pitch independent. Stretchers carry state from
//    block to block, so they live in WarpVoices: allocated on the edit side and
//    handed to the audio thread through the snapshot, like everything else.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace gil {

struct ClipRender;

// Warp modes; the order matches the UI list (model/project.py WARP_MODES).
//  Transients: short stretch blocks, tight attacks (drums).
//  Standard:   the stretcher's default blocks; good all-round.
//  Smooth:     long blocks: smooth tones, pads and noise, softer attacks.
//  Formants:   Standard, plus keeping the formants in place when transposing.
//  RePitch:    no stretching; resampled, so speed and pitch change together.
enum class WarpMode : uint8_t { Transients, Standard, Smooth, Formants, RePitch };

// Stretcher block sizes. Each warp mode (except Re-Pitch) maps to one, and
// voices are pooled per configuration because reconfiguring allocates.
enum class StretchConfig : uint8_t { Transient, Standard, Smooth };
inline constexpr int kNumStretchConfigs = 3;

StretchConfig stretchConfigFor(WarpMode mode) noexcept;

// One stretcher, bound to whichever clip is using it. Constructing it allocates;
// render() is real-time safe.
class WarpVoice {
public:
    WarpVoice(StretchConfig config, double sampleRate);
    ~WarpVoice();
    WarpVoice(const WarpVoice&) = delete;
    WarpVoice& operator=(const WarpVoice&) = delete;

    StretchConfig config() const noexcept { return config_; }

    // Writes (does not add) `frames` frames of `clip`, starting at timeline sample
    // `position`, to outL/outR. `continuing` says the voice played this clip in
    // the previous block; it then carries on seamlessly if the source position
    // still lines up (e.g. across a tempo change), and otherwise re-seeks so the
    // first frame is exactly aligned.
    void render(const ClipRender& clip, int64_t position, int frames, bool continuing, float* outL,
                float* outR) noexcept;

    // Bookkeeping for the renderer's voice allocation.
    uint64_t key = 0;       // ClipRender::key of the clip it last played
    uint64_t lastUsed = 0;  // renderer block counter

private:
    struct State;
    StretchConfig config_;
    std::unique_ptr<State> state_;
};

// The voices a renderer may use, per stretch configuration.
using WarpVoiceSet = std::array<std::vector<std::shared_ptr<WarpVoice>>, kNumStretchConfigs>;

// Re-Pitch playback: band-limited (windowed-sinc) resampling of the source.
// Writes (does not add) `frames` frames starting at timeline sample `position`.
void renderResampled(const ClipRender& clip, int64_t position, int frames, float* outL, float* outR) noexcept;

}  // namespace gil
