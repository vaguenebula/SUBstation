#pragma once
// What a track's notes say about the part it plays: how low or high, how many
// at once, how long, how fast. A bass line is low and one note at a time; chords
// are three notes or more struck together; an arp is one fast note after another
// over a wide range; a pad holds its chords for bars.

#include <string>
#include <vector>

#include "labels/Facts.h"

namespace sub::intelligence::labels {

struct NoteProfile {
    int count = 0;
    int lowest = 0, highest = 0;  // the 5th and 95th percentile pitches (a stray note doesn't count)
    double medianPitch = 0.0;     // weighted by how long each sounds
    double medianLength = 0.0;    // beats
    double voices = 0.0;          // notes sounding at each note's start, on average
    double chordShare = 0.0;      // of the onsets, those where three notes or more start together
    double notesPerBeat = 0.0;    // onsets per beat while it plays
    int distinctPitches = 0;
};

NoteProfile profile(const std::vector<NoteFact>& notes);

// What the notes play, if they say: "Chords", "Arp", "Line" (one note at a time), "" if nothing clear.
// `sound` is the kind of sound they suggest: "Bass", "Sub Bass", "Lead", "Pad", "Stab", "" if none.
struct NoteReading {
    std::string pattern;
    std::string sound;
    double confidence = 0.0;  // 0..1
};
NoteReading readNotes(const NoteProfile& profile);

// "C3" for 60 (as Ableton names notes).
std::string noteName(int pitch);
// "chords, C2-G4, 1/2-beat notes": what the notes are, for the details.
std::string describe(const NoteProfile& profile);

}  // namespace sub::intelligence::labels
