#pragma once
// The Phaser-Flanger's response and LFO, as its editor draws them: worked out
// from the engine's own maths (engine/src/builtin/PhaserDesign.h: the stages,
// the comb, Warmth, Safe Bass, the modulation's mappings and the LFO's shapes),
// so the curve drawn is what plays.

#include <QList>
#include <QStringList>
#include <QtGlobal>

namespace sub::app {

// What plays now, as a linear filter (Warmth's saturation left out).
struct PhaserCurve {
    int mode = 0;  // 0 Phaser, 1 Flanger, 2 Doubler
    int notches = 4;
    double centerHz = 1000.0, q = 0.866;  // Phaser: the stages as tuned now
    double delayMs = 2.5;                 // Flanger, Doubler: the delay now
    double feedback = 0.0;                // the signed loop gain (phaserFeedbackGain)
    double warmth = 0.0;                  // 0..1
    double mix = 0.5;                     // 0..1
    double safeBassHz = 0.0;              // at or below phaserRanges().safeBassOffHz: off
    double outputDb = 0.0;
};

// The level in dB (floored at -120) at each of `frequencies` (Hz; above Nyquist: Nyquist's).
QList<double> phaserResponseDb(const PhaserCurve& curve, double sampleRate, const QList<double>& frequencies);

// The curve the editor draws over `columns` columns from lowHz to highHz (log): the polyline
// (each notch at its depth; where a comb is finer than a column, its top), and per column the
// highest and lowest the response reaches in it, whether it is that fine (`dense`), and how far
// the wet path turns its phase across it (`turn`, radians: a cycle of the comb is 2π, so a
// column turning 2π / 8 shows a cycle every 8 columns).
struct PhaserCurvePoints {
    QList<double> lineHz, lineDb;
    QList<double> top, bottom;
    QList<bool> dense;
    QList<double> turn;
};
PhaserCurvePoints phaserCurvePoints(const PhaserCurve& curve, double lowHz, double highHz, int columns,
                                    double sampleRate);

// Where the Phaser's notches are with no feedback (rising, Hz), and a comb's (the first `limit` below highHz).
QList<double> phaserNotchFrequencies(int notches, double centerHz, double q, double sampleRate);
QList<double> phaserCombNotchFrequencies(double delayMs, double highHz, int limit);

// The LFO's value (-1..1) of waveform `wave` (the parameter's index) at `phase` (0..1) of cycle
// `cycle`, bent by `duty` (-1..1); `rateHz` shapes Triangle Analog.
double phaserLfoValue(int wave, double phase, double duty, double rateHz, quint32 cycle);
// Whether a waveform's values come by chance (Random, Random S&H): drawn as a trace, not a shape.
bool phaserWaveIsRandom(int wave);
// A synced Rate (its index) in Hz at `tempo`.
double phaserSyncedRateHz(int division, double tempo);

// The modulation's mappings, as the engine has them (mod: the summed modulation, -2..2).
double phaserQ(double spreadPercent, double blend = 0.0, double mod = 0.0);
double phaserCenterHz(double centerHz, double blend, double mod, double sampleRate);
double phaserDelayMs(int mode, double timeMs, double mod, double sampleRate);
double phaserFeedbackGain(double percent, bool invert);

QStringList phaserWaveLabels();

// The engine's constants its editor needs: the frames a display value stands for, and the
// parameters' ranges, which its drags hold to.
int phaserDisplaySamples();
struct PhaserRanges {
    int maxNotches = 0;
    double minCenterHz = 0.0, maxCenterHz = 0.0;
    double minFlangeMs = 0.0, maxFlangeMs = 0.0;
    double minDoublerMs = 0.0, maxDoublerMs = 0.0;
    double safeBassOffHz = 0.0;  // Safe Bass at (or below) this is off
};
PhaserRanges phaserRanges();

}  // namespace sub::app
