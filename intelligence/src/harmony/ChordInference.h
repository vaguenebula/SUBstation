// Inferring a song's chords and key from its MIDI notes.
//
// Cheap enough to run again on every edit (a song of a few thousand notes takes
// well under a millisecond, with no threads and nothing kept between calls):
//
// - The key: Krumhansl and Kessler's key profiles correlated with how long each
//   pitch class sounds, counting again how long it is the bass (estimateKey).
// - The chords: the song in steps (an eighth note by default). Each step's notes
//   make a pitch-class profile (how long each sounds, louder notes counting a
//   little more) and a bass profile (which pitch class is lowest, for how long,
//   counting less the higher it is). Every chord (12 roots, each quality of
//   Chords.h) is scored against it: the share of the profile it explains, less
//   half of what it doesn't, less a little for each of its notes that is
//   missing (more for the root and a seventh, less for the fifth), plus a bonus
//   when the bass is its root (less when it is another of its notes); triads
//   are preferred to the rest, and chords of the key a little. A step with
//   little sounding (a lone melody note) weighs less than a full one, and one
//   with fewer than three pitch classes (a melody, a bass line) leans on how
//   likely each chord is in the key: the primary triads (I, IV, V) most,
//   suspended, augmented and diminished sevenths not at all.
// - Smoothing: the chord sequence that scores best overall, where changing
//   chord costs something, least on a bar line, more on half a bar, more on a
//   beat, most between beats (a Viterbi search over the 132 chords and "no
//   chord", in steps × chords). So a passing note doesn't make a chord of its
//   own, a chord played clearly changes where it is played, and a long silence
//   (more than about a bar and a half) has no chord.
// - Each chord's bass is the pitch class lowest under it the longest: one of
//   its notes other than its root makes it an inversion ("C/E"), if it is the
//   bass under at least half of it in a bass register (a melody's lowest notes
//   make no inversion).

#pragma once

#include <optional>
#include <vector>

#include "harmony/Chords.h"

namespace sub::intelligence::harmony {

// A note, in beats (quarter notes) from any fixed point (the song's start).
struct Note {
    int pitch = 60;  // MIDI note number
    double start = 0.0;
    double end = 0.0;
    int velocity = 100;  // 1..127
};

// A chord and when it sounds, in the notes' beats.
struct ChordSpan {
    double start = 0.0;
    double end = 0.0;
    Chord chord;

    friend bool operator==(const ChordSpan&, const ChordSpan&) = default;
};

struct InferenceOptions {
    double step = 0.5;      // beats a step (an eighth note); longer songs than kMaxSteps steps get longer steps
    double barBeats = 4.0;  // bars start every barBeats beats from 0: chords change on bar lines most readily
    std::optional<Key> key; // the song's key, if known (else estimated from the notes)
};

struct Harmony {
    std::optional<Key> key;          // the key given, or estimated (none: too little to tell)
    std::vector<ChordSpan> chords;   // by time, not overlapping; nothing where no chord sounds
};

inline constexpr int kMaxSteps = 1 << 16;
// Too little to tell a key from: fewer beats of notes than this, or fewer than
// three pitch classes.
inline constexpr double kMinKeyBeats = 2.0;

// The key the notes are most likely in; none if they are too few.
std::optional<Key> estimateKey(const std::vector<Note>& notes);

// The notes' key (the one given, or estimated) and chords.
Harmony inferHarmony(const std::vector<Note>& notes, const InferenceOptions& options = {});

}  // namespace sub::intelligence::harmony
