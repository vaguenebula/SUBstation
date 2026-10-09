#include "labels/TrackLabels.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string_view>

#include "labels/Effects.h"
#include "labels/NoteProfile.h"
#include "labels/Words.h"

namespace sub::intelligence::labels {

namespace {

// How much each source's nouns weigh, and its adjectives (0..1, as traits do).
constexpr double kUserNouns = 3.0, kUserAdjectives = 0.7;
constexpr double kRackNouns = 2.5, kEffectRackNouns = 2.0, kRackAdjectives = 0.6;
constexpr double kPresetNouns = 2.5, kPresetCategory = 0.5, kPresetAdjectives = 0.55;
constexpr double kInstrumentNouns = 2.0, kInstrumentAdjectives = 0.45, kKnownAdjective = 0.35;
constexpr double kCategoryNouns = 1.5;
constexpr double kFileNouns = 2.5, kFileAdjectives = 0.5;
constexpr double kGenres = 0.3;
constexpr double kNotePatterns = 2.0, kNoteSounds = 2.0, kSynthSounds = 1.4;
// Adjectives after a preset's category ("PL - Electric Flow") are its flavour more than its sound.
constexpr double kPresetFlavour = 0.4;
// Nouns below this say too little to name a track by; patterns below kMinPattern.
constexpr double kMinNoun = 1.0, kMinPattern = 0.8;
// Evidence this strong outweighs what the notes suggest.
constexpr double kStrong = 2.0;
// A label's traits: the first needs this much weight, a second this much.
constexpr double kFirstTrait = 0.4, kSecondTrait = 0.55;
// Traits shared by more tracks weigh less, down to this share when all have them.
constexpr double kSharedTraitFloor = 0.35;
// What tells tracks alike apart: a trait at least this strong (a preset's flavour words aren't).
constexpr double kTellingTrait = 0.35;

std::string trimmed(std::string_view text) {
    size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    return std::string(text.substr(a, b - a));
}

std::string quoted(std::string_view text) { return "“" + std::string(text) + "”"; }

std::string decibels(double db) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f dB", db);
    return text;
}

// A file's name without its folders and extension.
std::string fileStem(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0 && name.size() - dot <= 6) name = name.substr(0, dot);
    return std::string(name);
}

// A take recorded into SUBstation: "<track> 2026-10-08 141500" (and " 2" or "_" after it).
bool isTake(std::string_view stem) {
    while (!stem.empty() && stem.back() == '_') stem.remove_suffix(1);
    // " dddd-dd-dd dddddd" at the end ('0': a digit).
    const auto endsWithTime = [](std::string_view s) {
        constexpr std::string_view kShape = " 0000-00-00 000000";
        if (s.size() < kShape.size()) return false;
        s = s.substr(s.size() - kShape.size());
        for (size_t i = 0; i < kShape.size(); ++i) {
            if (kShape[i] == '0' ? !std::isdigit(static_cast<unsigned char>(s[i])) : s[i] != kShape[i]) return false;
        }
        return true;
    };
    if (endsWithTime(stem)) return true;
    // Or a copy's number after it: " 2".
    const size_t space = stem.find_last_of(' ');
    if (space == std::string_view::npos || space + 1 == stem.size()) return false;
    const std::string_view number = stem.substr(space + 1);
    return std::all_of(number.begin(), number.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) &&
           endsWithTime(stem.substr(0, space));
}

// Whether a preset's name says anything ("- Init -", "Default Setting" don't).
bool informativePreset(const std::string& preset) {
    static const std::set<std::string, std::less<>> kEmpty = {
        "",        "init",         "initpatch",      "initpreset",          "initialize", "default",
        "default setting", "defaultsetting", "defaultpreset", "factorydefault", "factorydefaultpreset",
        "empty",   "untitled",     "newpreset",      "none",                "basic"};
    return !kEmpty.contains(squeezed(preset));
}

// "Serum 2" -> "Serum": a plug-in's name without its version.
std::string shortName(std::string name) {
    name = trimmed(name);
    while (!name.empty()) {
        const size_t space = name.find_last_of(' ');
        if (space == std::string::npos) break;
        const std::string last = name.substr(space + 1);
        const bool version = !last.empty() && std::all_of(last.begin(), last.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) || c == '.';
        });
        if (!version) break;
        name = trimmed(name.substr(0, space));
    }
    bool capitals = name.size() > 3;
    for (const char c : name) capitals = capitals && !std::islower(static_cast<unsigned char>(c));
    return capitals ? titleCase(lowered(name)) : name;  // "SERUM" -> "Serum"
}

// Instruments whose names alone don't say what they are.
struct KnownInstrument {
    std::string_view needle;  // in the plug-in's name, lower case
    bool synth;               // a synthesizer: its name goes in the label before its sound
    std::string_view noun;    // what it plays, whatever the preset ("" if that depends on the preset)
    std::string_view adjective;
    std::string_view source;  // how a label calls it ("" for its own short name)
};

