// Fitting the Sidechain device's curve to a kick (analysis/SidechainFit): the
// clash between the kick and the input, the kick's envelope there, the
// reduction it calls for, the points, and capturing hits. Where the Python had
// numbers, the same numbers (computed with numpy from the same signals).

#include <QTest>

#include <cmath>
#include <vector>

#include "analysis/Fft.h"
#include "analysis/SidechainFit.h"
#include "model/Automation.h"

namespace sf = sub::app::sidechainFit;

namespace {

constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// A kick: a sine falling from 170 Hz to 50 Hz, dying away over `tail` seconds.
std::vector<double> kick(double seconds = 0.6, double tail = 0.12, int pre = 0) {
    const int n = int(seconds * kRate);
    std::vector<double> samples(std::size_t(pre), 0.0);
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / kRate;
        phase += 50 + 120 * std::exp(-t / 0.02);  // numpy.cumsum(freq)
        samples.push_back(std::sin(2 * kPi * phase / kRate) * std::exp(-t / tail));
    }
    return samples;
}

std::vector<double> bass(const std::vector<double>& freqs, double seconds = 2.0) {
    const int n = int(seconds * kRate);
    std::vector<double> samples(std::size_t(n), 0.0);
    for (int i = 0; i < n; ++i)
        for (double f : freqs)
            samples[std::size_t(i)] += 0.4 * std::sin(2 * kPi * f * (double(i) / kRate));
    return samples;
}

std::vector<double> head(const std::vector<double>& values, std::size_t count) {
    return std::vector<double>(values.begin(), values.begin() + std::ptrdiff_t(std::min(count, values.size())));
}

std::vector<double> linspace(double a, double b, int n) {
    std::vector<double> values(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        values[std::size_t(i)] = a + (b - a) * i / (n - 1);
    values.back() = b;
    return values;
}

double maxOf(const std::vector<double>& values) { return *std::max_element(values.begin(), values.end()); }

bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

}  // namespace

