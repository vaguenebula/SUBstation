#pragma once
// Multiband Dynamics' gain law, as its editor draws and reads it out: the
// engine's own (engine/src/builtin/MultibandDesign.h), so what the graph shows
// a level getting is what plays. And its ratios as typed: "4:1", "1:2", "0.5".

#include <QString>

namespace sub::app {

// The device's ranges (MultibandDesign.h's, checked against them where this is built).
inline constexpr double kMultibandMinThresholdDb = -80.0, kMultibandMaxThresholdDb = 0.0;
inline constexpr double kMultibandMinRatio = 0.25, kMultibandMaxRatio = 100.0;

// The gain change (dB) the dynamics settle on for a steady level `levelDb` (dB), once attack and
// release are done: a band's Above and Below thresholds (dB) and ratios, Soft Knee, and Amount
// (0..100 %).
double multibandGainDb(double levelDb, double above, double aboveRatio, double below, double belowRatio,
                       bool softKnee, double amountPercent);

// A typed ratio: "4", "4:1", "4.00:1", "1:2", "1:2.00", "0.5", " 1 : 2.00 " ("a:b" is a / b, so "1:2" is
// 0.5 and "2:1" is 2), held to 0.25..100; 0 for text it can't read ("", "x", "1:0", "0", "-2").
double multibandParseRatio(const QString& text);

}  // namespace sub::app