const KnownInstrument kInstruments[] = {
    {"serum", true, "", "", ""},
    {"vital", true, "", "", ""},
    {"massive", true, "", "", ""},
    {"sylenth", true, "", "", "Sylenth"},
    {"spire", true, "", "", ""},
    {"diva", true, "", "", ""},
    {"pigments", true, "", "", ""},
    {"phase plant", true, "", "", ""},
    {"hive", true, "", "", ""},
    {"zebra", true, "", "", ""},
    {"repro", true, "", "", ""},
    {"avenger", true, "", "", ""},
    {"surge", true, "", "", ""},
    {"dexed", true, "", "", "DX7"},
    {"omnisphere", true, "", "", ""},
    {"synplant", true, "", "", ""},
    {"reaktor", true, "", "", ""},
    {"msoundfactory", true, "", "", "SoundFactory"},
    {"twin", true, "", "", ""},
    {"current", true, "", "", ""},
    {"vereor", true, "", "", ""},
    {"synthgpt", true, "", "", ""},
    {"kontakt", false, "", "", ""},
    {"komplete kontrol", false, "", "", "Kontakt"},
    {"bbc symphony orchestra", false, "", "Orchestral", "BBC SO"},
    {"originals", false, "", "", ""},
    {"labs", false, "", "", "LABS"},
    {"splice instrument", false, "", "", "Splice"},
    {"splice sounds", false, "", "", "Splice"},
    {"keyscape", false, "Piano", "", ""},
    {"pianoteq", false, "Piano", "", ""},
    {"superior drummer", false, "Drums", "", ""},
    {"ezdrummer", false, "Drums", "", ""},
    {"addictive drums", false, "Drums", "", ""},
    {"battery", false, "Drums", "", ""},
    {"trilian", false, "Bass", "", ""},
    {"emvoice", false, "Vocal", "", ""},
    {"synthesizer v", false, "Vocal", "", ""},
    {"ample metal", false, "Electric Guitar", "", ""},
    {"ample guitar", false, "Guitar", "", ""},
    {"ample bass", false, "Bass Guitar", "", ""},
};

const KnownInstrument* knownInstrument(const std::string& name) {
    const std::string lower = lowered(name);
    for (const KnownInstrument& known : kInstruments) {
        if (lower.find(known.needle) != std::string::npos) return &known;
    }
    return nullptr;
}

// Which traits say the same thing (a label takes one of each).
std::string traitGroup(std::string_view word) {
    static const std::map<std::string_view, std::string_view> kGroups = {
        {"Washed Out", "space"}, {"Spacious", "space"},   {"Roomy", "space"},     {"Distant", "space"},
        {"Echoing", "echo"},     {"Distorted", "drive"},  {"Saturated", "drive"}, {"Gritty", "drive"},
        {"Dirty", "drive"},      {"Overdriven", "drive"}, {"Crunchy", "drive"},   {"Squashed", "dynamics"},
        {"Compressed", "dynamics"}, {"Limited", "dynamics"}, {"Clipped", "dynamics"}, {"Pumping", "pump"},
        {"Ducked", "pump"},      {"Wide", "width"},       {"Mono", "width"},      {"Dark", "tone"},
        {"Bright", "tone"},      {"Muffled", "tone"},     {"Telephone", "tone"},  {"Mellow", "tone"},
        {"Smooth", "tone"},      {"Warm", "tone"},        {"Quiet", "level"},     {"Loud", "level"},
        {"Lo-Fi", "lofi"},       {"Thin", "body"},        {"Fat", "body"},        {"Big", "body"},
        {"Huge", "body"},        {"Massive", "body"},     {"Shimmering", "shimmer"},
    };
    const auto it = kGroups.find(word);
    return it != kGroups.end() ? std::string(it->second) : "word:" + std::string(word);
}

// Adjectives of what a sound is made of, which go next to its noun ("Soft
// Acoustic Kick", not "Acoustic Soft Kick").
bool nearNoun(std::string_view word) {
    static const std::set<std::string_view> kMaterial = {
        "Acoustic", "Electric", "Orchestral", "Cinematic", "Analog", "Vintage", "Retro", "Chiptune", "Saw",
        "Square", "Sine", "Triangle", "Detuned", "Pizzicato", "Staccato", "Granular", "Organic"};
    return kMaterial.contains(word);
}

struct Noun {
    Word word;
    double score = 0.0;
    int order = 0;  // when first seen: earlier wins a tie
    bool fromNotes = false;
};

// Everything known of a track, then what it is called.
struct Description {
    std::vector<Noun> nouns;
    std::vector<Trait> traits;
    std::string source;  // its instrument, as a label calls it ("Serum")
    bool synth = false;
    bool midi = false;
    std::vector<std::string> distinctive;  // what else tells it apart: a preset's own name, a file's words
    std::string ownWords;  // its main file's own words ("Missme"), to name it by if nothing else does
    bool empty = false;    // no clips, no notes, no instrument
    std::vector<std::string> details;
    NoteProfile notes;
    int seen = 0;
    // What it is called.
    std::string head;  // the noun phrase: "Serum Pluck Arp"
    Family family = Family::None;
    std::string role;
    std::string label;
    std::vector<std::string> labelTraits;  // those the label shows
    std::map<std::string, double> ownWeights;  // traits' weights before shared ones weighed less
};

void addNoun(Description& d, const Word& word, double score, bool fromNotes = false) {
    for (Noun& noun : d.nouns) {
        if (noun.word.display == word.display && noun.word.kind == word.kind) {
            noun.score = std::max(noun.score, score) + 0.25 * std::min(noun.score, score);
            noun.fromNotes = noun.fromNotes && fromNotes;
            return;
        }
    }
    d.nouns.push_back({word, score, d.seen++, fromNotes});
}

