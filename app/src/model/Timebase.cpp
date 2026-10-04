#include "model/Timebase.h"

#include "model/Numbers.h"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sub::app {

namespace {

// A whole number as typed: optional spaces and sign, digits (single
// underscores may group them, as in "1_000").
std::optional<long long> parseWhole(const QString& text) {
    const QString trimmed = text.trimmed();
    int i = 0;
    bool negative = false;
    if (i < trimmed.size() && (trimmed[i] == u'+' || trimmed[i] == u'-')) {
        negative = trimmed[i] == u'-';
        ++i;
    }
    if (i >= trimmed.size()) return std::nullopt;
    long long value = 0;
    bool lastDigit = false;
    for (; i < trimmed.size(); ++i) {
        const QChar c = trimmed[i];
        if (c >= u'0' && c <= u'9') {
            if (value > (std::numeric_limits<long long>::max() - 9) / 10) return std::nullopt;
            value = value * 10 + (c.unicode() - u'0');
            lastDigit = true;
        } else if (c == u'_' && lastDigit && i + 1 < trimmed.size() && trimmed[i + 1].isDigit()) {
            lastDigit = false;
        } else {
            return std::nullopt;
        }
    }
    return negative ? -value : value;
}

}  // namespace

QString TimeSignature::toString() const {
    return QStringLiteral("%1/%2").arg(numerator).arg(denominator);
}

double beatsToSeconds(double beats, double tempo) { return beats * 60.0 / tempo; }

double secondsToBeats(double seconds, double tempo) { return seconds * tempo / 60.0; }

BarPosition splitPosition(double beats, const TimeSignature& ts) {
    beats = std::max(0.0, beats) + 1e-9;
    const int bar = static_cast<int>(floorDiv(beats, ts.beatsPerBar()));
    const double inBar = beats - bar * ts.beatsPerBar();
    const int beat = std::min(static_cast<int>(floorDiv(inBar, ts.beatLength())), ts.numerator - 1);
    const double inBeat = inBar - beat * ts.beatLength();
    const int sixteenth = static_cast<int>(floorDiv(inBeat, 0.25));
    return {bar, beat, sixteenth};
}

QString formatPosition(double beats, const TimeSignature& ts) {
    const BarPosition p = splitPosition(beats, ts);
    return QStringLiteral("%1.%2.%3").arg(p.bar + 1).arg(p.beat + 1).arg(p.sixteenth + 1);
}

std::optional<double> parsePosition(const QString& text, const TimeSignature& ts) {
    QString cleaned = text.trimmed();
    cleaned.replace(u':', u'.');
    QStringList parts;
    for (const QString& part : cleaned.split(u'.')) {
        if (!part.isEmpty()) parts.append(part);
    }
    if (parts.isEmpty() || parts.size() > 3) return std::nullopt;
    long long values[3] = {1, 1, 1};
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const auto value = parseWhole(parts[i]);
        if (!value) return std::nullopt;
        values[i] = *value;
    }
    const auto [bar, beat, sixteenth] = values;
    if (bar < 1 || beat < 1 || sixteenth < 1) return std::nullopt;
    return static_cast<double>(bar - 1) * ts.beatsPerBar() + static_cast<double>(beat - 1) * ts.beatLength() +
           static_cast<double>(sixteenth - 1) * 0.25;
}

bool isMultiple(double value, double step) {
    const double ratio = value / step;
    return std::abs(ratio - roundHalfEven(ratio)) < 1e-6;
}

QString formatBarLabel(double beats, const TimeSignature& ts) {
    const BarPosition p = splitPosition(beats, ts);
    if (isMultiple(beats, ts.beatsPerBar())) return QString::number(p.bar + 1);
    if (isMultiple(beats, ts.beatLength())) return QStringLiteral("%1.%2").arg(p.bar + 1).arg(p.beat + 1);
    return QStringLiteral("%1.%2.%3").arg(p.bar + 1).arg(p.beat + 1).arg(p.sixteenth + 1);
}

double dbToGain(double db) { return db <= -70.0 ? 0.0 : std::pow(10.0, db / 20.0); }

double gainToDb(double gain) {
    return gain <= 0.0 ? -std::numeric_limits<double>::infinity() : 20.0 * std::log10(gain);
}

QString formatDb(double db) {
    return db <= -70.0 ? QStringLiteral("-inf dB") : formatFixed(db, 1) + QStringLiteral(" dB");
}

std::optional<double> parsePan(const QString& text) {
    QString cleaned = text.trimmed().toLower();
    if (cleaned == u"c" || cleaned == u"center" || cleaned == u"centre") return 0.0;
    double sign = 1.0;
    if (cleaned.endsWith(u'l')) {
        sign = -1.0;
        cleaned.chop(1);
    } else if (cleaned.endsWith(u'r')) {
        cleaned.chop(1);
    }
    bool ok = false;
    const double value = cleaned.toDouble(&ok);
    if (!ok) return std::nullopt;
    return std::max(-1.0, std::min(1.0, sign * value / 50));
}

QString formatPan(double pan) {
    const auto value = static_cast<long long>(roundHalfEven(pan * 50));
    if (value == 0) return QStringLiteral("C");
    return QString::number(std::llabs(value)) + (value < 0 ? u'L' : u'R');
}

}  // namespace sub::app
