#include "similarity/Similarity.h"

#include <algorithm>
#include <cmath>

namespace sub::intelligence {

std::array<float, kAspects> AspectWeights::defaults() {
    // Timbre, TimbreMotion, Spectrum, Envelope, Pitch, Rhythm: tuned on a
    // 5000-file sample library for how often a one-shot's nearest sounds are
    // of its kind (benchmarks/README.md, sound_similarity_bench).
    return {0.5f, 1.5f, 2.5f, 2.0f, 0.5f, 0.5f};
}

Comparison Comparison::fit(const float* fingerprints, size_t count, const std::vector<uint32_t>* rows,
                           const AspectWeights& weights) {
    Comparison c;
    const auto& info = featureInfo();
    const size_t n = rows ? rows->size() : count;
    std::array<double, kDims> sum{}, sumSquares{};
    for (size_t i = 0; i < n; ++i) {
        const float* f = fingerprints + static_cast<size_t>(rows ? (*rows)[i] : i) * kDims;
        for (size_t d = 0; d < kDims; ++d) {
            sum[d] += f[d];
            sumSquares[d] += static_cast<double>(f[d]) * f[d];
        }
    }
    std::array<int, kAspects> features{};
    for (size_t d = 0; d < kDims; ++d) ++features[static_cast<size_t>(info[d].aspect)];
    float total = 0.f;
    for (size_t a = 0; a < kAspects; ++a)
        if (features[a]) total += std::max(0.f, weights.weight[a]);
    for (size_t d = 0; d < kDims; ++d) {
        double spread = 0.0;
        if (n >= 2) {
            const double mean = sum[d] / static_cast<double>(n);
            c.mean_[d] = static_cast<float>(mean);
            spread = std::sqrt(std::max(0.0, sumSquares[d] / static_cast<double>(n) - mean * mean));
        }
        c.spread_[d] = std::max(static_cast<float>(spread), info[d].minSpread);
        c.inverseSpread_[d] = 1.f / c.spread_[d];
        const auto a = static_cast<size_t>(info[d].aspect);
        c.share_[d] = total > 0.f ? std::max(0.f, weights.weight[a]) / (static_cast<float>(features[a]) * total) : 0.f;
    }
    return c;
}

float Comparison::similarity(float distance) { return std::exp(-0.5f * std::max(0.f, distance)); }

}  // namespace sub::intelligence
