#pragma once
// What the labeller is told about a project's tracks (TrackFacts), and what it
// says about each (TrackLabel). Plain values, no Qt: the application gathers
// them from its project, its engine and its plug-in index (app/src/intelligence/
// TrackLabels), and the same facts always give the same labels.
//
// Text is UTF-8. Times are in beats on the song's timeline; pitches are MIDI
// note numbers (60 = C3, as in Ableton).

#include <string>
#include <vector>

namespace sub::intelligence::labels {

// A device's parameter, as the device has it now.
struct ParamFact {
    std::string id;    // "mix"; a plug-in's: its VST3 parameter id
    std::string name;  // "Dry/Wet"
    double value = 0.0;       // in its own units (a plug-in's: 0..1, or a step of a list)
    double normalized = 0.0;  // 0..1 over its range
    std::string unit;  // "%", "dB", "ms", "Hz"... ("" if none)
    std::string text;  // as the device shows the value ("35.0 %"; "" if it doesn't say)
};

// A device on a track: a built-in one (`kind` is its kind: "synth", "delay"...),
// a plug-in (kind "plugin") or a rack (kind "rack", with its chains).
struct DeviceFact {
    std::string kind;
    std::string name;      // a plug-in's ("Serum 2"), a built-in device's ("Delay"), a rack's own ("" if none)
    std::string vendor;    // a plug-in's
    std::string category;  // a plug-in's VST3 sub-categories ("Instrument|Synth", "Fx|Reverb"; "" if unknown)
    std::string preset;    // the preset it has loaded ("PL - Electric Flow"; "" if unknown)
    std::string sample;    // a sampler's sample file
    bool instrument = false;
    bool enabled = true;
    std::vector<ParamFact> params;               // those known (a plug-in's: only while it is loaded)
    std::vector<std::vector<DeviceFact>> chains;  // a rack's: each chain's devices
};

// A note the track plays, where it plays it.
struct NoteFact {
    int pitch = 60;
    double start = 0.0;
    double end = 0.0;
    int velocity = 100;
};

// An audio clip the track plays: its file, and how long it plays it.
struct ClipFact {
    std::string file;  // a path or a file name
    double beats = 0.0;
};

// A send to a return track.
struct SendFact {
    std::string returnId;
    double levelDb = -70.0;
};

inline const std::string kAudio = "audio";
inline const std::string kMidi = "midi";
inline const std::string kGroup = "group";
inline const std::string kReturn = "return";
inline const std::string kMaster = "master";

struct TrackFacts {
    std::string id;
    std::string kind;  // kAudio, kMidi, kGroup, kReturn or kMaster
    std::string name;  // as shown
    // Whether its name is the user's words, not one SUBstation gave it (after
    // its contents, "3 MIDI"...): such a name says what the track is.
    bool namedByUser = false;
    std::string parent;  // the group it is in ("" if none)
    double volumeDb = 0.0;
    double pan = 0.0;  // -1 (left) .. 1 (right)
    bool mute = false;
    bool frozen = false;
    std::vector<DeviceFact> devices;
    std::vector<ClipFact> audio;  // the audio clips it plays (not deactivated ones)
    std::vector<NoteFact> notes;  // the notes it plays (not deactivated ones)
    std::vector<SendFact> sends;
};

// What a track is, in words.
struct TrackLabel {
    // A few words, as a person would name it: "Washed Out Serum Pluck",
    // "Orchestral Trumpet", "Echoing Bell Stab", "Drum Group". Different from
    // every other track's in the project where anything tells them apart.
    std::string label;
    // What part it plays in the song, one of a fixed set ("drums", "bass",
    // "keys", "synth", "strings", "brass", "woodwinds", "vocals", "guitar",
    // "mallets", "fx", "ambience", "mix" for a group, a return or the master; ""
    // if nothing tells), and what it is, lower case ("pluck", "kick", "violins").
    std::string family;
    std::string role;
    // How it sounds, strongest first: "washed out", "wide", "quiet"... (every
    // trait found, not only those in the label).
    std::vector<std::string> traits;
    // What the label was made from, a line each: its instrument and preset, its
    // notes, its files, its effects and their amounts, its mixer.
    std::vector<std::string> details;
};

}  // namespace sub::intelligence::labels
