#include "similarity/Similarity.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sub::intelligence {

namespace {

constexpr double kWinsorize = 0.01;  // held at the 1st and 99th percentiles

}  // namespace

FeatureStatistics FeatureStatistics::measure(const FeatureSchema& schema, const float* fingerprints, size_t count,
                                             const std::vector<uint32_t>* rows) {
    const size_t dims = schema.dims();
    const size_t n = rows ? rows->size() : count;
    FeatureStatistics s;
    s.count = n;
    s.center.assign(dims, 0.f);
    s.spread.assign(dims, 0.f);
    std::vector<float> values(n);
    for (size_t d = 0; d < dims; ++d) {
        double spread = 0.0;
        if (n >= 2) {
            for (size_t i = 0; i < n; ++i)
                values[i] = fingerprints[static_cast<size_t>(rows ? (*rows)[i] : i) * dims + d];
            // The percentiles the values are held to.
            const auto lowAt = static_cast<size_t>(kWinsorize * static_cast<double>(n - 1));
            const size_t highAt = n - 1 - lowAt;
            std::nth_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(lowAt), values.end());
            const float low = values[lowAt];
            std::nth_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(highAt), values.end());
            const float high = values[highAt];
            double sum = 0.0, sumSquares = 0.0;
            for (const float v : values) {
                const double x = std::clamp(v, low, high);
                sum += x;
                sumSquares += x * x;
            }
            const double mean = sum / static_cast<double>(n);
            s.center[d] = static_cast<float>(mean);
            spread = std::sqrt(std::max(0.0, sumSquares / static_cast<double>(n) - mean * mean));
        }
        s.spread[d] = std::max(static_cast<float>(spread), schema.features[d].minSpread);
    }
    return s;
}

Comparison::Comparison(const FeatureSchema& schema, const FeatureStatistics& statistics, const AspectWeights& weights) {
    const size_t dims = schema.dims();
    std::array<int, kAspects> features{};
    for (const FeatureInfo& f : schema.features) ++features[static_cast<size_t>(f.aspect)];
    float total = 0.f;
    for (size_t a = 0; a < kAspects; ++a)
        if (features[a]) total += std::max(0.f, weights.weight[a]);
    spread_.resize(dims);
    inverseSpread_.resize(dims);
    share_.resize(dims);
    const bool measured = statistics.fits(schema);
    for (size_t d = 0; d < dims; ++d) {
        const FeatureInfo& f = schema.features[d];
        spread_[d] = std::max(measured ? statistics.spread[d] : 0.f, f.minSpread);
        inverseSpread_[d] = 1.f / spread_[d];
        const auto a = static_cast<size_t>(f.aspect);
        share_[d] = total > 0.f ? std::max(0.f, weights.weight[a]) / (static_cast<float>(features[a]) * total) : 0.f;
    }
}

Comparison Comparison::fit(const FeatureSchema& schema, const float* fingerprints, size_t count,
                           const std::vector<uint32_t>* rows) {
    return fit(schema, fingerprints, count, rows, schema.weights);
}

Comparison Comparison::fit(const FeatureSchema& schema, const float* fingerprints, size_t count,
                           const std::vector<uint32_t>* rows, const AspectWeights& weights) {
    return Comparison(schema, FeatureStatistics::measure(schema, fingerprints, count, rows), weights);
}

float Comparison::similarity(float distance) { return std::exp(-0.5f * std::max(0.f, distance)); }

}  // namespace sub::intelligence
