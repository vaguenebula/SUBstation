#pragma once
// The Gate's maths its editor shares (app/src/audio/GateResponse.h): the floor,
// the gain for how far open it is, the lookahead's choices, the display's rate,
// the ranges the editor's graphs drag in, and the sidechain EQ's filters, so
// what the editor draws is what plays; and how a key EQ filter starts warm.

#include <algorithm>
#include <cmath>

#include "builtin/DspBlocks.h"

namespace sub::gate {

inline constexpr float kThresholdMinDb = -70.f, kThresholdMaxDb = 6.f;
inline constexpr float kReturnMaxDb = 24.f;      // (from 0)
inline constexpr float kFloorMinDb = -75.f;      // Floor at its bottom mutes (Live's "-inf dB")
inline constexpr int kDisplaySamples = 256;      // audio per display value
inline constexpr float kDisplayFloorDb = -90.f;  // the displays' levels go no lower
inline constexpr double kLookaheadMs[] = {0.0, 1.0, 10.0};
inline constexpr int kLookaheads = 3;
inline constexpr float kKeyFreqMin = 30.f, kKeyFreqMax = 15000.f;  // the key EQ's frequency range (Live's)
inline constexpr float kKeyQMin = 0.1f, kKeyQMax = 12.f;
inline constexpr float kKeyGainMinDb = -15.f, kKeyGainMaxDb = 15.f;
inline constexpr int kEqChunk = 32;  // frames per key EQ glide step
// A key EQ filter that starts (switched on, or a new type) is run first over the key's last moments, at most
// kWarmSeconds of them, kWarmPace frames of them per frame until it has caught up (so no one block pays for it all).
inline constexpr double kWarmSeconds = 0.1;
inline constexpr int kWarmPace = 16;

// The key EQ's types, in Live's order (its S/C EQ Type's values).
enum class KeyFilter { LowShelf = 0, Bell, HighShelf, LowPass, BandPass, HighPass };
inline constexpr int kKeyFilters = 6;

// The floor's gain: silence at the bottom of its range.
inline float floorGain(float floorDb) noexcept {
    return floorDb <= kFloorMinDb + 0.05f ? 0.f : std::pow(10.f, floorDb / 20.f);
}
// Lookahead choice `index` in samples.
inline int lookaheadSamples(int index, double sampleRate) noexcept {
    return static_cast<int>(std::lround(kLookaheadMs[std::clamp(index, 0, kLookaheads - 1)] * 0.001 * sampleRate));
}
// The level (dB) below which an open gate closes again: Return below the threshold.
inline float closeDb(float thresholdDb, float returnDb) noexcept { return thresholdDb - std::max(0.f, returnDb); }
// How much passes: the openness (0..1, moving linearly over the attack or release) eased in and out
// (dsp::sCurve, so the gain's ramps start and end without a corner), or its opposite while flipped
// (flip fades 0..1 between them).
inline float pass(float openness, float flip) noexcept {
    const float shaped = dsp::sCurve(openness);
    return shaped + flip * (1.f - 2.f * shaped);
}
// The gain: exactly 1 when all passes, the floor's when nothing does, linear in between.
inline float gain(float pass, float floorGain) noexcept { return 1.f - (1.f - floorGain) * (1.f - pass); }

inline bool keyFilterUsesGain(KeyFilter type) noexcept {
    return type == KeyFilter::LowShelf || type == KeyFilter::Bell || type == KeyFilter::HighShelf;
}
inline bool keyFilterUsesQ(KeyFilter type) noexcept {  // (the shelves keep a fixed slope)
    return type != KeyFilter::LowShelf && type != KeyFilter::HighShelf;
}
// The key EQ: RBJ biquads (dsp::BiquadCoefficients), the frequency held to
// 10 Hz .. 0.45 of the rate. The shelves have the cookbook's plain slope (Q
// 0.7071: above about 1 a shelf overshoots, below 0.3 it hardly shelves): Q is
// the bell's width and the pass filters' resonance.
inline dsp::BiquadCoefficients keyFilter(KeyFilter type, double freq, double q, double gainDb, double sampleRate) {
    const double f = std::clamp(freq, 10.0, 0.45 * sampleRate);
    switch (type) {
    case KeyFilter::LowShelf: return dsp::BiquadCoefficients::lowShelf(f, 0.7071, gainDb, sampleRate);
    case KeyFilter::Bell: return dsp::BiquadCoefficients::peak(f, q, gainDb, sampleRate);
    case KeyFilter::HighShelf: return dsp::BiquadCoefficients::highShelf(f, 0.7071, gainDb, sampleRate);
    case KeyFilter::LowPass: return dsp::BiquadCoefficients::lowpass(f, q, sampleRate);
    case KeyFilter::BandPass: return dsp::BiquadCoefficients::bandpass(f, q, sampleRate);
    case KeyFilter::HighPass: return dsp::BiquadCoefficients::highpass(f, q, sampleRate);
    }
    return {};
}
// Its response in dB at `at` Hz (Nyquist's above it).
inline double keyFilterDb(KeyFilter type, double freq, double q, double gainDb, double sampleRate, double at) {
    return keyFilter(type, freq, q, gainDb, sampleRate).magnitudeDb(std::min(at, 0.5 * sampleRate), sampleRate);
}

// How many frames of the key a filter that starts is run over first: as long as its slowest pole
// takes to fall 60 dB (so where it started from no longer shows), at most `most`.
inline int warmFrames(const dsp::BiquadCoefficients& c, int most) noexcept {
    const double disc = c.a1 * c.a1 - 4.0 * c.a2;
    const double radius = disc < 0.0 ? std::sqrt(c.a2) : 0.5 * (std::abs(c.a1) + std::sqrt(disc));
    if (!(radius > 0.0)) return 1;
    if (radius >= 1.0) return most;
    return std::clamp(static_cast<int>(std::ceil(std::log(1e-3) / std::log(radius))), 1, most);
}
// The frame (from 0, where it starts) on which a filter run over `frames` of the key first, kWarmPace a
// frame, has caught up: it is heard from there.
inline int caughtUpFrame(int frames) noexcept { return std::max(0, (frames + kWarmPace - 2) / (kWarmPace - 1) - 1); }

}  // namespace sub::gate
