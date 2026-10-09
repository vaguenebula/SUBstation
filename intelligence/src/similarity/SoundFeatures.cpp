#include "similarity/SoundFeatures.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>
#include <vector>

#include "core/AudioReader.h"
#include "signalsmith-linear/fft.h"  // (vendored with the engine: engine/third_party)

namespace sub::intelligence {

namespace {

constexpr double kMinHz = 20.0;
constexpr double kMaxHz = 16000.0;
constexpr int kMelBands = 40;
constexpr double kSubBassHz = 120.0;
constexpr double kAirHz = 8000.0;
constexpr double kRolloff = 0.85;
constexpr double kFrameRangeDb = 50.0;  // frames within this of the loudest are weighed in, louder ones more
constexpr double kStartDb = -45.0;      // a sound starts where it first comes this near its peak
constexpr double kPreRollSeconds = 0.002;
constexpr double kBlockSeconds = 0.002;  // the amplitude envelope's resolution
constexpr double kAttackSeconds = 0.03;
constexpr double kBodySeconds = 0.25;
constexpr double kDurationDb = -30.0;
constexpr double kContourFloorDb = -60.0;
constexpr double kContourFirstSeconds = 0.02;
// A frame's mel bands are floored this far below its loudest: no quieter detail
// counts, and bands a file's rate can't hold (above its Nyquist frequency: a
// 22.05 kHz file's above 11 kHz) read as quiet, not as -100 dB of silence.
constexpr double kMelRangeDb = 80.0;
// A sound ends where it last comes this near its peak (for its length, and the
// rate of its onsets: silence padding a file changes neither).
constexpr double kEndDb = -60.0;
// Pitch: YIN on the sound brought down to about 11 kHz, 93 ms windows (down to
// 21.5 Hz), at most kPitchFrames of them from the peak on.
constexpr double kPitchRate = 11025.0;
constexpr double kPitchWindowSeconds = 0.08;
constexpr double kMaxPitchHz = 2000.0;
constexpr int kPitchFrames = 8;
constexpr double kPitchRangeDb = 40.0;
constexpr double kYinThreshold = 0.15;
constexpr double kVoicedAperiodicity = 0.3;
constexpr double kPitchReferenceHz = 220.0;
// Onsets: a rise of the mel spectrum this far above the usual that raises the
// level at least 3 dB over the frames just before, at least 50 ms apart, and
// not in the first 60 ms (the sound's own attack).
constexpr double kOnsetMinRiseDb = 1.5;
constexpr double kOnsetGapSeconds = 0.05;
constexpr double kOnsetIgnoreSeconds = 0.06;
constexpr double kOnsetRangeDb = 40.0;
constexpr double kOnsetFloorDb = 60.0;
constexpr double kOnsetLevelRiseDb = 3.0;
constexpr size_t kOnsetLookBack = 4;  // frames

size_t nextPowerOfTwo(size_t n) {
    size_t p = 2;
    while (p < n) p <<= 1;
    return p;
}

double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

double dB(double power, double floor = 1e-12) { return 10.0 * std::log10(power + floor); }

// A weighted mean and standard deviation.
struct Weighted {
    double sw = 0.0, sx = 0.0, sxx = 0.0;
    void add(double x, double w) {
        sw += w;
        sx += w * x;
        sxx += w * x * x;
    }
    double mean() const { return sw > 0.0 ? sx / sw : 0.0; }
    double sd() const {
        if (sw <= 0.0) return 0.0;
        const double m = sx / sw;
        return std::sqrt(std::max(0.0, sxx / sw - m * m));
    }
};

// About 23 ms (21-32 ms), hop a quarter of it.
size_t frameSize(uint32_t rate) { return rate >= 32000 ? 1024 : rate >= 16000 ? 512 : 256; }

struct MelBank {
    uint32_t rate = 0;
    size_t size = 0;
    size_t lo = 0, hi = 0;  // the bins analysed: [lo, hi]
    std::vector<std::vector<std::pair<uint32_t, float>>> bands;

