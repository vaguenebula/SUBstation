#pragma once
// The words of names: what a file, a preset, a rack or a track is called says
// what it is ("OS_LDNB_174_Amaj_Sad_Piano_Line": a sad piano line; "PL - Electric
// Flow": a pluck; "Cymbals_ClosedHiHat3": a closed hat), once its pack codes,
// tempos, keys and numbers are dropped.
//
// readWords() splits a name into words (at separators, camelCase and digits),
// joins those that go together ("hi hat", "future bass", "one shot"), and looks
// each up in a vocabulary of instruments, sounds, patterns, adjectives and
// genres. What a short code means depends on where it is: "PL", "BS", "LD" are
// a preset's categories (Serum's "PL - ...", "BS - ..."), but pack codes in a
// file's name. Words it doesn't know are kept apart (names: "Missme", "Dynasty"):
// they tell tracks apart when nothing else does.

#include <string>
#include <string_view>
#include <vector>

namespace sub::intelligence::labels {

// What part a sound plays in a song.
enum class Family { None, Drums, Bass, Keys, Synth, Strings, Brass, Woodwinds, Vocals, Guitar, Mallets, Fx, Ambience, Mix };

enum class WordKind {
    Instrument,  // what makes the sound: "Piano", "Violins", "Kick", "Vocal"
    Sound,       // a kind of (synthesized) sound: "Pluck", "Pad", "Lead", "Bass", "Stab", "Riser"
    Pattern,     // what it plays: "Chords", "Arp", "Loop", "Line", "Fill"
    Adjective,   // how it sounds: "Soft", "Dark", "Washed Out"
    Genre,       // a style: "Future Bass", "DnB" (not what the sound is)
};

struct Word {
    std::string display;  // as a label shows it: "Hi-Hat", "Washed Out"
    WordKind kind = WordKind::Adjective;
    Family family = Family::None;
    // Among nouns, how specific: "Closed Hat" 4 over "Hi-Hat" 3 over "Drums" 1.
    int rank = 0;

    friend bool operator==(const Word&, const Word&) = default;
};

// Where a name comes from: which codes it may hold.
enum class Source {
    Name,    // a track's, a rack's, a plug-in's
    File,    // an audio file's (its stem; a path's folders are not read)
    Preset,  // a plug-in's preset: "PL", "BS", "LD"... are its categories
};

struct Reading {
    std::vector<Word> words;           // in order
    std::vector<std::string> unknown;  // the words it doesn't know, lower case, in order
};

Reading readWords(std::string_view text, Source source);

// A name's words, lower case: split at separators, at camelCase and between
// letters and digits ("BellStab1" -> "bell", "stab", "1"). Hashes are dropped.
std::vector<std::string> splitWords(std::string_view text);

// The family's id ("drums", "" for None) and how a group of it is called ("Drum").
std::string familyId(Family family);
std::string familyTitle(Family family);

// "Washed Out" for "washed out": each word's first letter upper case.
std::string titleCase(std::string_view text);
// Lower case (ASCII letters); and lower case, letters and digits only ("Dry/Wet": "drywet").
std::string lowered(std::string_view text);
std::string squeezed(std::string_view text);

}  // namespace sub::intelligence::labels
