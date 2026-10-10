#pragma once
// Multiband Dynamics' gain law, as its editor draws and reads it out: the
// engine's own (engine/src/builtin/MultibandDesign.h), so what the graph shows
// a level getting is what plays, with the engine's ranges and display rate.
// And its ratios and times as typed: "1:4", "4:1", "0.5"; "250 ms", "1.5 s".

#include <QString>

#include <optional>

namespace sub::app {

// The device's thresholds' and ratios' ranges, and the audio per display value: the engine's
// (MultibandDesign.h), which the UI can't include.
double multibandMinThresholdDb();
double multibandMaxThresholdDb();
double multibandMinRatio();
double multibandMaxRatio();
int multibandDisplaySamples();

// The gain change (dB) the dynamics settle on for a steady level `levelDb` (dB), once attack and
// release are done: a band's Above and Below thresholds (dB) and ratios, Soft Knee, and Amount
// (0..100 %).
double multibandGainDb(double levelDb, double above, double aboveRatio, double below, double belowRatio,
                       bool softKnee, double amountPercent);

// A typed ratio R, as formatValue's `ratio` unit prints it, Live's way, "1:R" ("1:4", "1:4.00", " 1 : 0.5 ",
// "1:inf"), or the R alone ("4", "0.5"), or as a compressor writes it ("4:1" is 4; any "a:b" not starting with
// 1 is a / b); held to 0.25..100; 0 for text it can't read ("", "x", "1:0", "0", "-2"). Live's own field takes a
// bare number as 1 / R; here it is R, the number the box shows.
double multibandParseRatio(const QString& text);

// A typed time, in milliseconds (the attack's and release's unit): "250", "250 ms", "1.5 s", "1.5s"; a bare
// number is milliseconds. Nothing for text it can't read or a negative time.
std::optional<double> multibandParseMs(const QString& text);

}  // namespace sub::app
