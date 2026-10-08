// Parts made from chords: block chords and a bass line, written as notes, and
// a progression to start from where a song has no chords yet. Rules, not a
// model: what melody and accompaniment will need (machine learning) comes later.
//
// - Chords: each a close voicing between C2 and G4 (Ableton's octave numbers:
//   60 is C3), the first in root position near D3, each next one the
//   inversion and octave that moves the voices least (so they stay put rather
//   than jump), struck again at every bar line.
// - Bass: each chord's bass note (an inversion's, else its root) in the
//   octave from C1 (36), struck with each chord and again at every bar line.
//
// Times are the chords' beats; bars start every barBeats beats from 0.

#pragma once

#include <vector>

#include "harmony/ChordInference.h"

namespace sub::intelligence::harmony {

struct GeneratedNote {
    int pitch = 60;
    double start = 0.0;
    double length = 0.0;
    int velocity = 100;

    friend bool operator==(const GeneratedNote&, const GeneratedNote&) = default;
};

inline constexpr int kChordLowest = 48;   // C2
inline constexpr int kChordHighest = 79;  // G4
inline constexpr int kChordCentre = 62;   // D3: where the first chord sits
inline constexpr int kBassLowest = 36;    // C1: the bass is in the octave from here
inline constexpr int kChordVelocity = 90;
inline constexpr int kBassVelocity = 100;

std::vector<GeneratedNote> chordPart(const std::vector<ChordSpan>& chords, double barBeats = 4.0);
std::vector<GeneratedNote> bassPart(const std::vector<ChordSpan>& chords, double barBeats = 4.0);

// A chord a bar from `start` to `end` (the first and last cut at them): I V vi
// IV in a major key, i VI III VII in a minor one, over and over.
std::vector<ChordSpan> starterProgression(const Key& key, double start, double end, double barBeats = 4.0);

}  // namespace sub::intelligence::harmony
