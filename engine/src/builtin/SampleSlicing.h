#pragma once
// How the Sampler cuts its sample into slices (its Slice mode, as Ableton's
// Simpler): at the sample's transients, at beats, or into equal regions. The
// engine plays them and the Sampler's editor draws them, from these same
// functions (the application layer wraps them: app/src/audio/SampleSlices.h),
// so the slices drawn are the slices that play.
//
// Finding transients reads the whole sample: it runs once, when a sample is
// loaded (not on the audio thread). Each one found has a strength, so the
// sensitivity only chooses among them: sliceStarts() is real-time safe.
//
// Frames are counted in the order the sample plays: from its end when it plays
// reversed.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sub::slicing {

// The most slices a sample is cut into (keys from C1 up: more than there are).
inline constexpr int kMaxSlices = 128;
// The key the first slice plays on (C1).
inline constexpr int kFirstSliceKey = 36;

// A transient: the frame it starts at (just before its attack, at a zero
// crossing), and how strong it is, 0..1 of the strongest in the sample.
struct Onset {
    int64_t frame = 0;
    float strength = 0.f;
};

// The sample's transients, in order. `channels` are `numChannels` planar
// buffers of `frames` samples at `sampleRate`; `reversed`: of the sample played
// backwards (the frames counted from its end). Not real-time.
std::vector<Onset> detectOnsets(const float* const* channels, int numChannels, int64_t frames, double sampleRate,
                                bool reversed);

enum class SliceBy : int { Transient = 0, Beat, Region };

struct SliceSettings {
    SliceBy by = SliceBy::Transient;
    float sensitivity = 0.5f;    // Transient: 0..1; more finds weaker ones
    double regionBeats = 4.0;    // Beat: what Start..End lasts, in beats
    double divisionBeats = 0.5;  // Beat: a slice's length, in beats
    int regions = 8;             // Region: how many equal slices
};

// Where the slices of [start, end) start, in order; the first is always
// `start`. Writes at most kMaxSlices to `out` and returns how many. Transients
// count with a strength of at least 1 - sensitivity; none closer than 10 ms
// to another slice (or to `end`). Real-time safe.
int sliceStarts(const SliceSettings& settings, const Onset* onsets, size_t count, int64_t start, int64_t end,
                double sampleRate, int64_t* out);

// Snap: the zero crossing nearest `frame` (of the channels summed, frames as the
// sample plays) within `reach` frames either side, the earlier of two as near;
// `frame` itself if there is none, or at either end of the sample. Real-time safe.
int64_t nearestZeroCrossing(const float* const* channels, int numChannels, int64_t frames, bool reversed,
                            int64_t frame, int64_t reach);

// How far Snap looks for a zero crossing, in seconds.
inline constexpr double kSnapSeconds = 0.01;

// The lengths in beats of the Beat divisions, as the Sampler's list names them
// (1/16, 1/8, 1/4, 1/2, 1 Bar, 2 Bars, 4 Bars; a bar is 4 beats).
inline constexpr double kSliceDivisionBeats[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};

}  // namespace sub::slicing