class TestSidechainFit : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void fftMatchesTheDefinition() {
        std::vector<double> x{1.0, 2.0, -1.0, 0.5, 0.25, 0.0, 3.0, -2.0};
        const auto bins = sub::app::analysis::rfft(x.data(), x.size(), 8);
        QCOMPARE(bins.size(), std::size_t(5));
        for (std::size_t k = 0; k < bins.size(); ++k) {
            std::complex<double> expected;
            for (std::size_t n = 0; n < x.size(); ++n)
                expected += x[n] * std::polar(1.0, -2 * kPi * double(k * n) / 8.0);
            QVERIFY(std::abs(bins[k] - expected) < 1e-12);
        }
        std::vector<sub::app::analysis::Complex> data(bins.size() * 2 - 2);
        for (std::size_t i = 0; i < x.size(); ++i)
            data[i] = x[i];
        sub::app::analysis::fft(data);
        sub::app::analysis::fft(data, true);
        for (std::size_t i = 0; i < x.size(); ++i)
            QVERIFY(std::abs(data[i] - x[i]) < 1e-12);
        const auto window = sub::app::analysis::hann(5);
        QVERIFY(near(window[0], 0.0, 1e-15) && near(window[2], 1.0, 1e-15) && near(window[1], 0.5, 1e-15));
    }

    void clashIsWhereBothAreLoud() {
        const sf::Spectra found = sf::spectra(head(kick(), std::size_t(0.3 * kRate)), bass({55, 600}), kRate);
        QVERIFY(found.bassHeard);
        // The bass's 600 Hz doesn't clash: the kick has none there.
        QVERIFY(found.low < 55 && 55 < found.high && found.high < 120);
        QVERIFY(45 < found.peak && found.peak < 70);
        QVERIFY(near(maxOf(found.kick), 0.0, 1e-9) && near(maxOf(found.bass), 0.0, 1e-9));
        QVERIFY(near(maxOf(found.clash), 0.0, 1e-9) && found.freqs.size() == std::size_t(sf::kColumns));
        // numpy's numbers.
        QVERIFY(near(found.low, 46.324407135127686, 1e-6));
        QVERIFY(near(found.high, 71.53014660580419, 1e-6));
        QVERIFY(near(found.peak, 53.543030908418885, 1e-6));
        QVERIFY(near(found.kick[0], -56.44836063620389, 1e-6) && near(found.kick[2], -53.00849693646076, 1e-6));
        QVERIFY(near(found.clash[80], -75.47649561646291, 1e-6));

        // A bass an octave up clashes where the kick has that octave: higher.
        const sf::Spectra higher = sf::spectra(head(kick(), std::size_t(0.3 * kRate)), bass({120}), kRate);
        QVERIFY(higher.peak > found.peak);
        QVERIFY(near(higher.low, 101.2587382256468, 1e-6) && near(higher.high, 143.34280794299497, 1e-6));
        QVERIFY(near(higher.peak, 110.45067146834504, 1e-6));
    }

    void withoutABassTheKickAlone() {
        const sf::Spectra found = sf::spectra(kick(), std::vector<double>(kRate, 0.0), kRate);
        QVERIFY(!found.bassHeard);
        for (double db : found.bass)
            QCOMPARE(db, sf::kFloorDb);
        QVERIFY(found.clash == found.kick);
        QVERIFY(found.low < 55 && 55 < found.high);
        QVERIFY(near(found.low, 42.46919419497187, 1e-6) && near(found.high, 71.53014660580419, 1e-6));
    }

    void reductionHoldsToThePeakThenFollowsTheDecay() {
        std::vector<double> envelope(1000);
        for (int t = 0; t < 1000; ++t)
            envelope[std::size_t(t)] = t < 100 ? t / 100.0 : std::exp(-(t - 100) / 200.0);  // up to its peak at 100
        const std::vector<double> amount = sf::reduction(envelope, 20.0);
        for (int i = 0; i <= 100; ++i)
            QCOMPARE(amount[std::size_t(i)], 1.0);
        for (std::size_t i = 101; i < amount.size(); ++i)
            QVERIFY(amount[i] - amount[i - 1] <= 1e-12);  // never back up
        const double gone = 100 + 200 * std::log(10.0);  // 20 dB down
        QCOMPARE(amount[std::size_t(int(gone) + 2)], 0.0);
        QVERIFY(amount[std::size_t(int(gone) - 50)] > 0);
        QVERIFY(sf::reduction(std::vector<double>(10, 0.0), 20.0) == std::vector<double>(10, 0.0));
    }

    void curveAtBendsAsAutomation() {
        const std::vector<sf::FitPoint> points{{0.0, 0.0, 0.5}, {0.4, 1.0, -0.3}, {1.0, 0.2, 0.0}};
        const std::vector<double> xs = linspace(0, 1, 101);
        const std::vector<double> ours = sf::curveAt(points, xs);
        sub::app::Envelope model;
        for (const sf::FitPoint& p : points)
            model.push_back({p.x, p.y, p.curve});
        for (std::size_t k = 0; k < xs.size(); ++k) {
            const int count = sub::app::automation::countAtOrBefore(model, xs[k]);
            const int i = std::max(0, std::min(int(model.size()) - 2, count - 1));
            const double expected =
                sub::app::automation::segmentValue(model[std::size_t(i)], model[std::size_t(i) + 1], xs[k]);
            QVERIFY2(near(ours[k], expected, 1e-9), qPrintable(QString::number(xs[k])));
        }
    }

    void fitPointsFindsACurveAgain() {
        const std::vector<sf::FitPoint> points{{0.0, 0.0, 0.0}, {0.15, 0.0, -0.5}, {0.6, 0.8, 0.4}, {1.0, 1.0, 0.0}};
        const std::vector<double> xs = linspace(0, 1, 400);
        const std::vector<sf::FitPoint> fitted = sf::fitPoints(xs, sf::curveAt(points, xs));
        QCOMPARE(fitted.front().x, 0.0);
        QCOMPARE(fitted.back().x, 1.0);
        QVERIFY(fitted.size() <= 6);
        const std::vector<double> a = sf::curveAt(fitted, xs), b = sf::curveAt(points, xs);
        for (std::size_t i = 0; i < xs.size(); ++i)
            QVERIFY(std::abs(a[i] - b[i]) <= sf::kTolerance + 1e-9);
        // numpy's: ((0, 0, -1), (0.15038, 0.00011, -0.5), (0.599, 0.79439, 0.1), (0.6391, 0.84599, 0.35), (1, 1, 0)).
        const std::vector<sf::FitPoint> expected{
            {0.0, 0.0, -1.0}, {0.15038, 0.00011, -0.5}, {0.599, 0.79439, 0.1}, {0.6391, 0.84599, 0.35}, {1.0, 1.0, 0.0}};
        QCOMPARE(fitted.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            QVERIFY(near(fitted[i].x, expected[i].x, 1e-9) && near(fitted[i].y, expected[i].y, 1e-9));
            QVERIFY(near(fitted[i].curve, expected[i].curve, 1e-9));
        }
        // Flat: the ends.
        const std::vector<sf::FitPoint> flat = sf::fitPoints(xs, std::vector<double>(400, 1.0));
        QVERIFY((flat == std::vector<sf::FitPoint>{{0.0, 1.0, 0.0}, {1.0, 1.0, 0.0}}));
    }

    void analyzeFitsTheKick() {
        const int pre = int(sf::kPreSeconds * kRate);
        std::vector<sf::Fit> fits;
        for (int character = 0; character < 3; ++character) {
            const auto fit = sf::analyze({kick(0.6, 0.12, pre)}, bass({55}), kRate, character, pre);
            QVERIFY(fit);
            fits.push_back(*fit);
        }
        const sf::Fit &tight = fits[0], &natural = fits[1], &loose = fits[2];
        QVERIFY(tight.length < natural.length && natural.length < loose.length);  // Loose keeps out of the way longer
        for (const sf::Fit& fit : fits) {
            QVERIFY(sf::kLengthMin <= fit.length && fit.length <= sf::kLengthMax);
            QVERIFY(fit.points.front().x == 0.0 && fit.points.front().y == 0.0);  // ducked from the hit
            QVERIFY(fit.points.back().x == 1.0 && fit.points.back().y == 1.0);  // and back by the end
            QVERIFY(fit.points.size() >= 3 && fit.points.size() <= std::size_t(sf::kMaxPoints));
            QVERIFY(near(maxOf(fit.envelope), 1.0, 1e-6) && fit.times.size() == fit.target.size());
            const std::vector<double> xs = linspace(0, 1, 400);
            const std::vector<double> curve = sf::curveAt(fit.points, xs);
            for (std::size_t i = 0; i < xs.size(); ++i) {
                // numpy.interp(xs * length, times, target, right=1.0)
                const double at = xs[i] * fit.length;
                double wanted = 1.0;
                if (at <= fit.times.back()) {
                    const auto above = std::upper_bound(fit.times.begin(), fit.times.end(), at);
                    const std::size_t j = std::min(std::size_t(above - fit.times.begin()), fit.times.size() - 1);
                    const std::size_t k = j == 0 ? 0 : j - 1;
                    const double span = fit.times[j] - fit.times[k];
                    wanted = span > 0 ? fit.target[k] + (fit.target[j] - fit.target[k]) * (at - fit.times[k]) / span
                                      : fit.target[j];
                }
                QVERIFY(std::abs(curve[i] - wanted) < 0.05);
            }
        }
        // The kick dies away (to -20 dB, Natural) after about 0.12 * ln(10) s from its peak.
        QVERIFY(250 < natural.length && natural.length < 420);
        // numpy's lengths, and the Natural fit's points.
        QCOMPARE(tight.length, 224.0);
        QCOMPARE(natural.length, 336.0);
        QCOMPARE(loose.length, 497.0);
        QCOMPARE(natural.times.size(), std::size_t(28800));
        QVERIFY(near(natural.envelope[1000], 0.28733635461367985, 1e-6));
        QVERIFY(near(natural.target[5000], 0.20592847846946327, 1e-6));
        const std::vector<sf::FitPoint> expected{{0.0, 0.0, 0.0},         {0.17544, 0.0, 0.0},
                                                 {0.67168, 0.68871, -0.125}, {0.7594, 0.74519, 0.025},
                                                 {0.92982, 0.99997, 1.0},   {1.0, 1.0, 0.0}};
        QCOMPARE(natural.points.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            QVERIFY2(near(natural.points[i].x, expected[i].x, 2e-5) && near(natural.points[i].y, expected[i].y, 2e-5),
                     qPrintable(QStringLiteral("point %1: %2, %3").arg(i).arg(natural.points[i].x).arg(natural.points[i].y)));
            QVERIFY(near(natural.points[i].curve, expected[i].curve, 1e-9));
        }

        QVERIFY(!sf::analyze({std::vector<double>(kRate, 0.0)}, bass({55}), kRate));  // nothing to fit to
        QVERIFY(!sf::analyze({}, bass({55}), kRate));
    }

    void captureHandsOutEachHit() {
        sf::Capture capture(kRate, 4.0);
        const int pre = int(sf::kPreSeconds * kRate);
        const int total = 3 * kRate;
        std::vector<float> key(std::size_t(total), 0.0f), inputs(std::size_t(total), 0.25f), phase(std::size_t(total), -1.0f);
        const int hits[2] = {kRate / 2, kRate / 2 + 12000};  // the second 250 ms after the first: the first is cut there
        for (int hit : hits) {
            for (int i = hit; i < hit + 2000; ++i)
                key[std::size_t(i)] = 0.9f;
            for (int i = 0; i < 4800; ++i)
                phase[std::size_t(hit + i)] = float(i);
        }
        std::vector<sf::Capture::Hit> taken;
        auto slice = [](const std::vector<float>& values, int from, int count) {
            return std::vector<float>(values.begin() + from, values.begin() + std::min<int>(from + count, int(values.size())));
        };
        for (int start = 0; start < total; start += 800) {  // as the displays come, a refresh at a time
            capture.feed(sf::Capture::Stream::Key, start, slice(key, start, 800));
            capture.feed(sf::Capture::Stream::Input, start, slice(inputs, start, 800));
            capture.feed(sf::Capture::Stream::Phase, start, slice(phase, start, 800));
            for (auto& hit : capture.hits())
                taken.push_back(std::move(hit));
        }
        QCOMPARE(capture.hitCount(), 2);
        QCOMPARE(taken.size(), std::size_t(2));
        const auto& first = taken[0];
        const auto& second = taken[1];
        QCOMPARE(first.first.size(), std::size_t(pre + 12000));
        QVERIFY(near(first.first[std::size_t(pre)], 0.9, 1e-6) && first.first[std::size_t(pre - 1)] == 0.0f);
        QCOMPARE(second.first.size(), std::size_t(pre + int(sf::kKickSeconds * kRate)));
        for (float value : first.second)
            QCOMPARE(value, 0.25f);
        QCOMPARE(first.second.size(), std::size_t(hits[0] + 12000));  // the input since it started (< 2 s)
        QVERIFY(near(capture.takeKeyPeak(), 0.9, 1e-6));
        QCOMPARE(capture.takeKeyPeak(), 0.0f);

        // After a gap (the editor hidden), it starts again from what comes.
        capture.feed(sf::Capture::Stream::Phase, 10 * kRate, {0.0f});
        capture.feed(sf::Capture::Stream::Key, 10 * kRate, {1.0f});
        QCOMPARE(capture.stream(sf::Capture::Stream::Key).start(), qint64(10 * kRate));
        QVERIFY(capture.hits().empty());
        QCOMPARE(capture.lastPhase(), 0.0);
        capture.feed(sf::Capture::Stream::Phase, 10 * kRate + 1, {});
        QCOMPARE(capture.lastPhase(), -1.0);  // nothing came: no playhead
    }
};

QTEST_GUILESS_MAIN(TestSidechainFit)
#include "test_sidechain_fit.moc"
