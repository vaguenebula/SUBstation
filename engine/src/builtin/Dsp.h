#pragma once
// DSP the built-in devices share: a state-variable filter section, 4-point
// interpolation, a peak follower, flushing tiny values, an S-curve, the
// instruments' envelopes, and rendering a block between its note events. All
// inline: they run per sample. More (delay lines, LFOs, biquads, crossovers,
// glides, oversampling) is in DspBlocks.h.

#include <algorithm>
#include <cmath>

#include "Processor.h"

namespace sub::dsp {

// A recursive filter's state, zero once it is too small to hear, so silence
// never runs into denormals.
inline double flushTiny(double state) noexcept { return std::abs(state) < 1e-20 ? 0.0 : state; }
inline float flushTiny(float state) noexcept { return std::abs(state) < 1e-20f ? 0.f : state; }

// --- State-variable filter ----------------------------------------------------------------
// A TPT state-variable filter section (Zavalishin/Simper), stable at any cutoff
// and resonance. Its coefficients for g = tan(pi f / sr) and k = 1/Q (damping):
struct SvfCoefficients {
    float k = 1.f, a1 = 1.f, a2 = 0.f, a3 = 0.f;

    SvfCoefficients() = default;
    SvfCoefficients(float g, float damping) noexcept : k(damping) {
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};

// A section's state. tick() gives its band-pass (v1) and low-pass (v2)
// outputs; the others are made from them and the input x: high-pass
// x - k v1 - v2, band-pass at unity gain k v1, notch x - k v1, all-pass
// x - 2 k v1.
struct Svf {
    struct Outputs {
        float band;  // v1
        float low;   // v2
    };

    float ic1 = 0.f, ic2 = 0.f;

    void reset() noexcept { ic1 = ic2 = 0.f; }
    void flush() noexcept {
        ic1 = flushTiny(ic1);
        ic2 = flushTiny(ic2);
    }
    Outputs tick(const SvfCoefficients& c, float x) noexcept {
        const float v3 = x - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        return {v1, v2};
    }
};

// --- Small pieces -------------------------------------------------------------------------

// 4-point, 3rd-order Hermite interpolation between x0 and x1, `t` of the way.
inline float hermite(float xm1, float x0, float x1, float x2, float t) noexcept {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// An envelope that jumps up to a peak at once and falls back by `release` (a
// one-pole coefficient) per sample.
inline float followPeak(float envelope, float input, float release) noexcept {
    return input >= envelope ? input : input + release * (envelope - input);
}

// An S-curve from 0 at t = 0 to 1 at t = 1, flat at both ends (smoothstep, t² (3 - 2t)):
// crossfades and switches with no corner to click. `t` in 0..1.
inline float sCurve(float t) noexcept { return t * t * (3.f - 2.f * t); }
inline double sCurve(double t) noexcept { return t * t * (3.0 - 2.0 * t); }

// --- The instruments' envelopes -----------------------------------------------------------
// Times in ms, made into steps and coefficients per sample (`samplesPerMs`).

inline constexpr float kSilent = 1e-4f;  // -80 dB: a releasing voice ends here
inline constexpr double kFadeTo = 1e-3;  // decay and release times are to -60 dB

// A linear rise from 0 to 1 over `ms` (one sample at least): the step per sample.
inline float riseStep(float ms, double samplesPerMs) noexcept {
    return static_cast<float>(1.0 / std::max(1.0, ms * samplesPerMs));
}

// An exponential fall to kFadeTo over `ms` (one sample at least): the factor per sample.
inline float fallCoefficient(float ms, double samplesPerMs) noexcept {
    return static_cast<float>(std::exp(std::log(kFadeTo) / std::max(1.0, ms * samplesPerMs)));
}

// --- Notes ---------------------------------------------------------------------------------

// Renders a block in stretches between its events: render(from, to) up to each
// event, then noteOn(key, velocity) for a note that starts there or
// noteOff(key) for one that ends (ProcessEvent::startsNote/endsNote), then the
// rest. Every event splits the block, so one more kind (a pitch bend, a
// controller) needs only its own handler here.
template <typename Render, typename NoteOn, typename NoteOff>
void renderBetweenNotes(const EventList& events, int frames, Render&& render, NoteOn&& noteOn, NoteOff&& noteOff) {
    int position = 0;
    for (size_t e = 0; e < events.count; ++e) {
        const ProcessEvent& event = events.events[e];
        const int at = std::clamp(static_cast<int>(event.sampleOffset), position, frames);
        render(position, at);
        position = at;
        if (event.startsNote()) {
            noteOn(event.key(), event.velocity());
        } else if (event.endsNote()) {
            noteOff(event.key());
        }
    }
    render(position, frames);
}

}  // namespace sub::dsp