void addTrait(Description& d, const std::string& word, double weight, const std::string& group = {}) {
    if (weight <= 0.0) return;
    for (Trait& trait : d.traits) {
        if (trait.word == word) {
            trait.weight = std::min(1.0, std::max(trait.weight, weight) + 0.15 * std::min(trait.weight, weight));
            return;
        }
    }
    d.traits.push_back({word, weight, group.empty() ? traitGroup(word) : group});
}

void addReading(Description& d, const Reading& reading, double nouns, double adjectives, double materials = -1.0) {
    for (const Word& word : reading.words) {
        switch (word.kind) {
        case WordKind::Adjective:
            addTrait(d, word.display, materials >= 0.0 && nearNoun(word.display) ? materials : adjectives);
            break;
        case WordKind::Genre: addTrait(d, word.display, std::min(adjectives, kGenres), "genre"); break;
        default: addNoun(d, word, nouns); break;
        }
    }
}

// A word the vocabulary has for a display name ("Chords", "Sub Bass").
Word wordFor(std::string_view display) {
    const Reading reading = readWords(display, Source::Name);
    return reading.words.empty() ? Word{std::string(display), WordKind::Sound, Family::None, 0} : reading.words.front();
}

// The unknown words of a name, title case: what only it is called ("Missme", "Electric Flow").
std::string ownWords(const Reading& reading, size_t at_most = 2) {
    std::string text;
    for (size_t i = 0; i < reading.unknown.size() && i < at_most; ++i) {
        if (!text.empty()) text += ' ';
        text += titleCase(reading.unknown[i]);
    }
    return text;
}

bool contains(const DeviceFact& rack, const DeviceFact* target) {
    for (const auto& chain : rack.chains) {
        for (const DeviceFact& device : chain) {
            if (&device == target || contains(device, target)) return true;
        }
    }
    return false;
}


// --- Reading a track ------------------------------------------------------------------------------

void readInstrument(Description& d, const DeviceFact& device) {
    const KnownInstrument* known = device.kind == "plugin" ? knownInstrument(device.name) : nullptr;
    const std::string name = device.name.empty() ? titleCase(device.kind) : device.name;
    if (device.kind == "synth") {
        d.synth = true;
        d.source = "Synth";
    } else if (device.kind == "sampler") {
        d.source = "Sampler";
    } else {
        d.synth = known ? known->synth
                        : lowered(device.category).find("synth") != std::string::npos &&
                              lowered(device.vendor).find("spitfire") == std::string::npos;
        d.source = known && !known->source.empty() ? std::string(known->source) : shortName(device.name);
    }
    if (known && !known->noun.empty()) addReading(d, readWords(known->noun, Source::Name), kInstrumentNouns, 0.0);
    if (known && !known->adjective.empty()) addTrait(d, std::string(known->adjective), kKnownAdjective);
    addReading(d, readWords(device.name, Source::Name), kInstrumentNouns, kInstrumentAdjectives);
    const std::string category = lowered(device.category);
    if (category.find("drum") != std::string::npos) addNoun(d, wordFor("Drums"), kCategoryNouns);
    if (category.find("piano") != std::string::npos) addNoun(d, wordFor("Piano"), kCategoryNouns);

    std::string line = "Instrument: " + name;
    if (informativePreset(device.preset)) {
        // A category first ("PL - Electric Flow") counts a little more.
        const size_t dash = device.preset.find(" - ");
        if (dash != std::string::npos) {
            const Reading first = readWords(device.preset.substr(0, dash), Source::Preset);
            const Reading rest = readWords(device.preset.substr(dash + 3), Source::Preset);
            const bool category = !first.words.empty() && first.unknown.empty();
            addReading(d, first, kPresetNouns + kPresetCategory, kPresetAdjectives);
            if (category) {
                addReading(d, rest, kPresetNouns, kPresetFlavour, kPresetFlavour * 0.75);
            } else {
                addReading(d, rest, kPresetNouns, kPresetAdjectives);
            }
            d.distinctive.push_back(category ? trimmed(device.preset.substr(dash + 3)) : trimmed(device.preset));
        } else {
            addReading(d, readWords(device.preset, Source::Preset), kPresetNouns, kPresetAdjectives);
            d.distinctive.push_back(trimmed(device.preset));
        }
        line += ", preset " + quoted(trimmed(device.preset));
    }
    if (device.kind == "sampler" && !device.sample.empty()) {
        const std::string stem = fileStem(device.sample);
        const Reading reading = readWords(stem, Source::File);
        addReading(d, reading, kFileNouns, kFileAdjectives);
        if (const std::string own = ownWords(reading); !own.empty()) d.distinctive.push_back(own);
        line += ", sample " + quoted(stem);
    }
    if (device.kind == "synth") {
        // Its envelope: a quick decay to silence plucks, a slow attack held swells.
        const double attack = builtinParam(device, "attack").value_or(3.0);
        const double decay = builtinParam(device, "decay").value_or(300.0);
        const double sustain = builtinParam(device, "sustain").value_or(70.0);
        if (attack >= 250.0 && sustain >= 40.0) {
            addNoun(d, wordFor("Pad"), kSynthSounds);
        } else if (decay <= 450.0 && sustain <= 25.0) {
            addNoun(d, wordFor("Pluck"), kSynthSounds);
        }
        static const char* const kWaves[] = {"Sine", "Triangle", "Saw", "Square"};
        const auto wave = static_cast<int>(std::lround(builtinParam(device, "wave").value_or(2.0)));
        if (wave >= 0 && wave < 4) addTrait(d, kWaves[wave], 0.2);
    }
    if (!device.enabled) line += " (off)";
    d.details.push_back(line);
}