    void make(uint32_t sampleRate, size_t frame) {
        rate = sampleRate;
        size = frame;
        const double binHz = static_cast<double>(rate) / static_cast<double>(frame);
        const double top = std::min(kMaxHz, rate / 2.0);
        lo = static_cast<size_t>(std::ceil(kMinHz / binHz));
        hi = std::min(frame / 2, static_cast<size_t>(std::floor(top / binHz)));
        const double melLo = hzToMel(kMinHz), melHi = hzToMel(kMaxHz);
        std::vector<double> edges(kMelBands + 2);
        for (int i = 0; i < kMelBands + 2; ++i) edges[i] = melToHz(melLo + (melHi - melLo) * i / (kMelBands + 1));
        bands.assign(kMelBands, {});
        for (int b = 0; b < kMelBands; ++b) {
            const double left = edges[b], centre = edges[b + 1], right = edges[b + 2];
            for (size_t k = lo; k <= hi; ++k) {
                const double f = static_cast<double>(k) * binHz;
                double w = 0.0;
                if (f > left && f <= centre) w = (f - left) / (centre - left);
                else if (f > centre && f < right) w = (right - f) / (right - centre);
                if (w > 0.0) bands[b].push_back({static_cast<uint32_t>(k), static_cast<float>(w)});
            }
            // Bands narrower than a bin take the bin nearest their centre.
            const auto nearest = static_cast<size_t>(std::lround(centre / binHz));
            if (bands[b].empty() && nearest >= lo && nearest <= hi)
                bands[b].push_back({static_cast<uint32_t>(nearest), 1.f});
        }
    }
};

struct Frame {
    double db = 0.0;
    double centroid = 0.0, bandwidth = 0.0, flatness = 0.0, rolloff = 0.0, sub = 0.0, air = 0.0;
    std::array<float, kMelBands> mel{};
    size_t start = 0;  // its first sample
};

// Finds the time lag of one YIN window (the cumulative mean normalised
// difference's first dip below the threshold, or its lowest), with its
// aperiodicity (the difference there: 0 is perfectly periodic).
struct YinResult {
    double lag = 0.0;
    double aperiodicity = 1.0;
};

}  // namespace

struct SoundAnalyzer::State {
    // Signalsmith Linear's FFTs, resized when the frame or window size changes.
    // The real one gives bins 0..n/2 - 1, with the Nyquist bin's (real) value
    // in bin 0's imaginary part; neither is scaled, forwards or back.
    signalsmith::linear::RealFFT<float> realFft;
    size_t realSize = 0;
    signalsmith::linear::FFT<float> complexFft;
    size_t complexSize = 0;
    MelBank mel;
    std::vector<float> window;  // Hann, frame size
    std::vector<float> buffer;
    std::vector<std::complex<float>> spectrum;
    std::vector<float> power;
    std::vector<Frame> frames;
    std::vector<float> signal;
    std::vector<double> blocks;
    std::vector<float> pitchSignal;
    std::vector<std::complex<float>> yinIn, yinSpectrum, yinProduct, yinOut;
    std::vector<double> yinD;
    std::array<std::array<float, kMelBands>, kMfccs> dct{};

    State() {
        for (int j = 0; j < kMfccs; ++j)
            for (int b = 0; b < kMelBands; ++b)
                dct[j][b] = static_cast<float>(std::cos(std::numbers::pi * (j + 1) * (b + 0.5) / kMelBands) *
                                               std::sqrt(2.0 / kMelBands));
    }

    void realFftOf(size_t size) {
        if (realSize != size) {
            realFft.resize(size);
            realSize = size;
        }
    }
    void complexFftOf(size_t size) {
        if (complexSize != size) {
            complexFft.resize(size);
            complexSize = size;
        }
    }

