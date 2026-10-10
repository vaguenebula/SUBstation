#pragma once
// Multiband Dynamics' gain computer, after Ableton's: each of the three bands
// has two thresholds, Above and Below, and a ratio for each. Past either
// threshold the distance from it is divided by the ratio R (out = T + (L - T) / R
// in that region), so R > 1 narrows that region's dynamic range (Above:
// downward compression; Below: upward compression) and R < 1 widens it (Above:
// upward expansion; Below: downward expansion). 1:1 does nothing.
//
// - A side's gain change is s (L - T) past its threshold, on either side (Above:
//   L over T; Below: L under T), s = 1 / R - 1: from -0.99 at 100:1 through 0
//   at 1:1 to +3 at 1:4. So below the Below threshold, R > 1 lifts the level.
// - Soft Knee bends it in over kKneeDb (6 dB) around the threshold, quadratically
//   (the Compressor's shape).
// - Upward compression (Below, R > 1) fades out between -72 and -96 dB
//   (kUpwardFloorDb, kUpwardFadeDb), so silence, and whatever lies under
//   -96 dB, isn't lifted. A noise floor above that is, as Over The Top's is
//   (at -80 dB under a Below of -40 at 4:1, by 20 dB).
// - Each side's change is held to -96..+30 dB (kMaxCutDb, kMaxBoostDb), and so
//   is the band's (Amount times the sum of its sides): a gate-like expansion
//   lets go from at most 96 dB down.
//
// Shared by the device (builtin/devices/Multiband.cpp), which runs these per
// sample on its detectors' levels, and its editor (through the application
// layer's multibandGainDb()), so the curve and readouts drawn are the sound.

#include <algorithm>
#include <cmath>

namespace sub::multiband {

enum Band { Low = 0, Mid = 1, High = 2 };
inline constexpr int kBands = 3;

inline constexpr float kMinThresholdDb = -80.f, kMaxThresholdDb = 0.f;
inline constexpr float kMinRatio = 0.25f, kMaxRatio = 100.f;
inline constexpr float kKneeDb = 6.f;  // Soft Knee's width
inline constexpr float kUpwardFloorDb = -96.f, kUpwardFadeDb = 24.f;
inline constexpr float kMaxBoostDb = 30.f, kMaxCutDb = -96.f;
inline constexpr float kDisplayFloorDb = -90.f;  // the displays' levels' floor
// The detectors' windows (seconds): a band's lowest period, within these; RMS at least kMinRmsSeconds.
inline constexpr double kMaxWindowSeconds = 0.025, kMinWindowSeconds = 0.001, kMinRmsSeconds = 0.010;

// A ratio's slope less one: the gain change per dB past the threshold.
inline float slope(float ratio) noexcept { return 1.f / std::clamp(ratio, kMinRatio, kMaxRatio) - 1.f; }

// How much of an upward gain a level gets: all of it above -72 dB, none at -96.
inline float upwardFade(float levelDb) noexcept {
    return std::clamp((levelDb - kUpwardFloorDb) / kUpwardFadeDb, 0.f, 1.f);
}

// The Above side's gain change (dB) at `levelDb`, its knee `kneeDb` wide (0: hard).
inline float aboveGainDb(float levelDb, float thresholdDb, float slope, float kneeDb) noexcept {
    const float over = levelDb - thresholdDb;
    float gain;
    if (kneeDb > 0.f && std::abs(over) < 0.5f * kneeDb) {
        const float x = over + 0.5f * kneeDb;
        gain = slope * x * x / (2.f * kneeDb);
    } else {
        gain = over > 0.f ? slope * over : 0.f;
    }
    return std::clamp(gain, kMaxCutDb, kMaxBoostDb);
}

// The Below side's gain change (dB): the same law mirrored, upward gain faded towards silence.
inline float belowGainDb(float levelDb, float thresholdDb, float slope, float kneeDb) noexcept {
    const float under = thresholdDb - levelDb;
    float gain;
    if (kneeDb > 0.f && std::abs(under) < 0.5f * kneeDb) {
        const float x = under + 0.5f * kneeDb;
        gain = -slope * x * x / (2.f * kneeDb);
    } else {
        gain = under > 0.f ? -slope * under : 0.f;
    }
    if (gain > 0.f) gain *= upwardFade(levelDb);
    return std::clamp(gain, kMaxCutDb, kMaxBoostDb);
}

// The band's gain change from its two sides' (smoothed) changes, Amount (0..1) applied.
inline float bandGainDb(float aboveDb, float belowDb, float amount) noexcept {
    return std::clamp(amount * (aboveDb + belowDb), kMaxCutDb, kMaxBoostDb);
}

// The static curve: the gain change (dB) a steady level `levelDb` gets once attack
// and release have settled. Ratios as the parameters hold them, Amount 0..1.
inline float staticGainDb(float levelDb, float above, float aboveRatio, float below, float belowRatio, bool softKnee,
                          float amount) noexcept {
    const float knee = softKnee ? kKneeDb : 0.f;
    return bandGainDb(aboveGainDb(levelDb, above, slope(aboveRatio), knee),
                      belowGainDb(levelDb, below, slope(belowRatio), knee), amount);
}

// A band's detector window (seconds): long enough to hold a steady tone's peak
// (a rectified sine peaks every half period, so the lowest period the band
// holds), within kMinWindowSeconds..kMaxWindowSeconds. The low band reaches
// down to 20 Hz, and so does the mid band's detector while the low band is
// merged into it (switched off, or on its way). The Low-Mid crossover takes
// effect at most at the Mid-High one.
inline double windowSeconds(int band, double lowCrossover, double highCrossover, bool lowMerged) noexcept {
    switch (band) {
    case Low: return kMaxWindowSeconds;
    case Mid:
        return lowMerged
                   ? kMaxWindowSeconds
                   : std::clamp(1.0 / std::min(lowCrossover, highCrossover), kMinWindowSeconds, kMaxWindowSeconds);
    default: return std::clamp(1.0 / highCrossover, kMinWindowSeconds, kMaxWindowSeconds);
    }
}

}  // namespace sub::multiband
