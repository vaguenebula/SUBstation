// Comparing fingerprints: how far apart two sounds are.
//
// Features come in different units (dB, octaves, seconds on a log scale), so
// each is measured in standard deviations of the library it is searched in
// (z-scores), never dividing by less than the feature's minSpread. A feature's
// squared difference is clipped at kClip (3 standard deviations: one wild
// feature, a pitch an octave off, can't outweigh all the others). Each aspect
// is the mean of its features' squared differences, and the distance is the
// aspects' weighted mean: so an aspect counts by its weight, however many
// features it has. Two unrelated sounds are about 2 apart (the expected squared
// difference of two independent z-scores); a sound and itself, 0.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "similarity/SoundFeatures.h"

namespace sub::intelligence {

// How much each aspect counts (relative to the others; 0 leaves it out).
struct AspectWeights {
    std::array<float, kAspects> weight = defaults();

    static std::array<float, kAspects> defaults();
    float& operator[](Aspect aspect) { return weight[static_cast<size_t>(aspect)]; }
    float operator[](Aspect aspect) const { return weight[static_cast<size_t>(aspect)]; }
};

class Comparison {
public:
    static constexpr float kClip = 9.f;  // the most a feature's squared z-difference counts

    // Measures with the spread of these fingerprints (kDims floats each, one
    // after another; `rows` picks some, or all if null). With fewer than two,
    // the features' minSpreads are the spreads.
    static Comparison fit(const float* fingerprints, size_t count, const std::vector<uint32_t>* rows = nullptr,
                          const AspectWeights& weights = {});

    // The weighted mean of the aspects' mean squared z-differences.
    float distance(const float* a, const float* b) const {
        float sum = 0.f;
        for (size_t d = 0; d < kDims; ++d) {
            const float z = (a[d] - b[d]) * inverseSpread_[d];
            const float z2 = z * z;
            sum += share_[d] * (z2 < kClip ? z2 : kClip);
        }
        return sum;
    }

    // 1 for the same sound, falling towards 0 as they grow apart; about 0.37
    // for two unrelated sounds.
    static float similarity(float distance);

    const std::array<float, kDims>& mean() const { return mean_; }
    const std::array<float, kDims>& spread() const { return spread_; }

private:
    std::array<float, kDims> mean_{};
    std::array<float, kDims> spread_{};
    std::array<float, kDims> inverseSpread_{};
    std::array<float, kDims> share_{};  // the aspect's weight / (its features * the total weight)
};

}  // namespace sub::intelligence
