#pragma once
// The Sampler's slices and snapping, as its editor draws them: the engine's own
// (engine/src/builtin/SampleSlicing.h), over the same decoded sample (a
// Waveform is the engine's AudioSource), so the slices drawn are the ones that
// play and a snapped marker is drawn where the sample starts.
//
// Frames are counted in the order the sample plays: from its end reversed.

#include <QtGlobal>

#include <vector>

#include "audio/Waveform.h"

namespace sub::app::sampleSlices {

// A transient: the frame it starts at, and how strong it is (0..1 of the strongest).
struct Transient {
    qint64 frame = 0;
    float strength = 0.f;
};

// The sample's transients, played forwards or `reversed`. Reads the whole
// sample: worth keeping.
std::vector<Transient> transients(const Waveform& waveform, bool reversed);

// The Sampler's Slice By list.
enum class SliceBy { Transient = 0, Beat, Region };

struct Settings {
    SliceBy by = SliceBy::Transient;
    double sensitivity = 0.5;    // Transient: 0..1
    double regionBeats = 4.0;    // Beat: what Start..End lasts, in beats
    double divisionBeats = 0.5;  // Beat: a slice, in beats
    int regions = 8;             // Region
};

// Where the slices of [start, end) start, the first at `start`, as the Sampler cuts them.
std::vector<qint64> sliceStarts(const Settings& settings, const std::vector<Transient>& transients, qint64 start,
                                qint64 end, double sampleRate);

// The Slice Division list's lengths in beats (1/16 .. 4 Bars), by index (clamped).
double divisionBeats(int index);
// The key the first slice plays on (C1), and the most slices there are.
int firstSliceKey();
int maxSlices();

// Snap: the zero crossing nearest `frame` (within the Sampler's reach), as the Sampler snaps Start, End,
// Loop Start and slices.
qint64 nearestZeroCrossing(const Waveform& waveform, bool reversed, qint64 frame);

}  // namespace sub::app::sampleSlices
