#pragma once
// Pure note-editing functions for the piano roll (no undo).

#include "model/Clip.h"

#include <QString>

#include <array>
#include <utility>
#include <vector>

class QRandomGenerator;

namespace sub::app::notes {

inline constexpr double kMinNoteBeats = 1.0 / 64;
inline constexpr double kEps = 1e-9;

// Quantize grids, as the piano roll offers them: (name, beats).
struct QuantizeGrid {
    const char* name;
    double beats;
};
inline constexpr std::array<QuantizeGrid, 6> kQuantizeGrids{{{"1/4", 1.0},
                                                             {"1/8", 0.5},
                                                             {"1/8T", 1.0 / 3},
                                                             {"1/16", 0.25},
                                                             {"1/16T", 1.0 / 6},
                                                             {"1/32", 0.125}}};
// At 100 % humanize, how far a note may move (a 32nd note) and its velocity change.
inline constexpr double kHumanizeBeats = 0.125;
inline constexpr int kHumanizeVelocity = 24;

// Ableton's octave numbering: note 60 (middle C) is C3.
QString noteName(int pitch);
bool isBlackKey(int pitch);

// Sort order: by start, then pitch.
bool byTime(const Note& a, const Note& b);

// How a clip stores its notes: sorted by start, then pitch; no exact duplicates.
std::vector<Note> normalize(const std::vector<Note>& notes);
// (earliest start, latest end) of some notes (there must be some).
std::pair<double, double> span(const std::vector<Note>& notes);
// Notes in `winners` keep their place. Any other note on the same key that
// overlaps one is shortened to end where the winner starts, starts where the
// winner ends, or goes if it is covered (like clips on a track).
std::vector<Note> resolveOverlaps(const std::vector<Note>& notes, const std::vector<Note>& winners);
// A clip's notes with those of `targets` deactivated (or, `active`, activated
// again), staying where they are.
std::vector<Note> withActive(const std::vector<Note>& notes, const std::vector<Note>& targets, bool active);
// A clip's notes with `removed` taken out and `added` put in; the added notes
// win where they overlap others.
std::vector<Note> place(const std::vector<Note>& notes, const std::vector<Note>& removed,
                        const std::vector<Note>& added);
// Limit a move of `notes` as a group: none before the content start or off the
// keyboard. (delta beats, delta pitch).
std::pair<double, int> clampMove(const std::vector<Note>& notes, double deltaBeats, int deltaPitch);
std::vector<Note> shifted(const std::vector<Note>& notes, double deltaBeats, int deltaPitch);

enum class Edge { Start, End };
// Move each note's `edge` by `delta` beats, keeping at least `minLength`; a
// moved start keeps the note's end and stays at or after 0.
std::vector<Note> resized(const std::vector<Note>& notes, Edge edge, double delta,
                          double minLength = kMinNoteBeats);
std::vector<Note> withVelocity(const std::vector<Note>& notes, double delta);
// Where notes on the same key overlap, the earlier one ends where the later
// starts (and goes if nothing is left of it); of two starting together, the
// longer stays. For notes changed together, which can't win against each other.
std::vector<Note> untangle(const std::vector<Note>& notes);
// Lengthen or shorten each target note to last until the next target starts (a
// chord's notes all reach the next chord); the last ones reach the next note in
// the clip after them, or `end`, the clip's end, if there is none. A note never
// runs into the next note on its own key.
std::vector<Note> legato(const std::vector<Note>& targets, const std::vector<Note>& clipNotes, double end);
// Stretch (factor 2) or squeeze (factor 0.5) the notes' timing: starts move away
// from or toward the earliest one, and lengths scale with them.
std::vector<Note> timeScaled(const std::vector<Note>& notes, double factor);
// Move each note's start toward the nearest multiple of `step` beats, all the
// way at `amount` 1, keeping its length.
std::vector<Note> quantized(const std::vector<Note>& notes, double step, double amount = 1.0);
// Nudge each note's start and velocity at random, as a player would: at
// `amount` 1 by up to kHumanizeBeats and kHumanizeVelocity either way, more
// often a little than a lot. Lengths stay.
std::vector<Note> humanized(const std::vector<Note>& notes, QRandomGenerator& rng, double amount);

}  // namespace sub::app::notes