    YinResult yin(const float* x, size_t w, double rate) {
        // d(tau) = sum over j < w/2 of (x[j] - x[j + tau])^2, from the
        // correlation of the first half with the whole window (one FFT of both
        // packed as real and imaginary parts, one inverse).
        const size_t half = w / 2;
        const size_t m = 2 * w;
        complexFftOf(m);
        yinIn.assign(m, {});
        for (size_t j = 0; j < w; ++j) yinIn[j] = {j < half ? x[j] : 0.f, x[j]};
        yinSpectrum.resize(m);
        complexFft.fft(yinIn.data(), yinSpectrum.data());
        yinProduct.resize(m);
        for (size_t k = 0; k < m; ++k) {
            const std::complex<float> z = yinSpectrum[k], zc = std::conj(yinSpectrum[(m - k) % m]);
            const std::complex<float> a = 0.5f * (z + zc);                               // the first half's
            const std::complex<float> b = std::complex<float>(0.f, -0.5f) * (z - zc);   // the window's
            yinProduct[k] = std::conj(a) * b;
        }
        yinOut.resize(m);
        complexFft.ifft(yinProduct.data(), yinOut.data());
        // Energies of x[tau .. tau + half).
        std::vector<double>& d = yinD;
        d.assign(half, 0.0);
        double e0 = 0.0;
        for (size_t j = 0; j < half; ++j) e0 += static_cast<double>(x[j]) * x[j];
        double et = e0;
        const double scale = 1.0 / static_cast<double>(m);
        for (size_t tau = 1; tau < half; ++tau) {
            et += static_cast<double>(x[tau + half - 1]) * x[tau + half - 1] - static_cast<double>(x[tau - 1]) * x[tau - 1];
            d[tau] = std::max(0.0, e0 + et - 2.0 * yinOut[tau].real() * scale);
        }
        // The cumulative mean normalised difference.
        double running = 0.0;
        d[0] = 1.0;
        for (size_t tau = 1; tau < half; ++tau) {
            running += d[tau];
            d[tau] = running > 0.0 ? d[tau] * static_cast<double>(tau) / running : 1.0;
        }
        const size_t minLag = std::max<size_t>(2, static_cast<size_t>(rate / kMaxPitchHz));
        size_t best = 0;
        for (size_t tau = minLag; tau + 1 < half; ++tau) {
            if (d[tau] < kYinThreshold) {
                while (tau + 2 < half && d[tau + 1] < d[tau]) ++tau;
                best = tau;
                break;
            }
        }
        if (best == 0) {
            best = minLag;
            for (size_t tau = minLag; tau + 1 < half; ++tau)
                if (d[tau] < d[best]) best = tau;
        }
        YinResult r;
        r.aperiodicity = std::clamp(d[best], 0.0, 1.0);
        double lag = static_cast<double>(best);
        if (best > 1 && best + 1 < half) {  // parabolic interpolation
            const double a = d[best - 1], b = d[best], c = d[best + 1];
            const double denominator = a - 2.0 * b + c;
            if (denominator > 0.0) lag += std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
        }
        r.lag = lag;
        return r;
    }
};

SoundAnalyzer::SoundAnalyzer() : state_(std::make_unique<State>()) {}

SoundAnalyzer::~SoundAnalyzer() = default;

std::optional<Fingerprint> SoundAnalyzer::analyze(const float* samples, size_t count, uint32_t sampleRate,
                                                  double fileSeconds, bool truncated) {
    State& s = *state_;
    if (!samples || count == 0 || sampleRate < 4000) return std::nullopt;
    const double rate = sampleRate;

    // --- Level: relative to the peak; where the sound starts ---------------------------
    float peak = 0.f;
    for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::fabs(samples[i]));
    if (!(peak > 1e-5f) || !std::isfinite(peak)) return std::nullopt;  // silent (below -100 dBFS)
    const float gain = 1.f / peak;
    const float threshold = static_cast<float>(std::pow(10.0, kStartDb / 20.0));
    size_t first = 0;
    while (first < count && std::fabs(samples[first]) * gain < threshold) ++first;
    // Always the same pre-roll before the start (silence where the file has
    // none), so where the frames fall doesn't depend on the silence before it.
    const auto preRoll = static_cast<size_t>(kPreRollSeconds * rate);
    const size_t lead = preRoll > first ? preRoll - first : 0;
    const size_t from = first - (preRoll - lead);
    const size_t window = static_cast<size_t>(kAnalysisSeconds * rate);
    const size_t length = std::min(count - from + lead, window);
    const bool cut = truncated || from + (length - lead) < count;
    s.signal.assign(lead, 0.f);
    s.signal.insert(s.signal.end(), samples + from, samples + from + (length - lead));
    for (float& x : s.signal) x *= gain;
    const float* x = s.signal.data();
    if (fileSeconds <= 0.0) fileSeconds = static_cast<double>(count) / rate;
    // How long the sound is: from its start to where it last comes within
    // kEndDb of its peak; if the file goes on past what is here, to the file's end.
    const float endThreshold = static_cast<float>(std::pow(10.0, kEndDb / 20.0));
    size_t last = count;
    while (last > first && std::fabs(samples[last - 1]) * gain < endThreshold) --last;
    const double soundSeconds = truncated ? std::max(fileSeconds - static_cast<double>(first) / rate, 0.0)
                                          : static_cast<double>(last - first) / rate;
    // How much of the analysed part is the sound (not silence after it).
    const double seconds =
        std::max(std::min(static_cast<double>(length) / rate, soundSeconds + kPreRollSeconds), kBlockSeconds);

