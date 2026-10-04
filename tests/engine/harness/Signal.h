#pragma once
// What the Python tests did with numpy, for the engine's renders: picking
// channels and ranges out of interleaved audio, levels (peak, RMS), where a
// signal is non-zero, comparisons sample by sample (np.testing.assert_allclose,
// assert_array_equal), spectra (np.fft.rfft, for any length), envelopes,
// correlation, random numbers, and reading WAV files back.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "Test.h"

namespace subtest {

inline constexpr double kPi = 3.14159265358979323846;

using Samples = std::vector<float>;

// --- Picking out ----------------------------------------------------------------------

// Python's slice v[from:to]: negative indexes count from the end; out of range is clipped.
template <typename T>
std::vector<T> slice(const std::vector<T>& v, int64_t from, int64_t to = std::numeric_limits<int64_t>::max()) {
    const auto size = static_cast<int64_t>(v.size());
    if (from < 0) from += size;
    if (to < 0) to += size;
    from = std::clamp<int64_t>(from, 0, size);
    to = std::clamp<int64_t>(to, from, size);
    std::vector<T> out(static_cast<size_t>(to - from));
    std::copy(v.begin() + from, v.begin() + to, out.begin());
    return out;
}

// Channel `c` of interleaved audio (out[:, c]).
template <typename T>
std::vector<T> channel(const std::vector<T>& interleaved, int c, int channels = 2) {
    std::vector<T> out;
    out.reserve(interleaved.size() / static_cast<size_t>(channels));
    for (size_t i = static_cast<size_t>(c); i < interleaved.size(); i += static_cast<size_t>(channels))
        out.push_back(interleaved[i]);
    return out;
}

// Frames from..to of interleaved audio (out[from:to]), as Python slices them.
inline Samples frames(const Samples& interleaved, int64_t from, int64_t to = std::numeric_limits<int64_t>::max(),
                      int channels = 2) {
    const auto count = static_cast<int64_t>(interleaved.size()) / channels;
    if (from < 0) from += count;
    if (to < 0) to += count;
    from = std::clamp<int64_t>(from, 0, count);
    to = std::clamp<int64_t>(to, from, count);
    Samples out(static_cast<size_t>((to - from) * channels));
    std::copy(interleaved.begin() + from * channels, interleaved.begin() + to * channels, out.begin());
    return out;
}

// Sample `frame` of channel `c` of interleaved stereo (out[frame, c]); a negative frame counts from the end.
inline float at(const Samples& interleaved, int64_t frame, int c, int channels = 2) {
    if (frame < 0) frame += static_cast<int64_t>(interleaved.size()) / channels;
    return interleaved.at(static_cast<size_t>(frame * channels + c));
}

// --- Making -------------------------------------------------------------------------------

inline Samples full(size_t count, float value) { return Samples(count, value); }

// Interleaves channels of equal length (np.stack(..., axis=1)).
inline Samples interleave(const std::vector<Samples>& channels) {
    Samples out;
    if (channels.empty()) return out;
    out.reserve(channels[0].size() * channels.size());
    for (size_t i = 0; i < channels[0].size(); ++i)
        for (const Samples& c : channels) out.push_back(c[i]);
    return out;
}

inline Samples stereo(const Samples& mono) { return interleave({mono, mono}); }

// amplitude * sin(2 pi freq t), `seconds` long at `rate` (computed in double, as numpy does).
inline Samples sine(double freq, double seconds, double amplitude = 0.5, int rate = 48000) {
    Samples out(static_cast<size_t>(seconds * rate));
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
    return out;
}

// np.hanning(n).
inline std::vector<double> hanning(size_t n) {
    std::vector<double> w(n, 1.0);
    if (n < 2) return w;
    for (size_t i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
    return w;
}

// --- Levels and where things are ---------------------------------------------------------

template <typename T>
double maxAbs(const std::vector<T>& v) {
    double peak = 0.0;
    for (const T x : v) peak = std::max(peak, std::fabs(static_cast<double>(x)));
    return peak;
}

template <typename T>
double maxOf(const std::vector<T>& v) {
    double peak = -std::numeric_limits<double>::infinity();
    for (const T x : v) peak = std::max(peak, static_cast<double>(x));
    return peak;
}

template <typename T>
double mean(const std::vector<T>& v) {
    double sum = 0.0;
    for (const T x : v) sum += static_cast<double>(x);
    return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
}

template <typename T>
double rms(const std::vector<T>& v) {
    double sum = 0.0;
    for (const T x : v) sum += static_cast<double>(x) * static_cast<double>(x);
    return v.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(v.size()));
}

// Every value equals `value` (np.all(v == value)).
template <typename T>
bool allEqual(const std::vector<T>& v, double value) {
    return std::all_of(v.begin(), v.end(), [&](T x) { return static_cast<double>(x) == value; });
}

template <typename T>
bool anyNonzero(const std::vector<T>& v) {
    return std::any_of(v.begin(), v.end(), [](T x) { return x != T{}; });
}

template <typename T>
size_t countNonzero(const std::vector<T>& v) {
    return static_cast<size_t>(std::count_if(v.begin(), v.end(), [](T x) { return x != T{}; }));
}

template <typename T>
bool allFinite(const std::vector<T>& v) {
    // (The engine is built with fast math, which assumes no infinity or NaN; the tests aren't.)
    return std::all_of(v.begin(), v.end(), [](T x) { return std::isfinite(static_cast<double>(x)); });
}

// The indexes where |v| > threshold (np.nonzero(np.abs(v) > threshold)[0]).
template <typename T>
std::vector<int64_t> above(const std::vector<T>& v, double threshold) {
    std::vector<int64_t> out;
    for (size_t i = 0; i < v.size(); ++i)
        if (std::fabs(static_cast<double>(v[i])) > threshold) out.push_back(static_cast<int64_t>(i));
    return out;
}

// The indexes of the non-zero values (np.nonzero(v)[0]).
template <typename T>
std::vector<int64_t> nonzero(const std::vector<T>& v) {
    std::vector<int64_t> out;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] != T{}) out.push_back(static_cast<int64_t>(i));
    return out;
}

