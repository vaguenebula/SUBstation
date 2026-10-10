#pragma once
// The Erosion device's maths, as its editor draws and reads it out: the noise's
// band (its magnitude, its edges), the frequency the modulator plays, how far
// Amount moves the delay, Noise Blend's weights. Worked out by the engine's own
// functions (engine/src/builtin/ErosionDesign.h), so the band drawn is the
// filter that plays and the excursion read out is the one applied.

#include <QList>
#include <QPair>
#include <QString>

namespace sub::app {

// Frequency's and Filter Width's ranges (Hz, octaves): the device's parameters'
// (checked against the engine's in ErosionResponse.cpp), for the editor's drags.
inline constexpr double kErosionMinFrequency = 20.0;
inline constexpr double kErosionMaxFrequency = 18000.0;
inline constexpr double kErosionMinWidth = 0.1;
inline constexpr double kErosionMaxWidth = 10.0;

// The noise band's magnitude (0..1, 1 at its centre) at each of `frequencies`
// (Hz), as the engine's filter has it at `sampleRate`, for Frequency `freq` (Hz)
// and Filter Width `width` (octaves).
QList<double> erosionBandMagnitude(double freq, double width, double sampleRate, const QList<double>& frequencies);

// Its -3 dB edges (Hz).
QPair<double, double> erosionBandEdges(double freq, double width, double sampleRate);

// The frequency the modulator plays for `freq`: the same, kept below 0.45 of the
// sample rate.
double erosionModFrequency(double freq, double sampleRate);

// How far Amount (%) moves the delay either way, in ms (0 before the rate is known).
double erosionExcursionMs(double amount, double sampleRate);

// That as the graph reads it out: "±1.38 ms" from 1 ms, "±87 µs" from 10 µs,
// "±0.9 µs" below, "±0 µs" at 0.
QString erosionExcursionText(double ms);

// The device's latency in ms (2 ms, in whole samples).
double erosionLatencyMs(double sampleRate);

// Noise Blend's equal-power weights {sine, noise} for `blend` (%): exactly 1 and
// 0 at the ends, as the engine's.
QPair<double, double> erosionBlendWeights(double blend);

}  // namespace sub::app
