#include "analysis/SidechainFit.h"

#include "analysis/Fft.h"
#include "model/Automation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace sub::app::sidechainFit {

namespace {

using analysis::Complex;

// numpy.argmax: the first of the largest.
std::size_t argmax(const std::vector<double>& values, std::size_t from = 0) {
    std::size_t best = from;
    for (std::size_t i = from + 1; i < values.size(); ++i)
        if (values[i] > values[best])
            best = i;
    return best;
}

double maxOf(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

// numpy.interp(x, xp, fp, left, right): xp increasing.
double interp(double x, const std::vector<double>& xp, const std::vector<double>& fp, double left, double right) {
    if (xp.empty())
        return left;
    if (x < xp.front())
        return left;
    if (x > xp.back())
        return right;
    if (x == xp.back())
        return fp.back();
    const auto above = std::upper_bound(xp.begin(), xp.end(), x);
    const std::size_t j = std::size_t(above - xp.begin());
    const std::size_t i = j - 1;
    const double span = xp[j] - xp[i];
    if (span <= 0.0)
        return fp[j];
    return fp[i] + (fp[j] - fp[i]) * (x - xp[i]) / span;
}

// numpy.geomspace(start, stop, count): the ends exact.
std::vector<double> geomspace(double start, double stop, int count) {
    std::vector<double> values(std::size_t(std::max(count, 0)));
    const double low = std::log10(start), high = std::log10(stop);
    for (int i = 0; i < count; ++i)
        values[std::size_t(i)] = std::pow(10.0, count > 1 ? low + i * ((high - low) / (count - 1)) : low);
    if (count > 0)
        values.front() = start;
    if (count > 1)
        values.back() = stop;
    return values;
}

// numpy.linspace(start, stop, count).
std::vector<double> linspace(double start, double stop, int count) {
    std::vector<double> values(std::size_t(std::max(count, 0)));
    const double step = count > 1 ? (stop - start) / (count - 1) : 0.0;
    for (int i = 0; i < count; ++i)
        values[std::size_t(i)] = i * step + start;
    if (count > 1)
        values.back() = stop;
    return values;
}

double roundTo(double value, int places) {
    const double scale = std::pow(10.0, places);
    return std::nearbyint(value * scale) / scale;  // (Python's round: halves to even)
}

// --- Spectra ---

// Mean power per FFT bin (Hann windows, half overlapping; one zero-padded if short).
std::vector<double> power(const std::vector<double>& samples) {
    static const std::vector<double> window = analysis::hann(kFftSize);
    std::vector<double> total(kFftSize / 2 + 1, 0.0);
    std::vector<double> frame(kFftSize);
    int frames = 0;
    auto add = [&](std::size_t from) {
        for (std::size_t i = 0; i < std::size_t(kFftSize); ++i) {
            const std::size_t at = from + i;
            frame[i] = (at < samples.size() ? samples[at] : 0.0) * window[i];
        }
        const std::vector<Complex> bins = analysis::rfft(frame.data(), frame.size(), kFftSize);
        for (std::size_t k = 0; k < bins.size(); ++k)
            total[k] += std::norm(bins[k]);
        ++frames;
    };
    if (samples.size() <= std::size_t(kFftSize)) {
        add(0);
    } else {
        for (std::size_t i = 0; i + kFftSize <= samples.size(); i += kFftSize / 2)
            add(i);
    }
    for (double& value : total)
        value /= frames;
    return total;
}

// Power at each frequency: the mean of the bins within kSmoothOctaves of it.
std::vector<double> columns(const std::vector<double>& power, double sampleRate, const std::vector<double>& freqs) {
    const double binWidth = sampleRate / kFftSize;
    const long long bins = static_cast<long long>(power.size());
    std::vector<double> total(power.size() + 1, 0.0);
    for (std::size_t i = 0; i < power.size(); ++i)
        total[i + 1] = total[i] + power[i];
    std::vector<double> result(freqs.size());
    for (std::size_t i = 0; i < freqs.size(); ++i) {
        const long long low =
            std::clamp(static_cast<long long>(std::floor(freqs[i] * std::pow(2.0, -kSmoothOctaves) / binWidth)), 0LL,
                       bins - 1);
        const long long high = std::min(
            std::max(static_cast<long long>(std::ceil(freqs[i] * std::pow(2.0, kSmoothOctaves) / binWidth)), low + 1),
            bins);
        result[i] = (total[std::size_t(high)] - total[std::size_t(low)]) / double(high - low);
    }
    return result;
}

std::vector<double> relativeDb(const std::vector<double>& power) {
    const double peak = maxOf(power);
    if (peak <= 0.0)
        return std::vector<double>(power.size(), kFloorDb);
    std::vector<double> result(power.size());
    for (std::size_t i = 0; i < power.size(); ++i)
        result[i] = std::max(10.0 * std::log10(std::max(power[i] / peak, 1e-30)), kFloorDb);
    return result;
}

// A Hann-weighted moving average, the ends held.
std::vector<double> smoothed(const std::vector<double>& values, int width) {
    if (width < 3 || values.size() < 2)
        return values;
    std::vector<double> window = analysis::hann(std::size_t(width) + 2);
    window = std::vector<double>(window.begin() + 1, window.end() - 1);
    const double sum = std::accumulate(window.begin(), window.end(), 0.0);
    for (double& w : window)
        w /= sum;
    const std::size_t pad = std::size_t(width / 2 + 1), n = values.size(), m = window.size();
    std::vector<double> padded(n + 2 * pad);
    std::fill(padded.begin(), padded.begin() + static_cast<std::ptrdiff_t>(pad), values.front());
    std::copy(values.begin(), values.end(), padded.begin() + static_cast<std::ptrdiff_t>(pad));
    std::fill(padded.end() - static_cast<std::ptrdiff_t>(pad), padded.end(), values.back());
    // numpy.convolve(padded, window, "same")[pad:pad + n]: output s of "same" is the full
    // convolution's s + (m - 1) / 2, sum over j of padded[that - j] * window[j].
    const long long total = static_cast<long long>(padded.size());
    std::vector<double> result(n);
    for (std::size_t k = 0; k < n; ++k) {
        const long long full = static_cast<long long>(k + pad + (m - 1) / 2);
        const long long firstJ = std::max(0LL, full - (total - 1));
        const long long lastJ = std::min(static_cast<long long>(m) - 1, full);
        double value = 0.0;
        for (long long j = firstJ; j <= lastJ; ++j)
            value += padded[std::size_t(full - j)] * window[std::size_t(j)];
        result[k] = value;
    }
    return result;
}

// --- Points ---

constexpr int kBendCount = 81;  // numpy.linspace(-1, 1, 81)

const std::vector<double>& bends() {
    static const std::vector<double> values = linspace(-1.0, 1.0, kBendCount);
    return values;
}

// The segment from point i to point j: the curve (as a breakpoint's) fitting y best, and its values.
std::pair<double, std::vector<double>> bestBend(const std::vector<double>& x, const std::vector<double>& y,
                                                std::size_t i, std::size_t j) {
    const double x0 = x[i], y0 = y[i], x1 = x[j], y1 = y[j];
    const std::size_t count = j - i + 1;
    std::vector<double> u(count);
    for (std::size_t k = 0; k < count; ++k)
        u[k] = (x[i + k] - x0) / std::max(x1 - x0, 1e-12);
    if (std::abs(y1 - y0) < 1e-6)
        return {0.0, std::vector<double>(count, y0)};
    std::size_t best = 0;
    double bestError = std::numeric_limits<double>::infinity();
    std::vector<double> candidate(count), bestValues;
    for (std::size_t b = 0; b < bends().size(); ++b) {
        double error = 0.0;
        for (std::size_t k = 0; k < count; ++k) {
            candidate[k] = y0 + (y1 - y0) * shape(u[k], bends()[b]);
            const double d = candidate[k] - y[i + k];
            error += d * d;
        }
        if (error < bestError) {
            best = b;
            bestError = error;
            bestValues = candidate;
        }
    }
    const double bend = bends()[best];
    return {y1 >= y0 ? bend : -bend, bestValues};
}

}  // namespace

// --- Spectra ---

Spectra spectra(const std::vector<double>& kick, const std::vector<double>& bass, double sampleRate) {
    Spectra found;
    found.freqs = geomspace(kClashLow, kClashHigh, kColumns);
    const std::vector<double> kickPower = columns(power(kick), sampleRate, found.freqs);
    double sum = 0.0;
    for (double value : bass)
        sum += value * value;
    found.bassHeard = !bass.empty() && std::sqrt(sum / double(bass.size())) > 1e-4;  // above -80 dBFS
    const std::vector<double> bassPower =
        found.bassHeard ? columns(power(bass), sampleRate, found.freqs) : std::vector<double>(kColumns, 0.0);
    found.kick = relativeDb(kickPower);
    found.bass = relativeDb(bassPower);
    // Without a bass, the kick alone: where it is loudest is where it would clash.
    if (found.bassHeard) {
        std::vector<double> mean(kColumns);
        for (int i = 0; i < kColumns; ++i)
            mean[std::size_t(i)] = std::sqrt(kickPower[std::size_t(i)] * bassPower[std::size_t(i)]);
        found.clash = relativeDb(mean);
    } else {
        found.clash = found.kick;
    }
    const std::size_t peak = argmax(found.clash);
    std::size_t low = peak, high = peak;
    while (low > 0 && found.clash[low - 1] > -kClashDb)
        --low;
    while (high < std::size_t(kColumns) - 1 && found.clash[high + 1] > -kClashDb)
        ++high;
    found.low = found.freqs[low];
    found.high = found.freqs[high];
    found.peak = found.freqs[peak];
    return found;
}

// --- The envelope ---

std::vector<double> clashEnvelope(const std::vector<double>& kick, double sampleRate, const Spectra& found,
                                  int pre) {
    std::size_t size = 1;
    while (size < std::max<std::size_t>(2, 2 * kick.size()))
        size <<= 1;
    const std::vector<Complex> spectrum = analysis::rfft(kick.data(), kick.size(), size);
    // The clash in dB (a power ratio) is the weight in amplitude at /20; nothing outside the band's surroundings.
    std::vector<double> logFreqs(found.freqs.size()), weights(found.clash.size());
    for (std::size_t i = 0; i < found.freqs.size(); ++i)
        logFreqs[i] = std::log(found.freqs[i]);
    for (std::size_t i = 0; i < found.clash.size(); ++i)
        weights[i] = std::max(found.clash[i], -40.0);
    std::vector<Complex> analytic(size, Complex(0.0, 0.0));
    for (std::size_t k = 0; k <= size / 2; ++k) {
        const double bin = double(k) * sampleRate / double(size);  // numpy.fft.rfftfreq
        const double weightDb = interp(std::log(std::max(bin, 1.0)), logFreqs, weights, -120.0, -120.0);
        analytic[k] = spectrum[k] * std::pow(10.0, weightDb / 20.0);
        if (k >= 1 && k < size / 2)
            analytic[k] *= 2.0;
    }
    analysis::fft(analytic, true);
    std::vector<double> envelope(kick.size());
    for (std::size_t i = 0; i < kick.size(); ++i)
        envelope[i] = std::abs(analytic[i]);
    // Smoothed over a cycle of the band's bottom (5..25 ms): no ripple from its partials beating.
    const int width = static_cast<int>(sampleRate * std::clamp(1.0 / std::max(found.low, 1.0), 0.005, 0.025));
    std::vector<double> result = smoothed(envelope, width);
    const std::size_t from = std::min(result.size(), std::size_t(std::max(pre, 0)));
    return std::vector<double>(result.begin() + static_cast<std::ptrdiff_t>(from), result.end());
}

std::vector<double> reduction(const std::vector<double>& envelope, double rangeDb, int smooth) {
    const double peak = maxOf(envelope);
    if (envelope.empty() || peak <= 0.0)
        return std::vector<double>(envelope.size(), 0.0);
    std::vector<double> level(envelope.size());
    for (std::size_t i = 0; i < envelope.size(); ++i)
        level[i] = 20.0 * std::log10(std::max(envelope[i] / peak, 1e-9));
    const std::size_t top = argmax(envelope);
    const std::vector<double> decay =
        smoothed(std::vector<double>(level.begin() + static_cast<std::ptrdiff_t>(top), level.end()), smooth);
    std::copy(decay.begin(), decay.end(), level.begin() + static_cast<std::ptrdiff_t>(top));
    std::vector<double> amount(envelope.size());
    for (std::size_t i = 0; i < envelope.size(); ++i)
        amount[i] = i < top ? 1.0 : std::clamp((level[i] + rangeDb) / rangeDb, 0.0, 1.0);
    for (std::size_t i = top + 1; i < amount.size(); ++i)
        amount[i] = std::min(amount[i], amount[i - 1]);
    return amount;
}

// --- Points ---

double shape(double x, double bend) {
    double a = -bend * automation::kCurvature;
    if (std::abs(a) < 1e-4)
        a = 1e-4;
    return std::expm1(a * x) / std::expm1(a);
}

double curveAt(const std::vector<FitPoint>& points, double x) {
    if (points.empty())
        return 1.0;
    double result = points.back().y;
    if (x <= points.front().x)
        result = points.front().y;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const FitPoint& a = points[i];
        const FitPoint& b = points[i + 1];
        if (b.x > a.x && x >= a.x && x < b.x) {
            const double bend = b.y >= a.y ? a.curve : -a.curve;
            result = a.y + (b.y - a.y) * shape((x - a.x) / (b.x - a.x), bend);
        }
    }
    return result;
}

