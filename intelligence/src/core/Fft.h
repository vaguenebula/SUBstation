// A radix-2 FFT for analysing sounds (offline, on the module's own threads; not
// real time). Complex in place, and real input through a complex one of half
// the size. Tables are made once per size: keep an Fft and reuse it, one per
// thread (real() works in a scratch buffer of its own).

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sub::intelligence {

class Fft {
public:
    // `size`: a power of two, at least 2.
    explicit Fft(size_t size);
    ~Fft();
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;

    size_t size() const { return n_; }

    // In place over size() points: forward is sum x[t] e^(-2 pi i k t / n);
    // inverse uses e^(+...) and is not scaled (divide by size() for the round trip).
    void complex(std::complex<float>* data, bool inverse = false) const;

    // size() real samples to bins 0..size()/2 (size()/2 + 1 values).
    void real(const float* in, std::complex<float>* out) const;

    static bool isPowerOfTwo(size_t n) { return n >= 2 && (n & (n - 1)) == 0; }
    static size_t nextPowerOfTwo(size_t n);

private:
    size_t n_;
    std::vector<std::complex<float>> twiddles_;  // e^(-2 pi i k / n), k < n/2
    std::vector<uint32_t> reversed_;             // bit reversal of each index
    // For real(): the half-size transform and e^(-2 pi i k / n) for k <= n/2.
    std::unique_ptr<Fft> half_;
    std::vector<std::complex<float>> realTwiddles_;
    mutable std::vector<std::complex<float>> scratch_;
};

}  // namespace sub::intelligence
