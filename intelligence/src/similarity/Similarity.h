// Comparing fingerprints: how far apart two sounds are.
//
// Features come in different units (dB, octaves, seconds on a log scale), so
// each is measured in spreads of the library it is searched in (z-scores),
// never dividing by less than the feature's minSpread. The library's
// statistics (each feature's spread) are measured robustly, its extreme 1% at
// either end held at the 1st and 99th percentiles (winsorized):
// a few broken or freakish files don't flatten the scale for everything else.
// The index keeps them with the fingerprints and saves them (SoundStore.h), so
// a search measures in the same scale whatever has been analysed since.
//
// A feature's squared difference is clipped at kClip (3 spreads: one wild
// feature, a pitch an octave off, can't outweigh all the others). Each aspect
// is the mean of its features' squared differences, and the distance is the
// aspects' weighted mean: so an aspect counts by its weight, however many
// features it has. Two unrelated sounds are about 2 apart (the expected squared
// difference of two independent z-scores); a sound and itself, 0.
//
// Level never counts (extractors measure relative to a sound's own peak), and
// length and duration are a feature or two of their aspects, clipped like the
// rest: they can't outweigh what a sound sounds like.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "similarity/FeatureSchema.h"

namespace sub::intelligence {

// Each feature's spread over a library's fingerprints. (Only differences are
// compared: where the features are centred doesn't count.)
struct FeatureStatistics {
    uint64_t count = 0;         // the fingerprints they describe
    std::vector<float> spread;  // the winsorized standard deviation, at least the feature's minSpread

    // Of these fingerprints (schema.dims() floats each, one after another;
    // `rows` picks some, or all if null). With fewer than two, the spreads
    // are the features' minSpreads.
    static FeatureStatistics measure(const FeatureSchema& schema, const float* fingerprints, size_t count,
                                     const std::vector<uint32_t>* rows = nullptr);
    bool fits(const FeatureSchema& schema) const { return spread.size() == schema.dims(); }
};

class Comparison {
public:
    static constexpr float kClip = 9.f;  // the most a feature's squared z-difference counts

    Comparison(const FeatureSchema& schema, const FeatureStatistics& statistics, const AspectWeights& weights);
    // With the schema's own weights.
    Comparison(const FeatureSchema& schema, const FeatureStatistics& statistics)
        : Comparison(schema, statistics, schema.weights) {}

    // Measures the statistics of these fingerprints and compares with them.
    static Comparison fit(const FeatureSchema& schema, const float* fingerprints, size_t count,
                          const std::vector<uint32_t>* rows = nullptr);
    static Comparison fit(const FeatureSchema& schema, const float* fingerprints, size_t count,
                          const std::vector<uint32_t>* rows, const AspectWeights& weights);

    // The weighted mean of the aspects' mean squared z-differences.
    float distance(const float* a, const float* b) const {
        float sum = 0.f;
        const size_t dims = share_.size();
        for (size_t d = 0; d < dims; ++d) {
            const float z = (a[d] - b[d]) * inverseSpread_[d];
            const float z2 = z * z;
            sum += share_[d] * (z2 < kClip ? z2 : kClip);
        }
        return sum;
    }

    // 1 for the same sound, falling towards 0 as they grow apart; about 0.37
    // for two unrelated sounds.
    static float similarity(float distance);

    size_t dims() const { return share_.size(); }
    const std::vector<float>& spread() const { return spread_; }

private:
    std::vector<float> spread_;
    std::vector<float> inverseSpread_;
    std::vector<float> share_;  // the aspect's weight / (its features * the total weight)
};

}  // namespace sub::intelligence