void readAudio(Description& d, const std::vector<ClipFact>& clips) {
    struct File {
        std::string stem;
        double beats = 0.0;
        int count = 0;
    };
    std::vector<File> files;
    double total = 0.0;
    for (const ClipFact& clip : clips) {
        const std::string stem = fileStem(clip.file);
        auto it = std::find_if(files.begin(), files.end(), [&](const File& f) { return f.stem == stem; });
        if (it == files.end()) it = files.insert(files.end(), File{stem});
        it->beats += std::max(clip.beats, 1e-3);
        ++it->count;
        total += std::max(clip.beats, 1e-3);
    }
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.beats > b.beats; });
    for (size_t i = 0; i < files.size() && i < 3; ++i) {
        const File& file = files[i];
        const double share = file.beats / total;
        if (isTake(file.stem)) addNoun(d, Word{"Recording", WordKind::Sound, Family::None, 0}, kMinNoun * share);
        const Reading reading = readWords(file.stem, Source::File);
        addReading(d, reading, kFileNouns * share, kFileAdjectives * share);
        if (i == 0) {
            if (const std::string own = ownWords(reading); !own.empty()) {
                d.distinctive.push_back(own);
                d.ownWords = own;
            }
        }
        if (i < 2) {
            d.details.push_back("Audio: " + quoted(file.stem) +
                                (file.count > 1 ? " (" + std::to_string(file.count) + " clips)" : std::string()));
        }
    }
}

void readNotesOf(Description& d, const std::vector<NoteFact>& notes) {
    d.notes = profile(notes);
    if (d.notes.count == 0) return;
    const NoteReading reading = readNotes(d.notes);
    if (!reading.pattern.empty()) addNoun(d, wordFor(reading.pattern), kNotePatterns * reading.confidence, true);
    if (!reading.sound.empty()) addNoun(d, wordFor(reading.sound), kNoteSounds * reading.confidence, true);
    d.details.push_back("Notes: " + describe(d.notes));
}

void readEffect(Description& d, const DeviceFact& device) {
    const DeviceReading reading = readDevice(device);
    for (const Trait& trait : reading.traits) addTrait(d, trait.word, trait.weight, trait.group);
    if (reading.summary.empty()) return;
    std::string line = "Effect: " + reading.summary;
    if (informativePreset(device.preset)) line += " (preset " + quoted(trimmed(device.preset)) + ")";
    d.details.push_back(line);
}

void readMixer(Description& d, const TrackFacts& track) {
    if (track.volumeDb <= -30.0) {
        addTrait(d, "Quiet", 0.5);
    } else if (track.volumeDb <= -20.0) {
        addTrait(d, "Quiet", 0.35);
    }
    const double pan = std::abs(track.pan);
    const std::string side = track.pan < 0.0 ? "Left" : "Right";
    if (pan >= 0.6) {
        addTrait(d, "Hard-" + side, 0.45, "pan");
    } else if (pan >= 0.25) {
        addTrait(d, side + "-Panned", 0.3, "pan");
    }
    std::string line = "Mixer: " + decibels(track.volumeDb);
    if (pan >= 0.005) line += ", " + std::to_string(static_cast<int>(std::lround(pan * 100.0))) + "% " + lowered(side);
    if (track.mute) line += ", muted";
    if (track.frozen) line += ", frozen";
    d.details.push_back(line);
}

// What a return does (its first effect that colours the sound), for the sends to it.
struct ReturnInfo {
    std::string name;
    Effect effect = Effect::None;
};

void readSends(Description& d, const TrackFacts& track, const std::map<std::string, ReturnInfo>& returns) {
    for (const SendFact& send : track.sends) {
        const auto it = returns.find(send.returnId);
        if (it == returns.end() || send.levelDb <= -60.0) continue;
        const Effect effect = it->second.effect;
        if (effect == Effect::Reverb) {
            addTrait(d, send.levelDb >= -3.0 ? "Washed Out" : "Spacious", send.levelDb >= -3.0 ? 0.6 : send.levelDb >= -12.0 ? 0.45 : 0.25);
        } else if (effect == Effect::Delay) {
            addTrait(d, "Echoing", send.levelDb >= -12.0 ? 0.4 : 0.25);
        }
        const std::string what = effect == Effect::None ? std::string() : " (" + effectName(effect) + ")";
        d.details.push_back("Send: " + it->second.name + what + " at " + decibels(send.levelDb));
    }
}

// Every device of a chain, depth first, with racks and their own devices apart.
void walk(const std::vector<DeviceFact>& devices, std::vector<const DeviceFact*>& out) {
    for (const DeviceFact& device : devices) {
        out.push_back(&device);
        for (const auto& chain : device.chains) walk(chain, out);
    }
}

