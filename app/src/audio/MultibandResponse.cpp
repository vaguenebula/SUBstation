#include "audio/MultibandResponse.h"

#include "builtin/MultibandDesign.h"

#include <QLocale>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace sub::app {

namespace mb = sub::multiband;

static_assert(kMultibandMinThresholdDb == double(mb::kMinThresholdDb) &&
              kMultibandMaxThresholdDb == double(mb::kMaxThresholdDb));
static_assert(kMultibandMinRatio == double(mb::kMinRatio) && kMultibandMaxRatio == double(mb::kMaxRatio));

double multibandGainDb(double levelDb, double above, double aboveRatio, double below, double belowRatio,
                       bool softKnee, double amountPercent) {
    const double amount = std::clamp(amountPercent / 100.0, 0.0, 1.0);
    return mb::staticGainDb(float(levelDb), float(above), float(aboveRatio), float(below), float(belowRatio), softKnee,
                            float(amount));
}

namespace {

// A positive, finite number (0 for anything else).
double positive(const QString& text) {
    bool ok = false;
    const double value = QLocale::c().toDouble(text.trimmed(), &ok);
    return ok && std::isfinite(value) && value > 0.0 ? value : 0.0;
}

}  // namespace

double multibandParseRatio(const QString& text) {
    const QStringList parts = text.trimmed().split(QLatin1Char(':'));
    double ratio = 0.0;
    if (parts.size() == 1) {
        ratio = positive(parts[0]);
    } else if (parts.size() == 2) {
        const double a = positive(parts[0]), b = positive(parts[1]);
        ratio = a > 0.0 && b > 0.0 ? a / b : 0.0;
    }
    return ratio > 0.0 ? std::clamp(ratio, kMultibandMinRatio, kMultibandMaxRatio) : 0.0;
}

}  // namespace sub::app
