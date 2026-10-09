#include "Warp.h"

#include <algorithm>
#include <cmath>

#include "Snapshot.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "signalsmith-stretch.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace sub {
namespace {

// Shorter blocks keep drum transients tight; longer ones give smoother tones
// and textures at the cost of smearing attacks.
constexpr std::array<StretchTiming, kNumStretchConfigs> kTimings = {{
    {0.080, 0.020},  // Transient (Transients mode)
    {0.120, 0.030},  // Standard (Standard and Formants modes): Signalsmith's default preset
    {0.200, 0.050},  // Smooth (Smooth mode)
}};

// A clip's source as the stretcher's input: `reader[channel][i]` is source frame
// `start + i`, with silence outside the file. Mono sources feed both channels.
struct SourceInput {
    const AudioSource* source;
    int64_t start;

    struct Channel {
        const float* data;
        int64_t frames;
        int64_t start;
        float operator[](int i) const noexcept {
            const int64_t k = start + i;
            return k >= 0 && k < frames ? data[k] : 0.f;
        }
    };
    Channel operator[](int c) const noexcept {
        const uint32_t channel = source->channels() > 1 ? static_cast<uint32_t>(c) : 0u;
        return {source->channelData(channel), source->frames(), start};
    }
};

// Windowed-sinc (Lanczos) kernel, tabulated. kHalfWidth zero crossings each
// side; linear interpolation between kResolution points per zero crossing.
constexpr int kHalfWidth = 8;
constexpr int kResolution = 512;
constexpr double kMaxRateFiltered = 4.0;  // beyond this, the kernel stops widening (cost cap)

struct SincTable {
    std::array<float, kHalfWidth * kResolution + 2> values{};
    SincTable() {
        constexpr double pi = 3.14159265358979323846;
        values[0] = 1.f;
        for (size_t i = 1; i < values.size(); ++i) {
            const double x = static_cast<double>(i) / kResolution;
            if (x >= kHalfWidth) {
                values[i] = 0.f;
                continue;
            }
            const double px = pi * x;
            values[i] = static_cast<float>(std::sin(px) / px * std::sin(px / kHalfWidth) / (px / kHalfWidth));
        }
    }
    float operator()(double x) const noexcept {  // x >= 0
        const double scaled = x * kResolution;
        const auto index = static_cast<size_t>(scaled);
        if (index + 1 >= values.size()) return 0.f;
        const auto frac = static_cast<float>(scaled - static_cast<double>(index));
        return values[index] + frac * (values[index + 1] - values[index]);
    }
};

// Built during static initialisation, never on the audio thread.
const SincTable kSinc;

}  // namespace

StretchTiming stretchTiming(StretchConfig config) noexcept { return kTimings[static_cast<size_t>(config)]; }

StretchConfig stretchConfigFor(WarpMode mode) noexcept {
    switch (mode) {
        case WarpMode::Transients: return StretchConfig::Transient;
        case WarpMode::Smooth: return StretchConfig::Smooth;
        default: return StretchConfig::Standard;
    }
}

// ---------------------------------------------------------------------------
// Stretch voices

struct WarpVoice::State {
    signalsmith::stretch::SignalsmithStretch<float> stretch{kStretchSeed};
    int64_t inputPos = 0;  // next source frame to feed
    double lead = 0.0;     // source frames between the fed input and the frame being output
    float transpose = 0.f;
    bool formants = false;
    bool primed = false;
};

WarpVoice::WarpVoice(StretchConfig config, double sampleRate)
    : config_(config), state_(std::make_unique<State>()) {
    const StretchTiming timing = stretchTiming(config);
    // Split computation spreads each spectral block's work over the following
    // interval instead of doing it all in one callback. It adds an interval of
    // latency, which the alignment below compensates like the rest.
    state_->stretch.configure(2, static_cast<int>(sampleRate * timing.blockSeconds),
                              static_cast<int>(sampleRate * timing.intervalSeconds), true);
}

WarpVoice::~WarpVoice() = default;

void WarpVoice::render(const ClipRender& clip, int64_t position, int frames, bool continuing, float* outL,
                       float* outR) noexcept {
    State& st = *state_;
    auto& stretch = st.stretch;

    if (!st.primed || clip.transpose != st.transpose || clip.preserveFormants != st.formants) {
        stretch.setTransposeSemitones(clip.transpose);
        stretch.setFormantFactor(1.f, clip.preserveFormants);  // compensatePitch: keep the formants in place
        st.transpose = clip.transpose;
        st.formants = clip.preserveFormants;
    }

    // Input runs ahead of the output by the stretcher's latency: inputLatency()
    // source frames, plus outputLatency() timeline frames' worth at this rate.
    const double rate = clip.rate;
    const double lead = stretch.inputLatency() + rate * stretch.outputLatency();
    const double wanted = clip.sourceAt(position);
    const double playing = static_cast<double>(st.inputPos) - st.lead;  // where the output is now
    // A tempo change rescales timeline positions but keeps the source position
    // (to within a frame or two), so it carries on without a re-seek.
    const double tolerance = 2.0 + 2.0 * rate;
    if (!continuing || !st.primed || std::abs(wanted - playing) > tolerance) {
        // outputSeek() pre-computes the pre-roll so the very next output frame is
        // source frame `from`: sample-accurate starts, locates and loop jumps.
        const auto from = static_cast<int64_t>(std::floor(wanted));
        const int seekLength = stretch.outputSeekLength(static_cast<float>(rate));
        SourceInput seekInput{clip.source.get(), from};
        stretch.outputSeek(seekInput, seekLength);
        st.inputPos = from + seekLength;
        st.primed = true;
    }
    st.lead = lead;

    // Feed exactly enough input to reach the source position the block should
    // end on; rounding never accumulates because the target is absolute.
    const int64_t target = std::llround(clip.sourceAt(position + frames) + lead);
    const auto count = static_cast<int>(std::max<int64_t>(0, target - st.inputPos));
    SourceInput input{clip.source.get(), st.inputPos};
    float* outputs[2] = {outL, outR};
    stretch.process(input, count, outputs, frames);
    st.inputPos += count;
}

// ---------------------------------------------------------------------------
// Re-Pitch

void renderResampled(const ClipRender& clip, int64_t position, int frames, float* outL, float* outR) noexcept {
    const AudioSource& source = *clip.source;
    const float* srcL = source.channelData(0);
    const float* srcR = source.channels() > 1 ? source.channelData(1) : srcL;
    const int64_t total = source.frames();

    // Speeding up moves content above the output Nyquist: lower the cutoff (and
    // widen the kernel) by the rate so it is filtered out instead of aliasing.
    const double cutoff = 1.0 / std::clamp(clip.rate, 1.0, kMaxRateFiltered);
    const double reach = kHalfWidth / cutoff;
    const auto gainScale = static_cast<float>(cutoff);

    for (int i = 0; i < frames; ++i) {
        const double pos = clip.sourceAt(position + i);
        const auto first = std::max<int64_t>(0, static_cast<int64_t>(std::floor(pos - reach)) + 1);
        const auto last = std::min<int64_t>(total - 1, static_cast<int64_t>(std::floor(pos + reach)));
        float l = 0.f, r = 0.f;
        for (int64_t k = first; k <= last; ++k) {
            const float w = kSinc(std::abs(pos - static_cast<double>(k)) * cutoff);
            l += srcL[k] * w;
            r += srcR[k] * w;
        }
        outL[i] = l * gainScale;
        outR[i] = r * gainScale;
    }
}

}  // namespace sub