Description describeTrack(const TrackFacts& track, const std::map<std::string, ReturnInfo>& returns) {
    Description d;
    d.midi = track.kind == kMidi;
    if (track.namedByUser) {
        const Reading reading = readWords(track.name, Source::Name);
        addReading(d, reading, kUserNouns, kUserAdjectives);
        d.distinctive.push_back(trimmed(track.name));
    }
    std::vector<const DeviceFact*> devices;
    walk(track.devices, devices);
    const DeviceFact* instrument = nullptr;
    for (const DeviceFact* device : devices) {
        if (classify(*device) != Effect::Instrument) continue;
        if (!instrument || (!instrument->enabled && device->enabled)) instrument = device;
    }
    // What it is first (its instrument, its racks, its files, its notes), then what shapes it.
    if (instrument) readInstrument(d, *instrument);
    for (const DeviceFact* device : devices) {
        if (device->kind == "rack" && !trimmed(device->name).empty()) {
            const bool holds = instrument && contains(*device, instrument);
            addReading(d, readWords(device->name, Source::Name), holds ? kRackNouns : kEffectRackNouns, kRackAdjectives);
            d.distinctive.push_back(trimmed(device->name));
            d.details.push_back("Rack: " + quoted(trimmed(device->name)));
        } else if (device != instrument && classify(*device) == Effect::Instrument) {
            d.details.push_back("Layered with: " + device->name);
        }
    }
    readAudio(d, track.audio);
    readNotesOf(d, track.notes);
    d.empty = !instrument && track.audio.empty() && track.notes.empty();
    for (const DeviceFact* device : devices) {
        if (device->kind != "rack" && classify(*device) != Effect::Instrument) readEffect(d, *device);
    }
    readSends(d, track, returns);
    readMixer(d, track);
    return d;
}

// --- Naming it ------------------------------------------------------------------------------------

const Noun* best(const Description& d, WordKind kind, double minimum, bool notes = true) {
    const Noun* found = nullptr;
    for (const Noun& noun : d.nouns) {
        if (noun.word.kind != kind || noun.score < minimum || (!notes && noun.fromNotes)) continue;
        if (!found || noun.score > found->score + 1e-9 ||
            (std::abs(noun.score - found->score) <= 1e-9 &&
             (noun.word.rank > found->word.rank || (noun.word.rank == found->word.rank && noun.order < found->order)))) {
            found = &noun;
        }
    }
    return found;
}

bool containsWord(const std::string& phrase, const std::string& word) {
    const std::vector<std::string> words = splitWords(phrase);
    const std::string lower = lowered(word);
    return std::find(words.begin(), words.end(), lower) != words.end();
}

// Appends `phrase` to `head` unless every word of it is there already ("Bass" after "Double Bass").
void append(std::string& head, const std::string& phrase) {
    if (phrase.empty()) return;
    bool fresh = false;
    for (const std::string& word : splitWords(phrase)) fresh = fresh || !containsWord(head, word);
    if (!fresh) return;
    if (!head.empty()) head += ' ';
    head += phrase;
}

// The nouns of a track with clips: [its instrument's name] [what plays] [the sound] [the pattern].
void nameClipTrack(Description& d) {
    const Noun* instrument = best(d, WordKind::Instrument, kMinNoun);
    const bool strongInstrument = instrument && instrument->score >= kStrong;
    const Noun* sound = best(d, WordKind::Sound, kMinNoun, !strongInstrument);
    const Noun* pattern = best(d, WordKind::Pattern, kMinPattern);
    const bool drums = instrument && instrument->word.family == Family::Drums;
    if (drums && sound && sound->fromNotes) sound = nullptr;
    if (drums && pattern && pattern->fromNotes) pattern = nullptr;
    std::string soundText = sound ? sound->word.display : std::string();
    std::string patternText = pattern ? pattern->word.display : std::string();
    // "Synth" says less than any other sound; before a bass it says what kind ("Synth Bass").
    const bool synthWord = std::any_of(d.nouns.begin(), d.nouns.end(), [](const Noun& noun) {
        return noun.word.kind == WordKind::Sound && noun.word.display == "Synth" && noun.score >= kMinNoun && !noun.fromNotes;
    });
    if (soundText == "Synth") {
        for (const Noun& other : d.nouns) {
            if (other.word.kind != WordKind::Sound || &other == sound || other.score < kMinNoun) continue;
            soundText = other.word.display;
            sound = &other;
            break;
        }
    }
    if (synthWord && soundText == "Bass") soundText = "Synth Bass";
    if (patternText == "One-Shot") patternText.clear();
    if ((soundText == "Pad" || soundText == "Stab") && patternText == "Chords") patternText.clear();
    if (soundText == "Lead" && (patternText == "Line" || patternText == "Melody")) patternText.clear();
    if (sound && sound->fromNotes && soundText == "Lead" && patternText == "Arp") soundText.clear();
    // That the notes come one at a time says nothing more of a pluck, a lead, a bass ("Piano Line" it does).
    if (!soundText.empty() && pattern && pattern->fromNotes && patternText == "Line") patternText.clear();
    std::string instrumentText = instrument ? instrument->word.display : std::string();
    if (instrumentText == "Drums" && !patternText.empty()) instrumentText = "Drum";  // "Drum Loop", "Drum Fill"
    // What the notes play, said of an orchestra's instruments or a voice, says little ("Violins Line").
    static const std::set<Family> kOrchestral = {Family::Strings, Family::Brass, Family::Woodwinds, Family::Vocals};
    if (instrument && kOrchestral.contains(instrument->word.family) && pattern && pattern->fromNotes) patternText.clear();
    // Nature sounds are an ambience ("Wind Ambience").
    static const std::set<std::string_view> kNature = {"Wind", "Rain", "Storm", "Thunder", "Forest", "Ocean", "Water", "Birds"};
    if (sound && kNature.contains(soundText) && patternText.empty()) soundText += " Ambience";

    std::string head;
    // The instrument's name: a synth's always ("Serum Pluck"); another's when nothing else
    // says what it plays ("Kontakt Chords"), but not before what its sample is ("808").
    const bool soundFromWords = sound && !sound->fromNotes && !soundText.empty();
    if (!d.source.empty() && (d.synth || (!instrument && !soundFromWords))) append(head, d.source);
    append(head, instrumentText);
    append(head, soundText);
    append(head, patternText);
    if (head.empty() && !d.ownWords.empty()) head = d.ownWords;  // a file nothing else names: its own words
    if (head.empty()) head = d.empty ? (d.midi ? "Empty MIDI" : "Empty Audio") : (d.midi ? "MIDI" : "Audio");
    d.head = head;

    if (d.synth && sound && sound->word.family != Family::None) {
        d.family = sound->word.family;  // a synth's flavour ("Vocal Lead") doesn't make it a voice
    } else if (instrument) {
        d.family = instrument->word.family;
    } else if (sound && sound->word.family != Family::None) {
        d.family = sound->word.family;
    } else if (pattern && pattern->word.family != Family::None) {
        d.family = pattern->word.family;
    } else if (d.synth) {
        d.family = Family::Synth;
    }
    const std::string role = d.synth && !soundText.empty() ? soundText
                             : !instrumentText.empty()     ? instrument->word.display
                             : !soundText.empty()          ? soundText
                                                           : patternText;
    d.role = lowered(role);
}

