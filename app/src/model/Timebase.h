#pragma once
// Musical time helpers. Timeline positions are quarter-note beats.

#include <QString>

#include <array>
#include <optional>

namespace sub::app {

inline constexpr std::array<int, 6> kValidDenominators{1, 2, 4, 8, 16, 32};

struct TimeSignature {
    int numerator = 4;
    int denominator = 4;

    // Bar length in quarter-note beats.
    double beatsPerBar() const { return numerator * 4.0 / denominator; }
    // Length of one signature beat (the denominator note) in quarter notes.
    double beatLength() const { return 4.0 / denominator; }
    // "6/8".
    QString toString() const;

    friend bool operator==(const TimeSignature&, const TimeSignature&) = default;
};

// A position split into zero-based bar, beat and sixteenth.
struct BarPosition {
    int bar = 0;
    int beat = 0;
    int sixteenth = 0;

    friend bool operator==(const BarPosition&, const BarPosition&) = default;
};

double beatsToSeconds(double beats, double tempo);
double secondsToBeats(double seconds, double tempo);

// Zero-based (bar, beat, sixteenth) for a beat position.
BarPosition splitPosition(double beats, const TimeSignature& ts);
// Ableton-style "bars.beats.sixteenths", one-based.
QString formatPosition(double beats, const TimeSignature& ts);
// The inverse of formatPosition; missing parts default to 1. None if unparsable.
std::optional<double> parsePosition(const QString& text, const TimeSignature& ts);
bool isMultiple(double value, double step);
// Ruler label: "5" on a bar line, "5.3" on a beat, "5.3.2" below that.
QString formatBarLabel(double beats, const TimeSignature& ts);

// -70 dB (the faders' floor) and below is silence.
double dbToGain(double db);
double gainToDb(double gain);
QString formatDb(double db);
// "25L", "30R", "C" or a plain -50..50 number (negative = left) as -1..1.
std::optional<double> parsePan(const QString& text);
QString formatPan(double pan);

}  // namespace sub::app