template <typename T>
size_t argmax(const std::vector<T>& v) {
    return static_cast<size_t>(std::max_element(v.begin(), v.end()) - v.begin());
}

// --- Comparing, as numpy's testing does ----------------------------------------------------

// What np.testing.assert_allclose(actual, desired, rtol, atol) would say: empty
// if |actual - desired| <= atol + rtol * |desired| everywhere.
template <typename A, typename D>
std::string allcloseFailure(const std::vector<A>& actual, const std::vector<D>& desired, double rtol, double atol) {
    if (actual.size() != desired.size())
        return "shapes differ: " + std::to_string(actual.size()) + " vs " + std::to_string(desired.size());
    size_t bad = 0, first = 0;
    double worst = 0.0;
    for (size_t i = 0; i < actual.size(); ++i) {
        const double a = static_cast<double>(actual[i]), d = static_cast<double>(desired[i]);
        const double error = std::fabs(a - d);
        if (!(error <= atol + rtol * std::fabs(d))) {
            if (bad++ == 0) first = i;
            worst = std::max(worst, error);
        }
    }
    if (!bad) return {};
    return std::to_string(bad) + " of " + std::to_string(actual.size()) + " differ, the first at " +
           std::to_string(first) + ": " + show(static_cast<double>(actual[first])) + " vs " +
           show(static_cast<double>(desired[first])) + " (largest difference " + show(worst) + ")";
}

template <typename A>
std::string allcloseFailure(const std::vector<A>& actual, double desired, double rtol, double atol) {
    return allcloseFailure(actual, std::vector<double>(actual.size(), desired), rtol, atol);
}

// np.allclose(a, b) (rtol 1e-5, atol 1e-8).
template <typename A, typename D>
bool allclose(const std::vector<A>& actual, const D& desired, double rtol = 1e-5, double atol = 1e-8) {
    return allcloseFailure(actual, desired, rtol, atol).empty();
}

}  // namespace subtest