// The label: its traits (at most two, one of each group, the stronger first;
// what a sound is made of next to its noun), then its nouns.
void compose(Description& d, int maxTraits = 2) {
    std::vector<const Trait*> sorted;
    for (const Trait& trait : d.traits) sorted.push_back(&trait);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Trait* a, const Trait* b) { return a->weight > b->weight; });
    std::vector<std::string> chosen;
    std::set<std::string> groups;
    for (const Trait* trait : sorted) {
        if (static_cast<int>(chosen.size()) >= maxTraits) break;
        if (trait->weight < (chosen.empty() ? kFirstTrait : kSecondTrait)) break;
        if (groups.contains(trait->group) || trait->group == "genre" || containsWord(d.head, trait->word)) continue;
        groups.insert(trait->group);
        chosen.push_back(trait->word);
    }
    std::stable_partition(chosen.begin(), chosen.end(), [](const std::string& word) { return !nearNoun(word); });
    d.labelTraits = chosen;
    d.label.clear();
    for (const std::string& word : chosen) d.label += word + ' ';
    d.label += d.head;
}

// A group: what the tracks in it are ("Drum Group", "Synth & Bass Group"), or
// what the user called it.
Description describeGroup(const TrackFacts& track, const std::vector<const Description*>& children,
                          const std::vector<std::string>& childLabels, int projectTracks, int heldTracks) {
    Description d;
    std::vector<const DeviceFact*> devices;
    walk(track.devices, devices);
    for (const DeviceFact* device : devices) {
        if (device->kind != "rack") readEffect(d, *device);
    }
    std::map<Family, int> families;
    for (const Description* child : children) {
        if (child->family != Family::None && child->family != Family::Mix) ++families[child->family];
    }
    std::vector<std::pair<int, Family>> ranked;
    for (const auto& [family, count] : families) ranked.emplace_back(count, family);
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const int counted = static_cast<int>(children.size());

    std::string head;
    if (track.namedByUser) {
        Description named;
        addReading(named, readWords(track.name, Source::Name), kUserNouns, 0.0);
        const Noun* noun = best(named, WordKind::Instrument, kMinNoun);
        if (!noun) noun = best(named, WordKind::Sound, kMinNoun);
        if (noun) {
            // A family's own word ("drums", "synths") names the group as a family does.
            const bool generic = noun->word.rank <= 1 && noun->word.family != Family::None;
            head = (generic ? familyTitle(noun->word.family) : noun->word.display) + " Group";
            d.family = noun->word.family;
        }
    }
    if (head.empty()) {
        if (children.empty()) {
            head = "Empty Group";
        } else if (!ranked.empty() && ranked[0].first * 10 >= counted * 6) {
            head = familyTitle(ranked[0].second) + " Group";
            d.family = ranked[0].second;
        } else if (ranked.size() >= 2 && (ranked[0].first + ranked[1].first) * 10 >= counted * 8) {
            head = familyTitle(ranked[0].second) + " & " + familyTitle(ranked[1].second) + " Group";
        } else if (projectTracks > 0 && heldTracks * 10 >= projectTracks * 6) {
            head = "Mix Bus";
        } else {
            head = "Mixed Group";
        }
    }
    if (d.family == Family::None) d.family = Family::Mix;
    d.head = head;
    d.role = "group";
    // What it holds tells it from groups of the same kind: "Drum Group (Kick, Snare)".
    std::vector<std::string> held;
    for (const Description* child : children) {
        const std::string role = titleCase(child->role);
        if (!child->role.empty() && child->role != "group" && std::find(held.begin(), held.end(), role) == held.end()) {
            held.push_back(role);
        }
    }
    if (!held.empty()) d.distinctive.push_back(held.size() == 1 ? held[0] : held[0] + ", " + held[1]);
    if (!childLabels.empty()) {
        std::string holds = "Holds: ";
        for (size_t i = 0; i < childLabels.size(); ++i) {
            if (i == 6) {
                holds += ", and " + std::to_string(childLabels.size() - 6) + " more";
                break;
            }
            holds += (i ? ", " : "") + childLabels[i];
        }
        d.details.insert(d.details.begin(), holds);
    }
    readMixer(d, track);
    return d;
}

