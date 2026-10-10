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

// A positive number, or infinity ("inf": Live's brick wall, "1:inf"); 0 for anything else.
double positive(const QString& text) {
    bool ok = false;
    const double value = QLocale::c().toDouble(text.trimmed(), &ok);
    return ok && value > 0.0 ? value : 0.0;  // (NaN isn't over 0)
}

}  // namespace

double multibandParseRatio(const QString& text) {
    const QStringList parts = text.trimmed().split(QLatin1Char(':'));
    double ratio = 0.0;
    if (parts.size() == 1) {
        ratio = positive(parts[0]);
    } else if (parts.size() == 2) {
        const double a = positive(parts[0]), b = positive(parts[1]);
        if (a > 0.0 && b > 0.0)
            ratio = a == 1.0 ? b : a / b;  // Live's "1:R"; otherwise in to out, as "4:1"
    }
    return ratio > 0.0 ? std::clamp(ratio, kMultibandMinRatio, kMultibandMaxRatio) : 0.0;
}

std::optional<double> multibandParseMs(const QString& text) {
    QString cleaned = text.trimmed().toLower();
    double scale = 1.0;
    if (cleaned.endsWith(QLatin1String("ms"))) {
        cleaned.chop(2);
    } else if (cleaned.endsWith(QLatin1Char('s'))) {
        cleaned.chop(1);
        scale = 1000.0;
    }
    bool ok = false;
    const double value = QLocale::c().toDouble(cleaned.trimmed(), &ok);
    if (!ok || !std::isfinite(value) || value < 0.0)
        return std::nullopt;
    return value * scale;
}

}  // namespace sub::app
