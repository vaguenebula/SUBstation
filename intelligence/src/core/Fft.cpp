#include "core/Fft.h"

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace sub::intelligence {

size_t Fft::nextPowerOfTwo(size_t n) {
    size_t p = 2;
    while (p < n) p <<= 1;
    return p;
}

Fft::Fft(size_t size) : n_(size) {
    if (!isPowerOfTwo(size)) throw std::invalid_argument("Fft: the size must be a power of two");
    twiddles_.resize(n_ / 2);
    for (size_t k = 0; k < n_ / 2; ++k) {
        const double angle = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n_);
        twiddles_[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
    reversed_.resize(n_);
    int bits = 0;
    while ((size_t{1} << bits) < n_) ++bits;
    for (size_t i = 0; i < n_; ++i) {
        uint32_t r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (size_t{1} << b)) r |= 1u << (bits - 1 - b);
        reversed_[i] = r;
    }
    if (n_ >= 4) {
        half_ = std::make_unique<Fft>(n_ / 2);
        realTwiddles_.resize(n_ / 2 + 1);
        for (size_t k = 0; k <= n_ / 2; ++k) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n_);
            realTwiddles_[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
        }
        scratch_.resize(n_ / 2);
    }
}

Fft::~Fft() = default;

void Fft::complex(std::complex<float>* data, bool inverse) const {
    for (size_t i = 0; i < n_; ++i)
        if (i < reversed_[i]) std::swap(data[i], data[reversed_[i]]);
    for (size_t len = 2; len <= n_; len <<= 1) {
        const size_t halfLen = len / 2;
        const size_t step = n_ / len;
        for (size_t start = 0; start < n_; start += len) {
            for (size_t j = 0; j < halfLen; ++j) {
                std::complex<float> w = twiddles_[j * step];
                if (inverse) w = std::conj(w);
                const std::complex<float> a = data[start + j];
                const std::complex<float> b = data[start + j + halfLen] * w;
                data[start + j] = a + b;
                data[start + j + halfLen] = a - b;
            }
        }
    }
}

void Fft::real(const float* in, std::complex<float>* out) const {
    if (n_ == 2) {
        out[0] = {in[0] + in[1], 0.f};
        out[1] = {in[0] - in[1], 0.f};
        return;
    }
    // Even samples as the real part, odd ones as the imaginary part: one
    // transform of half the size, then the two halves taken apart.
    const size_t m = n_ / 2;
    for (size_t i = 0; i < m; ++i) scratch_[i] = {in[2 * i], in[2 * i + 1]};
    half_->complex(scratch_.data());
    for (size_t k = 0; k <= m; ++k) {
        const std::complex<float> zk = scratch_[k % m];
        const std::complex<float> zc = std::conj(scratch_[(m - k) % m]);
        const std::complex<float> even = 0.5f * (zk + zc);
        const std::complex<float> odd = std::complex<float>(0.f, -0.5f) * (zk - zc);
        out[k] = even + realTwiddles_[k] * odd;
    }
}

}  // namespace sub::intelligence