    Fingerprint fp{};

    // --- The amplitude envelope, in 2 ms blocks ---------------------------------------
    const size_t blockLen = std::max<size_t>(1, static_cast<size_t>(std::lround(kBlockSeconds * rate)));
    const double blockSeconds = static_cast<double>(blockLen) / rate;
    const size_t blockCount = (length + blockLen - 1) / blockLen;
    s.blocks.assign(blockCount, 0.0);
    for (size_t b = 0; b < blockCount; ++b) {
        const size_t begin = b * blockLen, end = std::min(length, begin + blockLen);
        double sum = 0.0;
        for (size_t i = begin; i < end; ++i) sum += static_cast<double>(x[i]) * x[i];
        s.blocks[b] = sum / static_cast<double>(blockLen);
    }
    size_t peakBlock = 0;
    for (size_t b = 1; b < blockCount; ++b)
        if (s.blocks[b] > s.blocks[peakBlock]) peakBlock = b;
    const double peakEnergy = std::max(s.blocks[peakBlock], 1e-20);
    {
        // Log attack time: from 20% to 90% of the peak amplitude (4% and 81% of its energy).
        size_t b20 = peakBlock, b90 = peakBlock;
        for (size_t b = 0; b <= peakBlock; ++b) {
            if (s.blocks[b] >= 0.04 * peakEnergy) {
                b20 = b;
                break;
            }
        }
        for (size_t b = b20; b <= peakBlock; ++b) {
            if (s.blocks[b] >= 0.81 * peakEnergy) {
                b90 = b;
                break;
            }
        }
        fp[feature::AttackTime] = static_cast<float>(std::log10((static_cast<double>(b90 - b20) + 0.5) * blockSeconds));
        const double durationLevel = peakEnergy * std::pow(10.0, kDurationDb / 10.0);
        size_t loud = 0;
        double sum = 0.0, weighted = 0.0;
        for (size_t b = 0; b < blockCount; ++b) {
            if (s.blocks[b] >= durationLevel) ++loud;
            sum += s.blocks[b];
            weighted += s.blocks[b] * (static_cast<double>(b) + 0.5);
        }
        fp[feature::Duration] = static_cast<float>(std::log10(std::max<size_t>(loud, 1) * blockSeconds));
        fp[feature::TemporalCentroid] =
            static_cast<float>(std::log10(std::max(sum > 0.0 ? weighted / sum : 0.5, 0.5) * blockSeconds));
        // The level in octave-wide windows after the peak. Past the end of the
        // sound is silence; past the end of what was analysed, unknown (the
        // window before it holds).
        double previous = 0.0;
        for (int i = 0; i < kContourPoints; ++i) {
            const double centre = kContourFirstSeconds * std::pow(2.0, i);
            const double from = centre / std::numbers::sqrt2, to = centre * std::numbers::sqrt2;
            const auto b0 = peakBlock + static_cast<size_t>(from / blockSeconds);
            const auto b1 = peakBlock + std::max<size_t>(static_cast<size_t>(to / blockSeconds), 1);
            double energy = 0.0;
            size_t n = 0;
            for (size_t b = b0; b < b1; ++b) {
                if (b < blockCount) {
                    energy += s.blocks[b];
                    ++n;
                } else if (!cut) {
                    ++n;
                }
            }
            double level = n ? std::clamp(dB(energy / static_cast<double>(n) / peakEnergy, 1e-9), kContourFloorDb, 0.0)
                             : (i ? previous : 0.0);
            fp[feature::Contour + i] = static_cast<float>(level);
            previous = level;
        }
    }

