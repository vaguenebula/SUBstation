#include "analysis/Spectrum.h"

#include "analysis/Fft.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace sub::app::analysis {

namespace {

// numpy.nan_to_num for float32.
float finite(float value) {
    if (std::isnan(value))
        return 0.0f;
    if (std::isinf(value))
        return value > 0 ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();
    return value;
}

// Shifts `buffer` left by the new samples' count and appends the latest of them (at most its size).
void append(std::vector<float>& buffer, const float* samples, std::size_t count) {
    const std::size_t size = buffer.size();
    if (count > size) {
        samples += count - size;
        count = size;
    }
    std::move(buffer.begin() + static_cast<std::ptrdiff_t>(count), buffer.end(), buffer.begin());
    for (std::size_t i = 0; i < count; ++i)
        buffer[size - count + i] = finite(samples[i]);
}

// |rfft(samples * window)| * scale, in dB with a floor of `minimum` (linear).
std::vector<double> spectrumDb(const std::vector<float>& samples, const std::vector<double>& window, double scale,
                               double minimum) {
    std::vector<double> windowed(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i)
        windowed[i] = double(samples[i]) * window[i];
    const std::vector<Complex> bins = rfft(windowed.data(), windowed.size(), windowed.size());
    std::vector<double> db(bins.size());
    for (std::size_t k = 0; k < bins.size(); ++k)
        db[k] = 20.0 * std::log10(std::max(std::abs(bins[k]) * scale, minimum));
    return db;
}

// numpy.interp(x, arange(n), values): between bins, held at the ends.
double atBin(const std::vector<float>& values, double x) {
    if (values.empty())
        return 0.0;
    if (x <= 0.0)
        return values.front();
    const double last = double(values.size() - 1);
    if (x >= last)
        return values.back();
    const std::size_t i = std::size_t(x);
    const double f = x - double(i);
    return values[i] + (values[i + 1] - values[i]) * f;
}

}  // namespace

// --- FallingSpectrum -----------------------------------------------------------------

FallingSpectrum::FallingSpectrum()
    : samples_(kFftSize, 0.0f), window_(hann(kFftSize)), spectrum_(kFftSize / 2 + 1, float(kFloorDb)) {
    for (double& w : window_)
        w = double(float(w));  // (as numpy's float32 window)
    scale_ = 2.0 / std::accumulate(window_.begin(), window_.end(), 0.0);
}

bool FallingSpectrum::add(const float* samples, std::size_t count, double sampleRate) {
    sampleRate_ = sampleRate;
    std::vector<float> latest(spectrum_.size(), float(kFloorDb));
    if (count > 0) {
        append(samples_, samples, count);
        const std::vector<double> db = spectrumDb(samples_, window_, scale_, 1e-9);
        for (std::size_t k = 0; k < latest.size(); ++k)
            latest[k] = float(db[k]);
    }
    bool changed = false;
    for (std::size_t k = 0; k < spectrum_.size(); ++k) {
        const float falling = std::max(spectrum_[k] - float(kFallDb), float(kFloorDb));
        const float value = std::max(latest[k], falling);
        if (value != spectrum_[k]) {
            spectrum_[k] = value;
            changed = true;
        }
    }
    return changed;
}

std::vector<double> FallingSpectrum::columns(int columns, double low, double high) const {
    const double hzPerBin = sampleRate_ / kFftSize;
    const long long lastBin = static_cast<long long>(spectrum_.size()) - 1;
    std::vector<double> edges(std::size_t(std::max(columns, 0)) + 1);
    for (int i = 0; i <= columns; ++i)
        edges[std::size_t(i)] = low * std::pow(high / low, double(i) / columns) / hzPerBin;  // in bins
    std::vector<double> values(std::size_t(std::max(columns, 0)));
    for (int i = 0; i < columns; ++i) {
        const double a = edges[std::size_t(i)], b = edges[std::size_t(i) + 1];
        double value = atBin(spectrum_, std::sqrt(a * b));
        const long long first = std::clamp(static_cast<long long>(std::ceil(a)), 0LL, lastBin);
        const long long last = std::clamp(static_cast<long long>(std::floor(b)), 0LL, lastBin);
        for (long long k = first; k <= last; ++k)
            value = std::max(value, double(spectrum_[std::size_t(k)]));
        values[std::size_t(i)] = value;
    }
    return values;
}