// np.testing.assert_allclose: `desired` is a vector of the same length or one value.
#define CHECK_ALLCLOSE(actual, desired, rtol, atol)                                                 \
    do {                                                                                            \
        const std::string subtest_why = ::subtest::allcloseFailure((actual), (desired), rtol, atol); \
        if (!subtest_why.empty())                                                                   \
            ::subtest::fail(__FILE__, __LINE__, "CHECK_ALLCLOSE(" #actual ", " #desired "): " + subtest_why); \
    } while (0)

// np.testing.assert_array_equal.
#define CHECK_ARRAY_EQUAL(actual, desired) CHECK_ALLCLOSE(actual, desired, 0.0, 0.0)

namespace subtest {

// --- Clicks ----------------------------------------------------------------------------------

using Clicks = std::map<int64_t, double>;

// Where a channel isn't silent (|x| > threshold), and its values there rounded to 6 decimals.
template <typename T>
Clicks clicksOf(const std::vector<T>& channel, double threshold = 1e-7) {
    Clicks found;
    for (size_t i = 0; i < channel.size(); ++i) {
        if (std::fabs(static_cast<double>(channel[i])) > threshold)
            found[static_cast<int64_t>(i)] = std::round(static_cast<double>(channel[i]) * 1e6) / 1e6;
    }
    return found;
}

inline std::string showClicks(const Clicks& clicks) {
    std::string text = "{";
    for (const auto& [at, value] : clicks) {
        if (text.size() > 200) return text + ", ...}";
        if (text.size() > 1) text += ", ";
        text += std::to_string(at) + ": " + show(value);
    }
    return text + "}";
}

// The same places, and values as pytest.approx sees them (relative 1e-6).
inline bool sameClicks(const Clicks& got, const Clicks& want) {
    if (got.size() != want.size()) return false;
    for (auto g = got.begin(), w = want.begin(); g != got.end(); ++g, ++w) {
        if (g->first != w->first || !(std::fabs(g->second - w->second) <= approxTolerance(w->second, 1e-6, 1e-12)))
            return false;
    }
    return true;
}

}  // namespace subtest

// The clicks of a render are these: {{at, value}, ...}.
#define CHECK_CLICKS(got, ...)                                                                                 \
    do {                                                                                                       \
        const ::subtest::Clicks subtest_got = (got);                                                           \
        const ::subtest::Clicks subtest_want = __VA_ARGS__;                                                    \
        if (!::subtest::sameClicks(subtest_got, subtest_want))                                                 \
            ::subtest::fail(__FILE__, __LINE__, "CHECK_CLICKS(" #got "): " + ::subtest::showClicks(subtest_got) + \
                                                    " != " + ::subtest::showClicks(subtest_want));             \
    } while (0)

namespace subtest {

// --- Spectra -------------------------------------------------------------------------------

using Complex = std::complex<double>;

// In-place radix-2 FFT (size a power of two); unscaled either way.
inline void fftPow2(std::vector<Complex>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const double angle = 2.0 * kPi / static_cast<double>(length) * (inverse ? 1.0 : -1.0);
        for (size_t start = 0; start < n; start += length) {
            for (size_t k = 0; k < length / 2; ++k) {
                const Complex w = std::polar(1.0, angle * static_cast<double>(k));
                const Complex u = a[start + k], v = a[start + k + length / 2] * w;
                a[start + k] = u + v;
                a[start + k + length / 2] = u - v;
            }
        }
    }
}

// The DFT of any length (Bluestein's chirp z-transform where it isn't a power of two).
inline std::vector<Complex> dft(const std::vector<double>& x) {
    const size_t n = x.size();
    if (n == 0) return {};
    if ((n & (n - 1)) == 0) {
        std::vector<Complex> a(x.begin(), x.end());
        fftPow2(a, false);
        return a;
    }
    size_t m = 1;
    while (m < 2 * n - 1) m <<= 1;
    std::vector<Complex> chirp(n);
    for (size_t k = 0; k < n; ++k) {
        const auto k2 = static_cast<uint64_t>(k) * k % (2 * n);  // exact, so long transforms stay precise
        chirp[k] = std::polar(1.0, -kPi * static_cast<double>(k2) / static_cast<double>(n));
    }
    std::vector<Complex> a(m), b(m);
    for (size_t k = 0; k < n; ++k) a[k] = x[k] * chirp[k];
    b[0] = std::conj(chirp[0]);
    for (size_t k = 1; k < n; ++k) b[k] = b[m - k] = std::conj(chirp[k]);
    fftPow2(a, false);
    fftPow2(b, false);
    for (size_t k = 0; k < m; ++k) a[k] *= b[k];
    fftPow2(a, true);
    std::vector<Complex> out(n);
    for (size_t k = 0; k < n; ++k) out[k] = a[k] / static_cast<double>(m) * chirp[k];
    return out;
}

// |np.fft.rfft(x * window)|: bins 0..n/2. No window: none.
template <typename T>
std::vector<double> spectrum(const std::vector<T>& x, const std::vector<double>& window = {}) {
    std::vector<double> in(x.size());
    for (size_t i = 0; i < x.size(); ++i) in[i] = static_cast<double>(x[i]) * (window.empty() ? 1.0 : window[i]);
    const std::vector<Complex> bins = dft(in);
    std::vector<double> out(x.size() / 2 + 1);
    for (size_t k = 0; k < out.size(); ++k) out[k] = std::abs(bins[k]);
    return out;
}

// The strongest frequency, between bins: a Hann window, and a parabola through
// the log magnitudes around the peak.
template <typename T>
double dominantFreq(const std::vector<T>& samples, double sampleRate = 48000.0) {
    const std::vector<double> s = spectrum(samples, hanning(samples.size()));
    const size_t peak = argmax(s);
    if (peak == 0 || peak + 1 >= s.size()) return static_cast<double>(peak) * sampleRate / static_cast<double>(samples.size());
    const double a = std::log(s[peak - 1] + 1e-12), b = std::log(s[peak] + 1e-12), c = std::log(s[peak + 1] + 1e-12);
    const double offset = 0.5 * (a - c) / (a - 2 * b + c);
    return (static_cast<double>(peak) + offset) * sampleRate / static_cast<double>(samples.size());
}

// --- Shapes over time ------------------------------------------------------------------------

// sqrt(np.convolve(x**2, np.ones(width) / width, mode="same")): an RMS envelope.
template <typename T>
std::vector<double> envelope(const std::vector<T>& x, size_t width = 48) {
    const auto n = static_cast<int64_t>(x.size());
    const auto w = static_cast<int64_t>(width);
    const int64_t shift = (w - 1) / 2;  // "same": the middle of the full convolution
    std::vector<double> squares(x.size() + 1, 0.0);  // running sums
    for (int64_t i = 0; i < n; ++i)
        squares[static_cast<size_t>(i + 1)] = squares[static_cast<size_t>(i)] + static_cast<double>(x[i]) * x[i];
    std::vector<double> out(x.size());
    for (int64_t i = 0; i < n; ++i) {
        const int64_t last = std::min(n - 1, i + shift), first = std::max<int64_t>(0, i + shift - w + 1);
        const double sum = last >= first ? squares[static_cast<size_t>(last + 1)] - squares[static_cast<size_t>(first)] : 0.0;
        out[static_cast<size_t>(i)] = std::sqrt(std::max(0.0, sum / static_cast<double>(w)));
    }
    return out;
}

// Pearson's correlation (np.corrcoef(a, b)[0, 1]).
inline double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    const double ma = mean(a), mb = mean(b);
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
    }
    return ab / std::sqrt(aa * bb);
}

