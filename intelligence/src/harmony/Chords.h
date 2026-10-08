// Chords and keys as the harmony analysis names them: pitch classes, the
// chord qualities it recognises (each a template of intervals above its root),
// keys and their scales.
//
// Names are spelled as the application spells keys: flats for the black keys
// but F# (C, C#, D, Eb, E, F, F#, G, Ab, A, Bb, B), so "Ebmaj7", "F#m", "C/E".

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace sub::intelligence::harmony {

// 0 = C ... 11 = B, for any MIDI pitch (negative ones too).
inline int pitchClass(int pitch) { return ((pitch % 12) + 12) % 12; }
// "C", "C#", "D", "Eb", ...
const char* noteName(int pitchClass);

// The chords recognised: triads, suspended chords and the common sevenths.
// (Sixth chords are their relative minor sevenths with another bass: C6 is
// Am7/C.)
enum class Quality : uint8_t {
    Major,
    Minor,
    Diminished,
    Augmented,
    Sus2,
    Sus4,
    Dominant7,
    Major7,
    Minor7,
    HalfDiminished7,
    Diminished7,
};
inline constexpr int kQualities = 11;

struct QualityInfo {
    const char* suffix;            // after the root: "", "m", "dim", "aug", "sus2", "sus4", "7", "maj7", "m7"...
    std::array<int, 4> intervals;  // semitones above the root, the root first; `size` of them count
    int size;
};
const QualityInfo& qualityInfo(Quality quality);

struct Chord {
    int root = 0;  // pitch class
    Quality quality = Quality::Major;
    int bass = -1;  // an inversion's bass (a chord tone, not the root); -1: the root

    // Its pitch classes as bits (bit 0: C).
    uint16_t pitchClasses() const;
    bool contains(int pitchClass) const { return (pitchClasses() >> pitchClass) & 1; }
    // The pitch class the bass plays: the inversion's, else the root.
    int bassClass() const { return bass < 0 ? root : bass; }
    // Its tones from the root up as pitch classes ("C E G Bb"); `size` of them.
    std::array<int, 4> tones() const;
    int size() const { return qualityInfo(quality).size; }
    // "Am7", "C/E", "F#dim".
    std::string name() const;

    friend bool operator==(const Chord&, const Chord&) = default;
};

struct Key {
    int tonic = 0;  // pitch class
    bool minor = false;

    // Whether a pitch class is in its scale (major, or natural minor).
    bool contains(int pitchClass) const;
    // Its scale's pitch classes as bits (bit 0: C).
    uint16_t scale() const;
    // The triad on a degree of its scale (0: the tonic's .. 6), as the scale has it.
    Chord triad(int degree) const;
    // Whether every note of a chord is in its scale.
    bool isDiatonic(const Chord& chord) const { return (chord.pitchClasses() & ~scale()) == 0; }
    // "A minor", "Eb major".
    std::string name() const;

    friend bool operator==(const Key&, const Key&) = default;
};

}  // namespace sub::intelligence::harmony