// A return: what its effect is ("Reverb Return"); the traits of how wet it is
// say nothing on a return, which is all wet.
Description describeReturn(const TrackFacts& track, Effect effect) {
    Description d;
    std::vector<const DeviceFact*> devices;
    walk(track.devices, devices);
    for (const DeviceFact* device : devices) {
        if (device->kind != "rack") readEffect(d, *device);
    }
    std::erase_if(d.traits, [](const Trait& t) { return t.group == "space" || t.group == "echo"; });
    const std::string what = effect == Effect::None ? std::string() : titleCase(effectName(effect));
    d.head = devices.empty() ? "Empty Return" : (what.empty() ? "Effect" : what) + " Return";
    if (effect == Effect::Eq) d.head = "EQ Return";
    d.family = Family::Mix;
    d.role = "return";
    readMixer(d, track);
    return d;
}

Description describeMaster(const TrackFacts& track) {
    Description d;
    std::vector<const DeviceFact*> devices;
    walk(track.devices, devices);
    for (const DeviceFact* device : devices) {
        if (device->kind != "rack") readEffect(d, *device);
    }
    std::erase_if(d.traits, [](const Trait& t) { return t.word == "Limited" || t.word == "Compressed" || t.word == "Mastered"; });
    d.head = "Master";
    d.family = Family::Mix;
    d.role = "master";
    if (devices.empty()) d.details.push_back("No effects");
    readMixer(d, track);
    return d;
}

// The effect a return colours its sends with: its first reverb or delay, else its first effect.
Effect returnEffect(const TrackFacts& track) {
    std::vector<const DeviceFact*> devices;
    walk(track.devices, devices);
    Effect first = Effect::None;
    for (const DeviceFact* device : devices) {
        if (!device->enabled || device->kind == "rack") continue;
        const Effect effect = classify(*device);
        if (effect == Effect::Reverb || effect == Effect::Delay) return effect;
        if (first == Effect::None && effect != Effect::Utility && effect != Effect::Analyzer && effect != Effect::Eq) first = effect;
    }
    if (first == Effect::None) {
        for (const DeviceFact* device : devices) {
            if (device->kind != "rack" && classify(*device) == Effect::Eq) return Effect::Eq;
        }
    }
    return first;
}

// --- The project ----------------------------------------------------------------------------------

// Traits most tracks share say little of any one of them.
void weighShared(std::vector<Description*>& tracks) {
    if (tracks.size() < 3) return;
    std::map<std::string, int> counts;
    for (const Description* d : tracks) {
        for (const Trait& trait : d->traits) {
            if (trait.weight >= 0.3) ++counts[trait.word];
        }
    }
    const double n = static_cast<double>(tracks.size());
    for (Description* d : tracks) {
        for (Trait& trait : d->traits) {
            const auto it = counts.find(trait.word);
            if (it == counts.end() || it->second <= 1) continue;
            d->ownWeights[trait.word] = trait.weight;
            trait.weight *= 1.0 - (1.0 - kSharedTraitFloor) * (it->second - 1) / (n - 1.0);
        }
    }
}

// How much a trait says of the track itself, however many others share it: what
// tells tracks alike apart (of six arps, the quiet ones are the quiet ones).
double ownWeight(const Description& d, const Trait& trait) {
    const auto it = d.ownWeights.find(trait.word);
    return it == d.ownWeights.end() ? trait.weight : it->second;
}

