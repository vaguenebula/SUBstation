#include "audio/PhaserResponse.h"

#include "builtin/PhaserDesign.h"

#include <algorithm>
#include <cmath>

namespace sub::app {

namespace {

sub::phaser::Mode modeOf(int mode) {
    return static_cast<sub::phaser::Mode>(std::clamp(mode, 0, 2));
}

sub::phaser::Response responseOf(const PhaserCurve& curve) {
    sub::phaser::Response r;
    r.mode = modeOf(curve.mode);
    r.notches = curve.notches;
    r.centerHz = curve.centerHz;
    r.q = curve.q;
    r.delayMs = curve.delayMs;
    r.feedback = curve.feedback;
    r.warmth = curve.warmth;
    r.mix = curve.mix;
    r.safeBassHz = curve.safeBassHz;
    r.outputDb = curve.outputDb;
    return r;
}

}  // namespace

QList<double> phaserResponseDb(const PhaserCurve& curve, double sampleRate, const QList<double>& frequencies) {
    const sub::phaser::Response r = responseOf(curve);
    QList<double> out;
    out.reserve(frequencies.size());
    for (const double f : frequencies)
        out.append(sub::phaser::responseDb(r, std::min(f, 0.499 * sampleRate), sampleRate));
    return out;
}

PhaserCurvePoints phaserCurvePoints(const PhaserCurve& curve, double lowHz, double highHz, int columns,
                                    double sampleRate) {
    sub::phaser::Curve worked;
    sub::phaser::curve(responseOf(curve), lowHz, highHz, columns, sampleRate, worked);
    PhaserCurvePoints out;
    out.lineHz.reserve(qsizetype(worked.line.size()));
    out.lineDb.reserve(qsizetype(worked.line.size()));
    for (const sub::phaser::CurvePoint& point : worked.line) {
        out.lineHz.append(point.freqHz);
        out.lineDb.append(point.db);
    }
    out.top = QList<double>(worked.top.begin(), worked.top.end());
    out.bottom = QList<double>(worked.bottom.begin(), worked.bottom.end());
    out.dense.reserve(qsizetype(worked.dense.size()));
    for (const uint8_t dense : worked.dense) out.dense.append(dense != 0);
    // Each column's turn, as curve() works it out to tell the dense ones.
    const sub::phaser::Response r = responseOf(curve);
    const int count = std::max(1, columns);
    const double nyquist = 0.499 * sampleRate;
    const double delaySeconds = sub::phaser::delaySamples(r.delayMs, sampleRate) / sampleRate;
    const int notches = std::clamp(r.notches, 1, sub::phaser::kMaxNotches);
    out.turn.reserve(count);
    for (int c = 0; c < count; ++c) {
        const double a = std::min(lowHz * std::pow(highHz / lowHz, double(c) / count), nyquist);
        const double b = std::min(c + 1 >= count ? highHz : lowHz * std::pow(highHz / lowHz, double(c + 1) / count),
                                  nyquist);
        out.turn.append(r.mode == sub::phaser::Mode::Phaser
                            ? notches * std::abs(sub::phaser::stagePhase(b, r.centerHz, r.q, sampleRate) -
                                                 sub::phaser::stagePhase(a, r.centerHz, r.q, sampleRate))
                            : 2.0 * sub::phaser::kPi * (b - a) * delaySeconds);
    }
    return out;
}

QList<double> phaserNotchFrequencies(int notches, double centerHz, double q, double sampleRate) {
    const std::vector<double> found = sub::phaser::notchFrequencies(notches, centerHz, q, sampleRate);
    return QList<double>(found.begin(), found.end());
}

QList<double> phaserCombNotchFrequencies(double delayMs, double highHz, int limit) {
    const std::vector<double> found = sub::phaser::combNotchFrequencies(delayMs, highHz, limit);
    return QList<double>(found.begin(), found.end());
}

double phaserLfoValue(int wave, double phase, double duty, double rateHz, quint32 cycle) {
    const auto shape = static_cast<sub::phaser::Wave>(std::clamp(wave, 0, 9));
    return sub::phaser::waveValue(shape, phase - std::floor(phase), cycle, duty, rateHz);
}

bool phaserWaveIsRandom(int wave) {
    const auto shape = static_cast<sub::phaser::Wave>(std::clamp(wave, 0, 9));
    return shape == sub::phaser::Wave::Random || shape == sub::phaser::Wave::RandomHold;
}

double phaserSyncedRateHz(int division, double tempo) { return sub::phaser::syncedRateHz(division, tempo); }

double phaserQ(double spreadPercent, double blend, double mod) {
    return sub::phaser::phaserQ(spreadPercent / 100.0, blend, mod);
}

double phaserCenterHz(double centerHz, double blend, double mod, double sampleRate) {
    return sub::phaser::phaserCenterHz(centerHz, blend, mod, sampleRate);
}

double phaserDelayMs(int mode, double timeMs, double mod, double sampleRate) {
    return sub::phaser::delayMs(modeOf(mode), timeMs, mod, sampleRate);
}

double phaserFeedbackGain(double percent, bool invert) { return sub::phaser::feedbackGain(percent, invert); }

QStringList phaserWaveLabels() {
    QStringList labels;
    for (const std::string& label : sub::phaser::waveLabels()) labels.append(QString::fromStdString(label));
    return labels;
}

}  // namespace sub::app
