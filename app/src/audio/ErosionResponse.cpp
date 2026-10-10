#include "audio/ErosionResponse.h"

#include "builtin/ErosionDesign.h"
#include "model/Numbers.h"

#include <algorithm>
#include <cmath>

namespace sub::app {

static_assert(kErosionMinFrequency == sub::erosion::kMinFrequency &&
                  kErosionMaxFrequency == sub::erosion::kMaxFrequency,
              "the editor's Frequency range is the device's");
static_assert(kErosionMinWidth == sub::erosion::kMinWidth && kErosionMaxWidth == sub::erosion::kMaxWidth,
              "the editor's Filter Width range is the device's");
static_assert(kErosionFloorDb == sub::erosion::kFloorDb, "the editor's floor is the `erosion` display's");

QList<double> erosionBandMagnitude(double freq, double width, double sampleRate, const QList<double>& frequencies) {
    QList<double> magnitudes;
    magnitudes.reserve(frequencies.size());
    for (const double frequency : frequencies) {
        magnitudes.append(sub::erosion::bandMagnitude(freq, width, sampleRate, frequency));
    }
    return magnitudes;
}

QPair<double, double> erosionBandEdges(double freq, double width, double sampleRate) {
    const auto [low, high] = sub::erosion::bandEdges(freq, width, sampleRate);
    return {low, high};
}

double erosionModFrequency(double freq, double sampleRate) { return sub::erosion::modFrequency(freq, sampleRate); }

double erosionExcursionMs(double amount, double sampleRate) { return sub::erosion::excursionMs(amount, sampleRate); }

QString erosionExcursionText(double ms) {
    static const QString plusMinus = QStringLiteral("±");
    if (ms >= 0.9995) return plusMinus + formatFixed(ms, 2) + QStringLiteral(" ms");
    const double us = ms * 1000.0;
    if (us >= 9.95) return plusMinus + formatFixed(us, 0) + QStringLiteral(" µs");
    if (us > 0.0) return plusMinus + formatFixed(us, 1) + QStringLiteral(" µs");
    return plusMinus + QStringLiteral("0 µs");
}

QPair<double, double> erosionBlendWeights(double blend) {
    const sub::erosion::Weights weights = sub::erosion::blendWeights(blend);
    return {weights.sine, weights.noise};
}

double erosionPeakDb(const std::vector<float>& values) {
    double db = kErosionFloorDb;
    for (const float value : values) {
        if (std::isfinite(value))
            db = std::max(db, double(value));
    }
    return db;
}

}  // namespace sub::app