// Tracks with the same label told apart: by a trait the others lack, by their
// register, by their presets' or files' own names, by the user's names; numbered
// if nothing tells them apart.
void distinguish(std::vector<Description*>& tracks) {
    const auto collisions = [&] {
        std::map<std::string, std::vector<Description*>> byLabel;
        for (Description* d : tracks) byLabel[lowered(d->label)].push_back(d);
        std::vector<std::vector<Description*>> same;
        for (auto& [label, group] : byLabel) {
            if (group.size() > 1) same.push_back(group);
        }
        return same;
    };
    // 1. A trait not in its label that the others lack.
    for (auto& same : collisions()) {
        for (Description* d : same) {
            const Trait* pick = nullptr;
            for (const Trait& trait : d->traits) {
                const double weight = ownWeight(*d, trait);
                if (weight < kTellingTrait || trait.group == "genre" || containsWord(d->label, trait.word)) continue;
                const bool shared = std::all_of(same.begin(), same.end(), [&](const Description* other) {
                    return other == d || std::any_of(other->traits.begin(), other->traits.end(), [&](const Trait& t) {
                               return t.word == trait.word && ownWeight(*other, t) >= kTellingTrait;
                           });
                });
                if (shared) continue;
                const bool groupTaken = std::any_of(d->labelTraits.begin(), d->labelTraits.end(), [&](const std::string& w) {
                    return traitGroup(w) == trait.group;
                });
                if (groupTaken) continue;
                if (!pick || weight > ownWeight(*d, *pick)) pick = &trait;
            }
            if (pick) {
                d->label = pick->word + ' ' + d->label;
                d->labelTraits.push_back(pick->word);
            }
        }
    }
    // 2. Their register, where their notes lie apart.
    for (auto& same : collisions()) {
        std::vector<Description*> withNotes;
        for (Description* d : same) {
            if (d->notes.count > 0) withNotes.push_back(d);
        }
        if (withNotes.size() < 2) continue;
        const auto [low, high] = std::minmax_element(withNotes.begin(), withNotes.end(), [](const Description* a, const Description* b) {
            return a->notes.medianPitch < b->notes.medianPitch;
        });
        if ((*high)->notes.medianPitch - (*low)->notes.medianPitch < 7.0) continue;
        (*high)->label = "High " + (*high)->label;
        (*low)->label = "Low " + (*low)->label;
    }
    // 3. What only it is called: its preset's own name, its file's words, the user's name.
    for (auto& same : collisions()) {
        for (Description* d : same) {
            for (const std::string& own : d->distinctive) {
                if (own.empty() || containsWord(d->label, own)) continue;
                const bool shared = std::any_of(same.begin(), same.end(), [&](const Description* other) {
                    return other != d && std::find(other->distinctive.begin(), other->distinctive.end(), own) != other->distinctive.end();
                });
                if (shared) continue;
                d->label += " (" + own + ")";
                break;
            }
        }
    }
    // 4. Numbers, in the project's order.
    for (auto& same : collisions()) {
        for (size_t i = 1; i < same.size(); ++i) same[i]->label += ' ' + std::to_string(i + 1);
    }
}

TrackLabel toLabel(const Description& d) {
    TrackLabel label;
    label.label = d.label;
    label.family = familyId(d.family);
    label.role = d.role;
    std::vector<const Trait*> sorted;
    for (const Trait& trait : d.traits) sorted.push_back(&trait);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Trait* a, const Trait* b) { return a->weight > b->weight; });
    for (const Trait* trait : sorted) {
        if (trait->weight >= 0.2) label.traits.push_back(lowered(trait->word));
    }
    label.details = d.details;
    return label;
}

}  // namespace

std::vector<TrackLabel> labelTracks(const std::vector<TrackFacts>& tracks) {
    std::vector<Description> descriptions(tracks.size());
    std::map<std::string, ReturnInfo> returns;
    std::map<std::string, size_t> indexOf;
    for (size_t i = 0; i < tracks.size(); ++i) {
        indexOf[tracks[i].id] = i;
        if (tracks[i].kind == kReturn) returns[tracks[i].id] = {tracks[i].name, returnEffect(tracks[i])};
    }

    // Tracks with clips first: everything else is named after them.
    std::vector<Description*> clipTracks;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].kind != kAudio && tracks[i].kind != kMidi) continue;
        descriptions[i] = describeTrack(tracks[i], returns);
        nameClipTrack(descriptions[i]);
        clipTracks.push_back(&descriptions[i]);
    }
    weighShared(clipTracks);
    for (Description* d : clipTracks) compose(*d);
    distinguish(clipTracks);

    // Groups, the innermost first (a group holds groups).
    const auto depth = [&](size_t i) {
        int n = 0;
        for (std::string parent = tracks[i].parent; !parent.empty() && n < 64; ++n) {
            const auto it = indexOf.find(parent);
            if (it == indexOf.end()) break;
            parent = tracks[it->second].parent;
        }
        return n;
    };
    std::vector<size_t> groups;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].kind == kGroup) groups.push_back(i);
    }
    std::stable_sort(groups.begin(), groups.end(), [&](size_t a, size_t b) { return depth(a) > depth(b); });
    const auto heldBy = [&](size_t group) {  // tracks with clips anywhere in it
        int held = 0;
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].kind != kAudio && tracks[i].kind != kMidi) continue;
            int n = 0;
            for (std::string parent = tracks[i].parent; !parent.empty() && n < 64; ++n) {
                if (parent == tracks[group].id) {
                    ++held;
                    break;
                }
                const auto it = indexOf.find(parent);
                if (it == indexOf.end()) break;
                parent = tracks[it->second].parent;
            }
        }
        return held;
    };
    std::vector<Description*> groupDescriptions;
    for (const size_t g : groups) {
        std::vector<const Description*> children;
        std::vector<std::string> childLabels;
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].parent != tracks[g].id) continue;
            children.push_back(&descriptions[i]);
            childLabels.push_back(descriptions[i].label);
        }
        descriptions[g] = describeGroup(tracks[g], children, childLabels, static_cast<int>(clipTracks.size()), heldBy(g));
        compose(descriptions[g], 1);
        groupDescriptions.push_back(&descriptions[g]);
    }
    distinguish(groupDescriptions);

    std::vector<Description*> others;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].kind == kReturn) {
            descriptions[i] = describeReturn(tracks[i], returns[tracks[i].id].effect);
        } else if (tracks[i].kind == kMaster) {
            descriptions[i] = describeMaster(tracks[i]);
        } else {
            continue;
        }
        compose(descriptions[i], 1);
        if (tracks[i].kind == kReturn) others.push_back(&descriptions[i]);
    }
    distinguish(others);

    std::vector<TrackLabel> labels;
    labels.reserve(tracks.size());
    for (const Description& d : descriptions) labels.push_back(toLabel(d));
    return labels;
}

}  // namespace sub::intelligence::labels
