#pragma once
// The Chorus-Ensemble's voices, as its editor draws them: where each delay
// sits and how it moves, worked out by the engine's own maths
// (engine/src/builtin/ChorusDesign.h), so the delays drawn are the delays that
// play. In the parameters' units: indices for the lists, percent, degrees.

namespace sub::app {

// The Rate's range (Hz), the engine's.
double chorusMinRate();
double chorusMaxRate();
// Samples per value of the device's displays (`phase`, `level`), the engine's.
int chorusDisplaySamples();

// The parameters' indices: mode 0 Chorus, 1 Ensemble, 2 Vibrato; taps 0 (one)
// or 1 (two); time 0 (Auto) to 5 (50 ms).
struct ChorusLayout {
    int mode = 0;
    int taps = 1;
    int time = 0;
};

// The same voices: the engine's layouts compared once normalised (Taps and
// Time only count in Chorus mode).
bool operator==(const ChorusLayout& a, const ChorusLayout& b);

// Voices a side.
int chorusVoices(const ChorusLayout& layout);
// The unmodulated delay in ms (Auto's grows with the Amount).
double chorusCentreMs(const ChorusLayout& layout, double amountPercent);
// How far the delay moves either way of it, in ms.
double chorusSwingMs(const ChorusLayout& layout, double amountPercent);
// The delay's range at Amount 100 (the editor's axis).
double chorusLowestMs(const ChorusLayout& layout);
double chorusHighestMs(const ChorusLayout& layout);
// A voice's place in the modulation's cycle (cycles, added to the LFO's phase),
// on `channel` 0 (left) or 1 (right); `offsetDegrees` is Vibrato's Offset.
double chorusVoicePhase(const ChorusLayout& layout, int channel, int voice, double offsetDegrees);
// The delay (ms) at absolute LFO phase `phase` (the LFO's phase plus the voice's).
double chorusDelayMs(const ChorusLayout& layout, double amountPercent, double shapePercent, double phase);
// The largest detune either way (the down one, the larger), in cents: the readout's.
double chorusPeakDetuneCents(const ChorusLayout& layout, double rateHz, double amountPercent, double shapePercent);

}  // namespace sub::app