// --- Random numbers ---------------------------------------------------------------------------

// Seeded random numbers, the same on every platform (the standard's
// distributions differ between libraries, so they are made here). Not numpy's
// sequences: the tests only need some fixed noise and graphs.
class Rng {
public:
    explicit Rng(uint64_t seed) : engine_(seed) {}
    double random() { return static_cast<double>(engine_() >> 11) * 0x1.0p-53; }  // [0, 1)
    double uniform(double low, double high) { return low + (high - low) * random(); }
    int64_t integers(int64_t low, int64_t high) {  // [low, high)
        return low + static_cast<int64_t>(random() * static_cast<double>(high - low));
    }
    double normal() {  // Box-Muller
        const double u = 1.0 - random(), v = random();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v);
    }
    Samples uniformSamples(size_t count, double low, double high) {
        Samples out(count);
        for (float& x : out) x = static_cast<float>(uniform(low, high));
        return out;
    }
    template <typename T>
    const T& choice(const std::vector<T>& items) {
        return items[static_cast<size_t>(integers(0, static_cast<int64_t>(items.size())))];
    }
    // An index, chosen with these probabilities.
    size_t weighted(const std::vector<double>& probabilities) {
        double x = random();
        for (size_t i = 0; i + 1 < probabilities.size(); ++i) {
            if (x < probabilities[i]) return i;
            x -= probabilities[i];
        }
        return probabilities.size() - 1;
    }

