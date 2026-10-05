#include "analysis/Fft.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sub::app::analysis {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

bool isPowerOfTwo(std::size_t n) { return n > 0 && (n & (n - 1)) == 0; }

void fft(std::vector<Complex>& data, bool inverse) {
    const std::size_t n = data.size();
    if (n < 2 || !isPowerOfTwo(n))
        return;
    // Bit-reversed order, then butterflies of growing span.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(data[i], data[j]);
    }
    // The twiddles for the largest span, each from cos and sin (no drift from multiplying them up).
    const double sign = inverse ? 1.0 : -1.0;
    std::vector<Complex> twiddles(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k) {
        const double angle = sign * 2.0 * kPi * double(k) / double(n);
        twiddles[k] = Complex(std::cos(angle), std::sin(angle));
    }
    for (std::size_t span = 2; span <= n; span <<= 1) {
        const std::size_t half = span / 2, stride = n / span;
        for (std::size_t start = 0; start < n; start += span) {
            for (std::size_t k = 0; k < half; ++k) {
                const Complex t = twiddles[k * stride] * data[start + k + half];
                data[start + k + half] = data[start + k] - t;
                data[start + k] += t;
            }
        }
    }
    if (inverse) {
        const double scale = 1.0 / double(n);
        for (Complex& value : data)
            value *= scale;
    }
}

std::vector<Complex> rfft(const double* x, std::size_t count, std::size_t n) {
    std::vector<Complex> data(n);
    const std::size_t used = std::min(count, n);
    for (std::size_t i = 0; i < used; ++i)
        data[i] = Complex(x[i], 0.0);
    fft(data);
    data.resize(n / 2 + 1);
    return data;
}

std::vector<double> hann(std::size_t n) {
    if (n == 0)
        return {};
    if (n == 1)
        return {1.0};
    std::vector<double> window(n);
    for (std::size_t i = 0; i < n; ++i)
        window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(n - 1));
    return window;
}

}  // namespace sub::app::analysis