// --- EqAnalyzer ------------------------------------------------------------------------

EqAnalyzer::EqAnalyzer() : window_(hann(kFftSize)) {
    for (double& w : window_)
        w = double(float(w));
    scale_ = 2.0 / std::accumulate(window_.begin(), window_.end(), 0.0);
    for (int c = 0; c < 2; ++c) {
        samples_[c].assign(kFftSize, 0.0f);
        levels_[c].assign(kFftSize / 2 + 1, float(kFloorDb));
    }
}

void EqAnalyzer::feed(Channel channel, const float* samples, std::size_t count, double sampleRate) {
    sampleRate_ = sampleRate;
    std::vector<float>& levels = levels_[channel];
    std::vector<double> latest(levels.size(), kFloorDb);
    if (count > 0) {
        append(samples_[channel], samples, count);
        latest = spectrumDb(samples_[channel], window_, scale_, 1e-7);
    }
    for (std::size_t k = 0; k < levels.size(); ++k) {
        const double level = levels[k];
        const double rate = latest[k] > level ? kRise : kFall;
        levels[k] = float(std::max(level + (latest[k] - level) * rate, kFloorDb));
    }
}

bool EqAnalyzer::live(Channel channel) const {
    const std::vector<float>& levels = levels_[channel];
    return !levels.empty() && *std::max_element(levels.begin(), levels.end()) > kFloorDb + 1.0;
}

std::vector<double> EqAnalyzer::columns(Channel channel, const std::vector<double>& freqs) const {
    const std::vector<float>& levels = levels_[channel];
    const std::size_t n = freqs.size();
    if (n < 2)
        return std::vector<double>(n, kFloorDb);
    const double hzPerBin = sampleRate_ / kFftSize;
    std::vector<double> bins(n);
    for (std::size_t i = 0; i < n; ++i)
        bins[i] = freqs[i] / hzPerBin;
    // Each column's span in bins: halfway (in log) to its neighbours.
    std::vector<double> edges(n + 1);
    for (std::size_t i = 1; i < n; ++i)
        edges[i] = std::sqrt(bins[i - 1] * bins[i]);
    edges[0] = bins[0] * bins[0] / edges[1];
    edges[n] = bins[n - 1] * bins[n - 1] / edges[n - 1];
    const long long lastBin = kFftSize / 2;
    std::vector<long long> first(n);
    std::vector<bool> hasBins(n);
    for (std::size_t i = 0; i < n; ++i) {
        first[i] = std::clamp(static_cast<long long>(std::ceil(edges[i])), 0LL, lastBin);
        const long long last = std::clamp(static_cast<long long>(std::floor(edges[i + 1])), 0LL, lastBin);
        hasBins[i] = last >= first[i];
    }
    std::vector<double> values(n);
    for (std::size_t i = 0; i < n; ++i) {
        double value = atBin(levels, bins[i]);
        if (hasBins[i]) {
            // numpy.maximum.reduceat(levels, first): from this column's first bin to the next's
            // (the last column's to the end); just its first where the next starts no later.
            const long long from = first[i];
            const long long to = i + 1 < n ? first[i + 1] : static_cast<long long>(levels.size());
            double peak = levels[std::size_t(from)];
            for (long long k = from + 1; k < to; ++k)
                peak = std::max(peak, double(levels[std::size_t(k)]));
            value = std::max(value, peak);
        }
        values[i] = value;
    }
    // Smoothed (0.25, 0.5, 0.25), the ends held; then tilted above the floor.
    std::vector<double> result(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double before = values[i == 0 ? 0 : i - 1], after = values[i + 1 < n ? i + 1 : n - 1];
        const double value = 0.25 * before + 0.5 * values[i] + 0.25 * after;
        const double tilt = kTilt * std::log2(std::max(freqs[i], 1.0) / 1000.0);
        result[i] = value + tilt * std::clamp((value - kFloorDb) / kTiltFade, 0.0, 1.0);
    }
    return result;
}

}  // namespace sub::app::analysis