private:
    std::mt19937_64 engine_;
};

// --- Files ---------------------------------------------------------------------------------

inline std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A WAV file's format and its samples' bytes.
struct Wav {
    int format = 0;  // 1: PCM, 3: float (the sub-format of an extensible one)
    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    std::vector<uint8_t> data;

    size_t frames() const { return channels && bitsPerSample ? data.size() / (static_cast<size_t>(channels) * bitsPerSample / 8) : 0; }
    // Every sample as a float in [-1, 1) (PCM) or as it is (float), interleaved.
    std::vector<double> samples() const {
        std::vector<double> out;
        const size_t width = static_cast<size_t>(bitsPerSample / 8);
        for (size_t i = 0; i + width <= data.size(); i += width) {
            const uint8_t* p = data.data() + i;
            if (format == 3 && width == 4) {
                float value;
                std::memcpy(&value, p, 4);
                out.push_back(value);
            } else if (format == 3 && width == 8) {
                double value;
                std::memcpy(&value, p, 8);
                out.push_back(value);
            } else {
                int64_t value = 0;
                for (size_t b = 0; b < width; ++b) value |= static_cast<int64_t>(p[b]) << (8 * b);
                const int64_t sign = int64_t{1} << (bitsPerSample - 1);
                if (value >= sign) value -= sign << 1;
                out.push_back(static_cast<double>(value) / static_cast<double>(sign));
            }
        }
        return out;
    }
};

inline Wav readWav(const std::filesystem::path& path) {
    const std::vector<uint8_t> bytes = readBytes(path);
    Wav wav;
    const auto u16 = [&](size_t at) { return static_cast<int>(bytes[at] | bytes[at + 1] << 8); };
    const auto u32 = [&](size_t at) {
        return static_cast<uint32_t>(bytes[at]) | static_cast<uint32_t>(bytes[at + 1]) << 8 |
               static_cast<uint32_t>(bytes[at + 2]) << 16 | static_cast<uint32_t>(bytes[at + 3]) << 24;
    };
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return wav;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        const uint32_t size = u32(at + 4);
        const size_t body = at + 8;
        if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && body + 16 <= bytes.size()) {
            wav.format = u16(body);
            wav.channels = u16(body + 2);
            wav.sampleRate = static_cast<int>(u32(body + 4));
            wav.bitsPerSample = u16(body + 14);
            if (wav.format == 0xFFFE && body + 26 <= bytes.size()) wav.format = u16(body + 24);  // extensible
        } else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
            const size_t end = std::min(bytes.size(), body + size);
            wav.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(body), bytes.begin() + static_cast<std::ptrdiff_t>(end));
            break;
        }
        at = body + size + (size & 1);
    }
    return wav;
}

}  // namespace subtest