    // --- The spectrum, frame by frame ---------------------------------------------------
    const size_t n = frameSize(sampleRate);
    const size_t hop = n / 4;
    if (s.mel.rate != sampleRate || s.mel.size != n) {
        s.mel.make(sampleRate, n);
        s.window.resize(n);
        for (size_t i = 0; i < n; ++i)
            s.window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / static_cast<double>(n)));
    }
    s.realFftOf(n);
    const double binHz = rate / static_cast<double>(n);
    // Frames up to the end: past it is silence (unless the sound goes on, cut
    // off: then only whole frames), so silence after a sound changes nothing.
    const size_t frameCount = length <= n ? 1 : cut ? 1 + (length - n + hop - 1) / hop : 1 + (length - 1) / hop;
    s.frames.resize(frameCount);
    s.buffer.resize(n);
    s.spectrum.resize(n / 2);
    s.power.resize(n / 2 + 1);
    const size_t lo = s.mel.lo, hi = s.mel.hi;
    double loudest = -1e9;
    for (size_t t = 0; t < frameCount; ++t) {
        Frame& fr = s.frames[t];
        fr.start = t * hop;
        for (size_t i = 0; i < n; ++i) {
            const size_t at = fr.start + i;
            s.buffer[i] = at < length ? x[at] * s.window[i] : 0.f;
        }
        s.realFft.fft(s.buffer.data(), s.spectrum.data());
        double total = 0.0, moment = 0.0, logSum = 0.0, sub = 0.0, air = 0.0;
        for (size_t k = lo; k <= hi; ++k) {
            // (Bin n/2, the Nyquist frequency's, comes in bin 0's imaginary part.)
            const double p = k < n / 2 ? std::norm(s.spectrum[k]) : static_cast<double>(s.spectrum[0].imag()) * s.spectrum[0].imag();
            s.power[k] = static_cast<float>(p);
            const double f = static_cast<double>(k) * binHz;
            total += p;
            moment += p * f;
            logSum += std::log(p + 1e-12);
            if (f < kSubBassHz) sub += p;
            if (f > kAirHz) air += p;
        }
        const double bins = static_cast<double>(hi - lo + 1);
        fr.db = dB(total);
        loudest = std::max(loudest, fr.db);
        const double centroid = total > 0.0 ? moment / total : 0.0;
        double spread = 0.0, cumulative = 0.0;
        double rolloff = static_cast<double>(hi) * binHz;
        bool rolled = false;
        for (size_t k = lo; k <= hi; ++k) {
            const double p = s.power[k];
            const double f = static_cast<double>(k) * binHz;
            spread += p * (f - centroid) * (f - centroid);
            cumulative += p;
            if (!rolled && cumulative >= kRolloff * total) {
                rolloff = f;
                rolled = true;
            }
        }
        fr.centroid = std::log2(std::max(centroid, kMinHz) / 1000.0);
        fr.bandwidth = std::log2(std::max(total > 0.0 ? std::sqrt(spread / total) : 0.0, 10.0) / 1000.0);
        fr.rolloff = std::log2(std::max(rolloff, kMinHz) / 1000.0);
        fr.flatness = std::clamp(10.0 * (logSum / bins - std::log(total / bins + 1e-12)) / std::log(10.0), -60.0, 0.0);
        fr.sub = total > 0.0 ? sub / total : 0.0;
        fr.air = total > 0.0 ? air / total : 0.0;
        float loudestBand = -1e9f;
        for (int b = 0; b < kMelBands; ++b) {
            double e = 0.0;
            for (const auto& [k, w] : s.mel.bands[b]) e += w * s.power[k];
            fr.mel[b] = static_cast<float>(dB(e, 1e-10));
            loudestBand = std::max(loudestBand, fr.mel[b]);
        }
        for (float& m : fr.mel) m = std::max(m, loudestBand - static_cast<float>(kMelRangeDb));
    }

    // Louder frames count more; those more than kFrameRangeDb down, not at all.
    std::vector<double> weights(frameCount);
    for (size_t t = 0; t < frameCount; ++t)
        weights[t] = std::clamp((s.frames[t].db - (loudest - kFrameRangeDb)) / kFrameRangeDb, 0.0, 1.0);
    std::array<Weighted, kMfccs> mfcc{};
    std::array<std::array<Weighted, kPartMfccs>, kParts> parts{};
    Weighted centroid, bandwidth, flatness, rolloff, sub, air, attack;
    const auto attackEnd = static_cast<size_t>(kAttackSeconds * rate);
    const auto bodyEnd = static_cast<size_t>(kBodySeconds * rate);
    for (size_t t = 0; t < frameCount; ++t) {
        const Frame& fr = s.frames[t];
        const double w = weights[t];
        if (w <= 0.0) continue;
        const int part = fr.start < attackEnd ? 0 : fr.start < bodyEnd ? 1 : 2;
        for (int j = 0; j < kMfccs; ++j) {
            double c = 0.0;
            for (int b = 0; b < kMelBands; ++b) c += s.dct[j][b] * fr.mel[b];
            mfcc[j].add(c, w);
            if (j < kPartMfccs) parts[part][j].add(c, w);
        }
        centroid.add(fr.centroid, w);
        bandwidth.add(fr.bandwidth, w);
        flatness.add(fr.flatness, w);
        rolloff.add(fr.rolloff, w);
        sub.add(fr.sub, w);
        air.add(fr.air, w);
        if (fr.start <= attackEnd || attack.sw <= 0.0) attack.add(fr.centroid, w);
    }
    for (int j = 0; j < kMfccs; ++j) fp[feature::MfccMean + j] = static_cast<float>(mfcc[j].mean());
    // The timbre of the attack, the body and the tail; a part the sound doesn't
    // reach has the one before it.
    for (int part = 0; part < kParts; ++part) {
        for (int j = 0; j < kPartMfccs; ++j) {
            const Weighted& here = parts[part][j];
            const double value = here.sw > 0.0 ? here.mean()
                                 : part > 0  ? fp[feature::PartMfcc + (part - 1) * kPartMfccs + j]
                                             : mfcc[j].mean();
            fp[feature::PartMfcc + part * kPartMfccs + j] = static_cast<float>(value);
        }
    }
    fp[feature::Centroid] = static_cast<float>(centroid.mean());
    fp[feature::CentroidSpread] = static_cast<float>(centroid.sd());
    fp[feature::Bandwidth] = static_cast<float>(bandwidth.mean());
    fp[feature::Flatness] = static_cast<float>(flatness.mean());
    fp[feature::Rolloff] = static_cast<float>(rolloff.mean());
    fp[feature::SubBass] = static_cast<float>(dB(sub.mean(), 1e-4));
    fp[feature::Air] = static_cast<float>(dB(air.mean(), 1e-4));
    fp[feature::AttackCentroid] = static_cast<float>(attack.mean());

    // --- Onsets after the first --------------------------------------------------------
    {
        // The spectral flux: how much the mel bands rise from frame to frame,
        // not counting bands far below the loudest (the noise floor's jitter).
        float top = -1e9f;
        for (const Frame& fr : s.frames)
            for (const float m : fr.mel) top = std::max(top, m);
        const float floor = top - static_cast<float>(kOnsetFloorDb);
        std::vector<double> flux(frameCount, 0.0), levelRise(frameCount, 0.0);
        Weighted usual;
        for (size_t t = 1; t < frameCount; ++t) {
            if (s.frames[t].db < loudest - kOnsetRangeDb) continue;
            double rise = 0.0;
            for (int b = 0; b < kMelBands; ++b)
                rise += std::max(0.f, std::max(s.frames[t].mel[b], floor) - std::max(s.frames[t - 1].mel[b], floor));
            flux[t] = rise / kMelBands;
            usual.add(flux[t], 1.0);
            // A new hit raises the level, where a sound dying away (even one
            // whose pitch falls, as a kick's) doesn't.
            double before = s.frames[t - 1].db;
            for (size_t k = 2; k <= kOnsetLookBack && k <= t; ++k) before = std::min(before, s.frames[t - k].db);
            levelRise[t] = s.frames[t].db - before;
        }
        const double limit = std::max(kOnsetMinRiseDb, usual.mean() + usual.sd());
        const double hopSeconds = static_cast<double>(hop) / rate;
        const auto gap = static_cast<size_t>(std::ceil(kOnsetGapSeconds / hopSeconds));
        const auto skip = static_cast<size_t>(std::ceil(kOnsetIgnoreSeconds / hopSeconds));
        size_t onsets = 0, last = 0;
        bool any = false;
        for (size_t t = std::max<size_t>(skip, 1); t < frameCount; ++t) {
            if (flux[t] < limit || levelRise[t] < kOnsetLevelRiseDb) continue;
            bool isPeak = true;
            for (size_t u = t > 2 ? t - 2 : 0; u <= std::min(frameCount - 1, t + 2) && isPeak; ++u)
                isPeak = u == t || flux[u] < flux[t] || (flux[u] == flux[t] && u > t);
            if (!isPeak || (any && t - last < gap)) continue;
            ++onsets;
            last = t;
            any = true;
        }
        fp[feature::OnsetRate] = static_cast<float>(std::log2(1.0 + static_cast<double>(onsets) / std::max(seconds, 0.05)));
        fp[feature::Length] = static_cast<float>(std::log10(std::clamp(soundSeconds, 0.01, 60.0)));
    }

    // --- Pitch (YIN), from the peak on ---------------------------------------------------
    {
        const size_t factor = std::max<size_t>(1, static_cast<size_t>(rate / kPitchRate));
        const double pitchRate = rate / static_cast<double>(factor);
        s.pitchSignal.resize(length / factor);
        for (size_t i = 0; i < s.pitchSignal.size(); ++i) {
            float sum = 0.f;
            for (size_t j = 0; j < factor; ++j) sum += x[i * factor + j];
            s.pitchSignal[i] = sum / static_cast<float>(factor);
        }
        const size_t w = nextPowerOfTwo(static_cast<size_t>(kPitchWindowSeconds * pitchRate));
        const size_t step = w / 2;
        const size_t from = peakBlock * blockLen / factor;
        struct Estimate {
            double pitch, aperiodicity, energy;
        };
        std::vector<Estimate> estimates;
        double totalEnergy = 0.0, maxEnergy = 0.0;
        std::vector<float> frame(w);
        for (int i = 0; i < kPitchFrames; ++i) {
            const size_t start = from + static_cast<size_t>(i) * step;
            if (start >= s.pitchSignal.size()) break;
            double energy = 0.0;
            for (size_t j = 0; j < w; ++j) {
                frame[j] = start + j < s.pitchSignal.size() ? s.pitchSignal[start + j] : 0.f;
                energy += static_cast<double>(frame[j]) * frame[j];
            }
            maxEnergy = std::max(maxEnergy, energy);
            if (energy < maxEnergy * std::pow(10.0, -kPitchRangeDb / 10.0) || energy <= 0.0) break;
            totalEnergy += energy;
            const YinResult r = s.yin(frame.data(), w, pitchRate);
            if (r.aperiodicity < kVoicedAperiodicity && r.lag > 0.0)
                estimates.push_back({std::log2(pitchRate / r.lag / kPitchReferenceHz), r.aperiodicity, energy});
        }
        double voiced = 0.0, aperiodic = 0.0;
        for (const Estimate& e : estimates) {
            voiced += e.energy;
            aperiodic += e.energy * e.aperiodicity;
        }
        if (voiced > 0.0 && totalEnergy > 0.0) {
            const double confidence = (voiced / totalEnergy) * (1.0 - aperiodic / voiced);
            // The energy-weighted median pitch.
            std::sort(estimates.begin(), estimates.end(), [](const Estimate& a, const Estimate& b) { return a.pitch < b.pitch; });
            double half = voiced / 2.0, pitch = estimates.front().pitch;
            for (const Estimate& e : estimates) {
                pitch = e.pitch;
                half -= e.energy;
                if (half <= 0.0) break;
            }
            fp[feature::PitchConfidence] = static_cast<float>(std::clamp(confidence, 0.0, 1.0));
            fp[feature::Pitch] = static_cast<float>(std::clamp(confidence, 0.0, 1.0) * pitch);
        }
    }

    for (float& v : fp)
        if (!std::isfinite(v)) v = 0.f;
    return fp;
}