std::vector<double> curveAt(const std::vector<FitPoint>& points, const std::vector<double>& x) {
    std::vector<double> result(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        result[i] = curveAt(points, x[i]);
    return result;
}

std::vector<FitPoint> fitPoints(const std::vector<double>& x, const std::vector<double>& y, int maxPoints,
                                double tolerance) {
    if (x.empty())
        return {};
    std::vector<std::size_t> chosen{0, x.size() - 1};
    std::vector<double> bendsFound;
    while (true) {
        bendsFound.clear();
        double worst = 0.0;
        long long worstAt = -1;
        for (std::size_t c = 0; c + 1 < chosen.size(); ++c) {
            const std::size_t i = chosen[c], j = chosen[c + 1];
            const auto [bend, values] = bestBend(x, y, i, j);
            bendsFound.push_back(bend);
            std::size_t k = 0;
            double largest = -1.0;
            for (std::size_t m = 0; m < values.size(); ++m) {
                const double error = std::abs(values[m] - y[i + m]);
                if (error > largest) {
                    largest = error;
                    k = m;
                }
            }
            if (largest > worst && i < i + k && i + k < j) {
                worst = largest;
                worstAt = static_cast<long long>(i + k);
            }
        }
        if (worst <= tolerance || worstAt < 0 || static_cast<int>(chosen.size()) >= maxPoints)
            break;
        chosen.push_back(std::size_t(worstAt));
        std::sort(chosen.begin(), chosen.end());
    }
    std::vector<FitPoint> points;
    for (std::size_t c = 0; c < chosen.size(); ++c) {
        const double bend = c < bendsFound.size() ? bendsFound[c] : 0.0;
        points.push_back({roundTo(x[chosen[c]], 5), roundTo(y[chosen[c]], 5), roundTo(bend, 4)});
    }
    return points;
}

// --- Analysis ---

std::optional<Fit> analyze(const std::vector<std::vector<double>>& allKicks, const std::vector<double>& bass,
                           double sampleRate, int character, int pre) {
    std::vector<const std::vector<double>*> kicks;
    double loudest = 0.0;
    for (const std::vector<double>& kick : allKicks) {
        if (kick.size() > std::size_t(std::max(pre, 0) + 16)) {
            kicks.push_back(&kick);
            for (double value : kick)
                loudest = std::max(loudest, std::abs(value));
        }
    }
    if (kicks.empty() || loudest < 1e-4)
        return std::nullopt;
    std::vector<double> joined;
    const std::size_t from = std::size_t(std::max(pre, 0)), span = std::size_t(int(0.3 * sampleRate));
    for (const std::vector<double>* kick : kicks) {
        const std::size_t end = std::min(kick->size(), from + span);
        joined.insert(joined.end(), kick->begin() + static_cast<std::ptrdiff_t>(from),
                      kick->begin() + static_cast<std::ptrdiff_t>(end));
    }
    Fit fit;
    fit.spectra = spectra(joined, bass, sampleRate);
    // Each hit's envelope against its own peak, averaged over the hits (cut to the shortest).
    std::vector<std::vector<double>> envelopes;
    std::size_t shortest = std::numeric_limits<std::size_t>::max();
    for (const std::vector<double>* kick : kicks) {
        envelopes.push_back(clashEnvelope(*kick, sampleRate, fit.spectra, pre));
        shortest = std::min(shortest, envelopes.back().size());
    }
    fit.envelope.assign(shortest, 0.0);
    for (const std::vector<double>& e : envelopes) {
        double peak = 0.0;
        for (std::size_t i = 0; i < shortest; ++i)
            peak = std::max(peak, e[i]);
        peak = std::max(peak, 1e-12);
        for (std::size_t i = 0; i < shortest; ++i)
            fit.envelope[i] += e[i] / peak;
    }
    for (double& value : fit.envelope)
        value /= double(envelopes.size());
    const std::size_t rangeIndex = std::size_t(std::clamp(character, 0, int(kRangeDb.size()) - 1));
    const std::vector<double> amount =
        reduction(fit.envelope, kRangeDb[rangeIndex], static_cast<int>(kDecaySmoothSeconds * sampleRate));
    fit.times.resize(shortest);
    for (std::size_t i = 0; i < shortest; ++i)
        fit.times[i] = double(i) * 1000.0 / sampleRate;

    // The length: until the reduction is over (a little after), within the device's range.
    double end = fit.times.empty() ? 0.0 : fit.times.back();
    if (!amount.empty()) {
        const std::size_t top = argmax(amount);
        for (std::size_t i = top; i < amount.size(); ++i) {
            if (amount[i] <= 0.005) {
                end = fit.times[i];
                break;
            }
        }
    }
    fit.length = std::clamp(std::nearbyint(end * 1.08), kLengthMin, kLengthMax);
    const std::vector<double> x = linspace(0.0, 1.0, 400);
    std::vector<double> target(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        target[i] = 1.0 - interp(x[i] * fit.length, fit.times, amount, amount.empty() ? 0.0 : amount.front(), 0.0);
    fit.points = fitPoints(x, target);
    fit.target.resize(amount.size());
    for (std::size_t i = 0; i < amount.size(); ++i)
        fit.target[i] = 1.0 - amount[i];
    return fit;
}

// --- Capturing hits ---

Ring::Ring(std::size_t size) : data_(std::max<std::size_t>(size, 1), 0.0f) {}

void Ring::feed(qint64 start, const float* values, std::size_t count) {
    const qint64 size = static_cast<qint64>(data_.size());
    if (start != end_)  // a gap (or the first values): start again from these
        start_ = end_ = start;
    if (static_cast<qint64>(count) > size) {  // (only the latest fit)
        end_ += static_cast<qint64>(count) - size;
        values += count - std::size_t(size);
        count = std::size_t(size);
    }
    for (std::size_t i = 0; i < count; ++i)
        data_[std::size_t((end_ + static_cast<qint64>(i)) % size)] = values[i];
    end_ += static_cast<qint64>(count);
    start_ = std::max(start_, end_ - size);
}

std::vector<float> Ring::get(qint64 start, qint64 end) const {
    start = std::max(start, start_);
    end = std::min(end, end_);
    if (end <= start)
        return {};
    const qint64 size = static_cast<qint64>(data_.size());
    std::vector<float> values(std::size_t(end - start));
    for (qint64 i = start; i < end; ++i)
        values[std::size_t(i - start)] = data_[std::size_t(i % size)];
    return values;
}

Capture::Capture(double sampleRate, double keepSeconds)
    : sampleRate_(sampleRate),
      key_(std::size_t(keepSeconds * sampleRate)),
      input_(std::size_t(keepSeconds * sampleRate)),
      phase_(std::size_t(keepSeconds * sampleRate)) {}

const Ring& Capture::stream(Stream stream) const {
    switch (stream) {
    case Stream::Key: return key_;
    case Stream::Input: return input_;
    case Stream::Phase: break;
    }
    return phase_;
}

void Capture::feed(Stream stream, qint64 start, const std::vector<float>& values) {
    Ring& ring = stream == Stream::Key ? key_ : stream == Stream::Input ? input_ : phase_;
    ring.feed(start, values.data(), values.size());
    if (stream == Stream::Phase) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] == 0.0f) {
                pending_.push_back(start + static_cast<qint64>(i));
                ++hitCount_;
            }
        }
        lastPhase_ = values.empty() ? -1.0 : double(values.back());
    } else if (stream == Stream::Key) {
        for (float value : values)
            keyPeak_ = std::max(keyPeak_, std::abs(value));
    }
}

float Capture::takeKeyPeak() { return std::exchange(keyPeak_, 0.0f); }

std::vector<Capture::Hit> Capture::hits() {
    const qint64 ready = std::min(key_.end(), input_.end());
    const qint64 pre = static_cast<qint64>(kPreSeconds * sampleRate_);
    std::vector<Hit> done;
    while (!pending_.empty()) {
        const qint64 hit = pending_.front();
        qint64 end = hit + static_cast<qint64>(kKickSeconds * sampleRate_);
        if (pending_.size() > 1)
            end = std::min(end, pending_[1]);
        if (ready < end)
            break;
        pending_.erase(pending_.begin());
        if (hit - pre >= key_.start()) {
            done.emplace_back(key_.get(hit - pre, end),
                              input_.get(hit - static_cast<qint64>(kBassSeconds * sampleRate_), end));
        }
    }
    return done;
}

}  // namespace sub::app::sidechainFit
