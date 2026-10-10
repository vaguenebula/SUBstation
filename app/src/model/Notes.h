#pragma once
// Pure note-editing functions for the piano roll (no undo).

#include "model/Clip.h"

#include <QString>

#include <array>
#include <optional>
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
// At 100 % Humanize › Timing, how far a note may move (a 32nd note).
inline constexpr double kHumanizeBeats = 0.125;

// Bends: how far a note bends either way (the engine's sub::kMaxBendSemitones,
// MIDI 2.0's per-note range), the shortest vibrato, and a new vibrato's rate
// (cycles a second), depth (semitones) and ramp (the share of its length it
// takes to reach its depth).
inline constexpr double kMaxBendSemitones = 48.0;
inline constexpr double kMinVibratoBeats = 1.0 / 32;
inline constexpr double kDefaultVibratoRate = 5.5;
inline constexpr double kMinVibratoRate = 0.5;  // what the vibrato tool draws (cycles a second)
inline constexpr double kMaxVibratoRate = 20.0;
inline constexpr double kDefaultVibratoDepth = 0.5;
inline constexpr double kDefaultVibratoFade = 0.3;

// Ableton's octave numbering: note 60 (middle C) is C3.
QString noteName(int pitch);
bool isBlackKey(int pitch);

// Sort order: by start, then pitch.
bool byTime(const Note& a, const Note& b);
// A total order on notes: by start, pitch, length, velocity, deactivated last,
// then by their bends (two notes alike but for their bends are two notes).
bool lessFull(const Note& a, const Note& b);

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
// Nudge each note's start at random, as a player would: at `amount` 1 by up
// to kHumanizeBeats either way, more often a little than a lot. Lengths and
// velocities stay (velocities are the velocity model's: app::Humanizer).
std::vector<Note> humanizedTiming(const std::vector<Note>& notes, QRandomGenerator& rng, double amount);

// --- Bends (MIDI 2.0's per-note pitch bend) ------------------------------------------------

// A note whose start moves to `start` (its end staying, or not: the caller's
// length), its bend staying where it was in time: its points and vibratos move
// back by as much (a point before the note's start then sets where it starts).
Note withStart(Note note, double start);
// The note with a point added to its bend (kept sorted; after any other at its
// time), held to the bend's range. Returns where the point went.
Note withBendPoint(Note note, BendPoint point, int* index = nullptr);
// The note with the points at `indices` moved by `deltaTime` beats and
// `deltaSemitones` together: they stay in order between the points not moved,
// and within the note (0..its length); values held to the range.
Note withBendPointsMoved(Note note, const std::vector<int>& indices, double deltaTime, double deltaSemitones);
Note withoutBendPoints(Note note, const std::vector<int>& indices);
// The segment from point `index` bent by `curve` (-1..1).
Note withBendCurve(Note note, int index, double curve);
// The note with a vibrato added: it takes the stretch it covers from any
// already there (they are shortened, split, or go), held to the note.
Note withVibrato(Note note, Vibrato vibrato);
Note withoutVibrato(Note note, int index);
// The note a slide from `note` goes to: the next of `notes` to start after it
// (later, not with it; of several starting together, the nearest in pitch,
// then the lower), deactivated ones left out. None if nothing follows it.
std::optional<Note> nextNote(const std::vector<Note>& notes, const Note& note);
// The note with a slide (a glissando) over `start`..`end` (beats from its
// start, held to it): from where its curve is at `start` to `semitones` at
// `end`, bent by `curve` (-1..1, as a segment's), points it covers replaced and
// those after it kept. A point at each end, so the slide is ordinary bend.
Note withSlide(Note note, double start, double end, double semitones, double curve);
// Few of a bend's many points (a recorded one's) that draw it as closely as
// `tolerance` semitones: the first and last stay, and so does every point a
// straight line between those kept around it would miss by more
// (Ramer-Douglas-Peucker).
std::vector<BendPoint> simplifiedBend(const std::vector<BendPoint>& points, double tolerance = 0.05);

}  // namespace sub::app::notes
