#pragma once
// The Limiter device's levels, as its editor draws them: worked out from the
// engine's own maths (engine/src/builtin/LimiterDesign.h), so the line drawn is
// where the device limits, and Soft Clip's band where it rounds off.
//
// The device works around a line: the Ceiling, or with Maximize the Threshold.
// Its input display is in the line's domain (with Maximize, the raw input; else
// the input after Gain); its output display is in dBFS, at most the Ceiling, or
// with Maximize the Output.

namespace sub::app {

// Where the editor's line is (dB) and what to take off an output level (dBFS)
// to draw it in the line's domain (so the loudest output meets the line):
// without Maximize (the ceiling, 0); with it (the threshold, output - threshold).
struct LimiterLine {
    double lineDb = 0.0;
    double outputShiftDb = 0.0;
};
LimiterLine limiterLine(bool maximize, double gainDb, double ceilingDb, double thresholdDb, double outputDb);

// Soft Clip's knee, relative to the line: where it starts rounding off (-6.02 dB) and where it reaches
// the line (+3.52 dB: louder peaks are limited).
double limiterSoftKneeDb();
double limiterSoftTopDb();
// Soft Clip's curve: a level in dB relative to the line, the level that comes out.
double limiterSoftClipDb(double inDb);

// The device's displays: how many samples each value covers (128), and the level it publishes for
// silence (-90 dB).
int limiterMeterSamples();
double limiterFloorDb();

}  // namespace sub::app