std::optional<Fingerprint> SoundAnalyzer::analyzeFile(const std::string& path, double start, double length) {
    const double wanted = kLeadInSeconds + kAnalysisSeconds;
    const double decode = length < 0.0 ? wanted : std::min(length, wanted);
    const MonoAudio audio = readMono(path, start, decode);
    const double fileSeconds = length >= 0.0 ? length : audio.fileSeconds - std::max(0.0, start);
    const bool truncated = length >= 0.0 ? length > decode : audio.truncated;
    return analyze(audio.samples.data(), audio.samples.size(), audio.sampleRate, fileSeconds, truncated);
}

const std::array<FeatureInfo, kDims>& featureInfo() {
    static const std::array<FeatureInfo, kDims> info = [] {
        std::array<FeatureInfo, kDims> a{};
        static const char* const mfccMean[kMfccs] = {"mfcc1",  "mfcc2",  "mfcc3",  "mfcc4",  "mfcc5",  "mfcc6",
                                                     "mfcc7",  "mfcc8",  "mfcc9",  "mfcc10", "mfcc11", "mfcc12"};
        static const char* const partMfcc[kParts * kPartMfccs] = {
            "attackMfcc1", "attackMfcc2", "attackMfcc3", "attackMfcc4", "bodyMfcc1", "bodyMfcc2",
            "bodyMfcc3",   "bodyMfcc4",   "tailMfcc1",   "tailMfcc2",   "tailMfcc3", "tailMfcc4"};
        static const char* const contour[kContourPoints] = {"level20ms",  "level40ms",  "level80ms",  "level160ms",
                                                            "level320ms", "level640ms", "level1280ms", "level2560ms"};
        for (int j = 0; j < kMfccs; ++j) a[feature::MfccMean + j] = {mfccMean[j], Aspect::Timbre, 1.0f};
        for (int j = 0; j < kParts * kPartMfccs; ++j) a[feature::PartMfcc + j] = {partMfcc[j], Aspect::TimbreMotion, 1.0f};
        a[feature::Centroid] = {"centroid", Aspect::Spectrum, 0.1f};
        a[feature::CentroidSpread] = {"centroidSd", Aspect::Spectrum, 0.05f};
        a[feature::Bandwidth] = {"bandwidth", Aspect::Spectrum, 0.1f};
        a[feature::Flatness] = {"flatness", Aspect::Spectrum, 1.0f};
        a[feature::Rolloff] = {"rolloff", Aspect::Spectrum, 0.1f};
        a[feature::SubBass] = {"subBass", Aspect::Spectrum, 1.0f};
        a[feature::Air] = {"air", Aspect::Spectrum, 1.0f};
        a[feature::AttackCentroid] = {"attackCentroid", Aspect::Spectrum, 0.1f};
        a[feature::AttackTime] = {"attackTime", Aspect::Envelope, 0.05f};
        a[feature::Duration] = {"duration", Aspect::Envelope, 0.05f};
        a[feature::TemporalCentroid] = {"temporalCentroid", Aspect::Envelope, 0.05f};
        for (int i = 0; i < kContourPoints; ++i) a[feature::Contour + i] = {contour[i], Aspect::Envelope, 1.0f};
        a[feature::PitchConfidence] = {"pitchConfidence", Aspect::Pitch, 0.05f};
        a[feature::Pitch] = {"pitch", Aspect::Pitch, 0.05f};
        a[feature::OnsetRate] = {"onsetRate", Aspect::Rhythm, 0.1f};
        a[feature::Length] = {"length", Aspect::Rhythm, 0.05f};
        return a;
    }();
    return info;
}

const char* aspectName(Aspect aspect) {
    switch (aspect) {
        case Aspect::Timbre: return "timbre";
        case Aspect::TimbreMotion: return "timbreMotion";
        case Aspect::Spectrum: return "spectrum";
        case Aspect::Envelope: return "envelope";
        case Aspect::Pitch: return "pitch";
        case Aspect::Rhythm: return "rhythm";
    }
    return "";
}

}  // namespace sub::intelligence
