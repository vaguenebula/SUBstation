#pragma once
// The Fourier transform the editors' signal maths needs (numpy.fft's, as the
// Python had it): a radix-2 transform of a power-of-two size, in double, and
// the Hann window. Not real-time: it runs on the UI thread as editors read
// their displays.

#include <complex>
#include <cstddef>
#include <vector>

namespace sub::app::analysis {

using Complex = std::complex<double>;

// Whether `n` is a power of two (1 is).
bool isPowerOfTwo(std::size_t n);
// In place: data.size() must be a power of two. The forward transform is
// numpy.fft.fft's (e^-i, unscaled); the inverse numpy.fft.ifft's (e^+i, divided by n).
void fft(std::vector<Complex>& data, bool inverse = false);
// numpy.fft.rfft(x, n): the first n / 2 + 1 bins of the transform of `x` (its
// first `count` values) cut or zero-padded to `n`, a power of two.
std::vector<Complex> rfft(const double* x, std::size_t count, std::size_t n);
// numpy.hanning(n): 0.5 - 0.5 cos(2 pi i / (n - 1)), symmetric, zero at both ends.
std::vector<double> hann(std::size_t n);

}  // namespace sub::app::analysis
