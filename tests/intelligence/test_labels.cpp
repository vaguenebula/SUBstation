// Track labels: the words of names (files, presets, racks, tracks), what devices
// do to the sound (classified by kind, by known plug-ins, by the words of their
// names, by their VST3 categories; how much from their parameters), what notes
// play, and the labels made from all of it: a track's, a group's, a return's;
// shared traits weighing less, tracks that come out alike told apart.

#include <chrono>
#include <string>
#include <vector>

#include "harness/Test.h"
#include "labels/Effects.h"
#include "labels/NoteProfile.h"
#include "labels/TrackLabels.h"
#include "labels/Words.h"

using namespace sub::intelligence::labels;

namespace {

// The display names of a name's words, space separated: "Sad Piano Line".
std::string words(const std::string& text, Source source = Source::File) {
    std::string out;
    for (const Word& word : readWords(text, source).words) out += (out.empty() ? "" : " ") + word.display;
    return out;
}

// A name's words, "|" between them.
std::string split(const std::string& text) {
    std::string out;
    for (const std::string& word : splitWords(text)) out += (out.empty() ? "" : "|") + word;
    return out;
}

std::string unknown(const std::string& text, Source source = Source::File) {
    std::string out;
    for (const std::string& word : readWords(text, source).unknown) out += (out.empty() ? "" : " ") + word;
    return out;
}

ParamFact percent(const std::string& name, double value) {
    return {name, name, value, value / 100.0, "%", ""};
}

ParamFact pluginParam(const std::string& name, double normalized, const std::string& text) {
    return {"1", name, normalized, normalized, "", text};
}

DeviceFact plugin(const std::string& name, const std::string& category, bool instrument = false,
                  const std::string& preset = {}, std::vector<ParamFact> params = {}) {
    DeviceFact device;
    device.kind = "plugin";
    device.name = name;
    device.category = category;
    device.instrument = instrument;
    device.preset = preset;
    device.params = std::move(params);
    return device;
}

DeviceFact builtin(const std::string& kind, const std::string& name, std::vector<ParamFact> params = {},
                   bool instrument = false) {
    DeviceFact device;
    device.kind = kind;
    device.name = name;
    device.params = std::move(params);
    device.instrument = instrument;
    return device;
}

DeviceFact serum(const std::string& preset) { return plugin("Serum 2", "Instrument|Synth", true, preset); }

DeviceFact reverb(double wet) {
    return plugin("ValhallaVintageVerb", "Fx|Reverb", false, "Default",
                  {pluginParam("Mix", wet / 100.0, std::to_string(static_cast<int>(wet)) + ".0 %")});
}

// Notes struck together (a chord) every `every` beats from 0, `count` times, `length` long.
std::vector<NoteFact> chords(std::initializer_list<int> pitches, int count, double every, double length) {
    std::vector<NoteFact> notes;
    for (int i = 0; i < count; ++i) {
        for (const int pitch : pitches) notes.push_back({pitch, i * every, i * every + length, 100});
    }
    return notes;
}

// One note after another, cycling through `pitches`.
std::vector<NoteFact> line(std::initializer_list<int> pitches, int count, double length) {
    std::vector<NoteFact> notes;
    const std::vector<int> cycle(pitches);
    for (int i = 0; i < count; ++i) notes.push_back({cycle[i % cycle.size()], i * length, (i + 1) * length, 100});
    return notes;
}

TrackFacts track(const std::string& id, const std::string& kind, std::vector<DeviceFact> devices = {}) {
    TrackFacts facts;
    facts.id = id;
    facts.kind = kind;
    facts.name = id;
    facts.devices = std::move(devices);
    return facts;
}

TrackFacts audioTrack(const std::string& id, std::initializer_list<std::string> files, std::vector<DeviceFact> devices = {}) {
    TrackFacts facts = track(id, kAudio, std::move(devices));
    for (const std::string& file : files) facts.audio.push_back({file, 4.0});
    return facts;
}

TrackLabel only(const TrackFacts& facts) { return labelTracks({facts}).front(); }

bool has(const std::vector<std::string>& list, const std::string& item) {
    for (const std::string& x : list) {
        if (x == item) return true;
    }
    return false;
}

bool mentions(const std::vector<std::string>& details, const std::string& text) {
    for (const std::string& line : details) {
        if (line.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// --- Words ------------------------------------------------------------------------------------------

TEST_CASE("names split into words") {
    CHECK_EQ(split("C_BellStab1"), std::string("c|bell|stab|1"));
    CHECK_EQ(split("Cymbals_ClosedHiHat3"), std::string("cymbals|closed|hi|hat|3"));
    CHECK_EQ(split("ct_fill110_rousey"), std::string("ct|fill|110|rousey"));
    CHECK_EQ(split("XMLParser 808s"), std::string("xml|parser|808|s"));
    CHECK_EQ(split("VRB2_Missme_A#min"), std::string("vrb|2|missme|a#min"));
    CHECK(splitWords("568f8e0e6dab4e2487ceaa0d65fe95af").empty());  // a checksum says nothing
}

TEST_CASE("what files' names say") {
    // Pack codes, tempos and keys go; what is left is the sound.
    CHECK_EQ(words("OS_LDNB_174_Amaj_Sad_Piano_Line"), std::string("Liquid DnB Sad Piano Line"));
    CHECK_EQ(words("VRB2_Missme_Acapella_DRY_120_A#min"), std::string("Acapella"));
    CHECK_EQ(unknown("VRB2_Missme_Acapella_DRY_120_A#min"), std::string("missme"));
    CHECK_EQ(words("Cymbals_ClosedHiHat3"), std::string("Cymbal Closed Hat"));
    CHECK_EQ(words("ct_clhat_quick"), std::string("Closed Hat"));
    CHECK_EQ(words("DS_MDS_kick_one_shot_wash"), std::string("Kick One-Shot"));
    CHECK_EQ(words("KSHMR Sweep Down 02"), std::string("Downlifter"));
    CHECK_EQ(words("C_BellStab1"), std::string("Bell Stab"));
    CHECK_EQ(words("WindStormForest"), std::string("Wind Storm Forest"));
    CHECK_EQ(words("91V_FRF_174_bass_synth_club_greedy_Fm"), std::string("Bass Synth"));
    // Plurals and words glued together.
    CHECK_EQ(words("big_risers"), std::string("Big Riser"));
    CHECK_EQ(words("funky_bassline_01"), std::string("Funky Bass Line"));
    // Capitals are a pack's, never a name to tell tracks apart by.
    CHECK_EQ(unknown("018_Snare_-_DECODEDDRUMBASS_Zenhiser"), std::string());
}

TEST_CASE("what presets' names say") {
    // A preset's category codes...
    CHECK_EQ(words("PL - Electric Flow", Source::Preset), std::string("Pluck Electric"));
    CHECK_EQ(words("BS - Dirty Fat BASS", Source::Preset), std::string("Bass Dirty Fat Bass"));
    CHECK_EQ(words("AOKI_serum_BA_big_bed_bass", Source::Preset), std::string("Bass Big Bass"));
    // ...are a pack's code in a file's name.
    CHECK_EQ(words("PL_Electric_Flow", Source::File), std::string("Electric"));
    // A genre is no sound: "Future Bass" is no bass.
    const Reading futureBass = readWords("Future Bass - Shlow", Source::Preset);
    REQUIRE(futureBass.words.size() == 1);
    CHECK(futureBass.words[0].kind == WordKind::Genre);
    CHECK_EQ(words("Doggy Woggies D&B - Fat BASS", Source::Preset), std::string("DnB Fat Bass"));
    // An orchestra's.
    CHECK_EQ(words("Core - Violins 1", Source::Preset), std::string("Violins"));
    CHECK_EQ(words("Core - Basses", Source::Preset), std::string("Double Bass"));
}

// --- Effects ----------------------------------------------------------------------------------------

TEST_CASE("what effects are") {
    CHECK(classify(builtin("delay", "Delay")) == Effect::Delay);
    CHECK(classify(builtin("ott", "Over The Top")) == Effect::Ott);
    CHECK(classify(builtin("sidechain", "Sidechain")) == Effect::Ducking);
    CHECK(classify(builtin("synth", "Synth", {}, true)) == Effect::Instrument);
    CHECK(classify(serum("")) == Effect::Instrument);
    // The words of their names: "verb" a reverb, "dist" a distortion...
    CHECK(classify(plugin("ValhallaVintageVerb", "Fx")) == Effect::Reverb);
    CHECK(classify(plugin("Gayverb", "")) == Effect::Reverb);
    CHECK(classify(plugin("MDistortionMB", "")) == Effect::Distortion);
    CHECK(classify(plugin("Saturation Knob", "Fx|Distortion")) == Effect::Saturation);
    CHECK(classify(plugin("kHs Delay", "Fx|Spatial")) == Effect::Delay);  // its name over its category
    CHECK(classify(plugin("Pro-C 2", "Fx|Dynamics")) == Effect::Compressor);
    CHECK(classify(plugin("Pro-Q 4", "Fx|EQ")) == Effect::Eq);
    CHECK(classify(plugin("smartEQ3", "")) == Effect::Eq);
    CHECK(classify(plugin("Ozone 11 Imager", "Fx|Spatial")) == Effect::Width);
    CHECK(classify(plugin("Ozone 11", "Fx|Mastering")) == Effect::Mastering);
    // ...but some names mislead: these come first.
    CHECK(classify(plugin("SPL De-Verb Plus", "Fx|Dynamics")) == Effect::Restoration);
    CHECK(classify(plugin("Trackspacer 2.5", "Fx")) == Effect::Ducking);
    CHECK(classify(plugin("ValhallaSupermassive", "Fx")) == Effect::Reverb);
    CHECK(classify(plugin("Crystalline", "Fx")) == Effect::Reverb);
    CHECK(classify(plugin("soothe2", "Fx|Dynamics|Mastering")) == Effect::Resonance);
    CHECK(classify(plugin("kHs Tape Stop", "Fx")) == Effect::Glitch);
    CHECK(classify(plugin("kHs Phase Distortion", "Fx|Distortion")) == Effect::Distortion);
    CHECK(classify(plugin("OTT", "Fx")) == Effect::Ott);
    CHECK(classify(plugin("Bottle Rocket", "")) == Effect::None);  // ("ott" only as a word)
    // Else its category; else nothing.
    CHECK(classify(plugin("Mystery Box", "Fx|Reverb")) == Effect::Reverb);
    CHECK(classify(plugin("Mystery Box", "Fx")) == Effect::None);
}

TEST_CASE("how much effects do") {
    // A reverb by how wet: washed out, spacious, roomy; dry, nothing.
    const auto first = [](const DeviceFact& device) {
        const DeviceReading reading = readDevice(device);
        return reading.traits.empty() ? std::string() : reading.traits.front().word;
    };
    CHECK_EQ(first(reverb(50)), std::string("Washed Out"));
    CHECK_EQ(first(reverb(30)), std::string("Spacious"));
    CHECK_EQ(first(reverb(15)), std::string("Roomy"));
    CHECK_EQ(first(reverb(0)), std::string());
    CHECK_EQ(first(plugin("ValhallaPlate", "Fx")), std::string("Spacious"));  // not loaded: how wet unknown
    CHECK_EQ(readDevice(reverb(50)).summary, std::string("ValhallaVintageVerb: reverb, 50% wet"));
    // Off: nothing.
    DeviceFact off = reverb(50);
    off.enabled = false;
    CHECK(readDevice(off).traits.empty());
    CHECK_EQ(readDevice(off).summary, std::string("ValhallaVintageVerb: reverb (off)"));

    const DeviceReading delay = readDevice(builtin("delay", "Delay", {percent("mix", 40), percent("feedback", 75)}));
    REQUIRE(delay.traits.size() == 1);
    CHECK_EQ(delay.traits[0].word, std::string("Echoing"));
    CHECK_APPROX_TOL(delay.traits[0].weight, 0.8, 1e-9, 1e-9);
    CHECK_EQ(delay.summary, std::string("Delay, 40% wet, 75% feedback"));

    CHECK_EQ(first(builtin("ott", "Over The Top", {percent("depth", 77)})), std::string("Squashed"));
    CHECK_EQ(first(builtin("utility", "Utility", {{"width", "Width", 180, 0.9, "%", ""}})), std::string("Wide"));
    CHECK_EQ(first(builtin("utility", "Utility", {{"width", "Width", 0, 0, "%", ""}})), std::string("Mono"));
    CHECK_EQ(first(plugin("Wider", "Fx|Spatial")), std::string("Wide"));
    CHECK_EQ(first(builtin("sidechain", "Sidechain", {percent("depth", 100)})), std::string("Pumping"));

    // A built-in EQ by its cuts.
    const auto band = [](int n, int type, double freq) {
        const std::string b = "b" + std::to_string(n) + "_";
        return std::vector<ParamFact>{{b + "used", "", 1, 1, "", ""},
                                      {b + "on", "", 1, 1, "", ""},
                                      {b + "type", "", static_cast<double>(type), 0, "", ""},
                                      {b + "freq", "", freq, 0, "Hz", ""}};
    };
    const auto eq = [&](std::vector<std::vector<ParamFact>> bands) {
        std::vector<ParamFact> params;
        for (auto& b : bands) params.insert(params.end(), b.begin(), b.end());
        return builtin("eq", "EQ", params);
    };
    CHECK_EQ(first(eq({band(1, 4, 1200)})), std::string("Muffled"));
    CHECK_EQ(first(eq({band(1, 4, 3000)})), std::string("Dark"));
    CHECK_EQ(first(eq({band(1, 2, 900)})), std::string("Thin"));
    CHECK_EQ(first(eq({band(1, 2, 400), band(2, 4, 3500)})), std::string("Telephone"));
    CHECK(readDevice(eq({band(1, 2, 100)})).traits.empty());  // cleaning the lows says nothing
    CHECK_EQ(readDevice(eq({band(1, 2, 100)})).summary, std::string("EQ, low cut 100 Hz"));
}

TEST_CASE("parameters' values") {
    CHECK_APPROX(fraction(pluginParam("Mix", 0.2, "50.0 %")), 0.5);  // its text says best
    CHECK_APPROX_TOL(fraction({"wet", "Wet", -6.0206, 0.9, "dB", ""}), 0.5, 1e-3, 1e-3);
    CHECK_APPROX(fraction(pluginParam("Mix", 0.3, "")), 0.3);
    CHECK(readsParam("Dry/Wet"));
    CHECK(readsParam("Mix"));
    CHECK(!readsParam("Filter 1 Wet"));
    // What a plug-in's saved state says, where it keeps its values as text.
    const std::vector<ParamFact> saved =
        paramsInState(R"(<ValhallaSupermassive presetName="Default" Mix="0.5" Feedback="0.25" Width="1.0"/>)"
                      R"(<PARAM id="drive" value="0.75"/><PARAM id="mix" value="0.1"/><x Depth="12"/>)");
    REQUIRE(saved.size() == 3);
    CHECK_EQ(saved[0].name, std::string("Mix"));
    CHECK_APPROX(saved[0].normalized, 0.5);
    CHECK_EQ(saved[1].name, std::string("Feedback"));
    CHECK_EQ(saved[2].name, std::string("drive"));  // (the second mix not taken; a depth of 12 isn't 0..1)
}

// --- Notes ------------------------------------------------------------------------------------------

TEST_CASE("what notes play") {
    const auto reading = [](const std::vector<NoteFact>& notes) {
        const NoteReading r = readNotes(profile(notes));
        return r.sound + "/" + r.pattern;
    };
    CHECK_EQ(reading(chords({60, 64, 67}, 8, 1.0, 1.0)), std::string("/Chords"));
    CHECK_EQ(reading(chords({48, 55, 60, 64}, 4, 4.0, 4.0)), std::string("Pad/Chords"));
    CHECK_EQ(reading(chords({60, 64, 67}, 16, 0.5, 0.25)), std::string("Stab/"));
    CHECK_EQ(reading(line({36, 36, 43, 41}, 16, 0.5)), std::string("Sub Bass/"));
    CHECK_EQ(reading(line({45, 45, 52, 50}, 16, 0.5)), std::string("Bass/"));
    CHECK_EQ(reading(line({60, 64, 67, 72, 76, 79}, 32, 0.25)), std::string("/Arp"));
    CHECK_EQ(reading(line({72, 74, 76, 79}, 16, 0.5)), std::string("Lead/Line"));
    CHECK_EQ(reading({{60, 0, 1, 100}}), std::string("/"));  // too little to tell

    const NoteProfile p = profile(chords({60, 64, 67}, 8, 1.0, 1.0));
    CHECK_EQ(p.count, 24);
    CHECK_APPROX(p.voices, 3.0);
    CHECK_APPROX(p.chordShare, 1.0);
    CHECK_EQ(noteName(60), std::string("C3"));
    CHECK_EQ(noteName(25), std::string("C#0"));
    CHECK_EQ(describe(p), std::string("24 notes, chords, C3-G3, mostly 1/4 notes"));
}

// --- Labels -----------------------------------------------------------------------------------------

TEST_CASE("a pluck washed out by its reverb") {
    // A Serum preset of the pluck category, through a reverb half wet.
    TrackFacts pluck = track("1", kMidi, {serum("PLUCK - Dynasty"), reverb(50)});
    pluck.name = "1 Serum 2";
    pluck.notes = line({72, 76, 79, 76}, 16, 0.5);
    const TrackLabel label = only(pluck);
    CHECK_EQ(label.label, std::string("Washed Out Serum Pluck"));
    CHECK_EQ(label.family, std::string("synth"));
    CHECK_EQ(label.role, std::string("pluck"));
    CHECK(has(label.traits, "washed out"));
    CHECK(mentions(label.details, "Instrument: Serum 2, preset \u201CPLUCK - Dynasty\u201D"));
    CHECK(mentions(label.details, "ValhallaVintageVerb: reverb, 50% wet"));
    CHECK(mentions(label.details, "Notes: 16 notes, one at a time"));
    CHECK(mentions(label.details, "Mixer: 0.0 dB"));
    // A dry one is just a pluck.
    CHECK_EQ(only(track("1", kMidi, {serum("PL - Electric Flow")})).label, std::string("Serum Pluck"));
}

TEST_CASE("instruments, presets and samples name what plays") {
    CHECK_EQ(only(track("1", kMidi, {plugin("BBC Symphony Orchestra", "Instrument|Synth", true, "Core - Trumpet")})).label,
             std::string("Orchestral Trumpet"));
    CHECK_EQ(only(track("1", kMidi, {plugin("BBC Symphony Orchestra", "Instrument|Synth", true, "Core - Trumpet")})).family,
             std::string("brass"));
    CHECK_EQ(only(track("1", kMidi, {serum("BS - Dirty Fat BASS")})).label, std::string("Dirty Serum Bass"));
    // A synth's flavour words don't make it what they name: a lead "like a vocal" is a synth.
    const TrackLabel vocalLead = only(track("1", kMidi, {serum("LEAD - Sounds Like a Vocal Shot")}));
    CHECK_EQ(vocalLead.label, std::string("Serum Vocal Lead"));
    CHECK_EQ(vocalLead.family, std::string("synth"));
    // Spitfire's family is in its plug-in's name.
    TrackFacts piano = track("1", kMidi, {plugin("Originals - Cinematic Soft Piano", "Instrument|Synth", true, "Washed Out")});
    piano.notes = chords({48, 55, 64}, 8, 2.0, 2.0);
    CHECK_EQ(only(piano).label, std::string("Washed Out Piano Chords"));
    // A sampler by its sample; a built-in synth by its envelope.
    DeviceFact sampler = builtin("sampler", "Sampler", {}, true);
    sampler.sample = "C:/Samples/808s/TR_808_long_F.wav";
    CHECK_EQ(only(track("1", kMidi, {sampler})).label, std::string("808"));
    DeviceFact synth = builtin("synth", "Synth",
                               {{"attack", "Attack", 2, 0, "ms", ""}, {"decay", "Decay", 200, 0, "ms", ""},
                                {"sustain", "Sustain", 0, 0, "%", ""}, {"wave", "Wave", 2, 0, "", ""}},
                               true);
    CHECK_EQ(only(track("1", kMidi, {synth})).label, std::string("Synth Pluck"));
    // A rack named by the user says what it holds.
    DeviceFact rack = builtin("rack", "Big Bed Bass");
    rack.chains = {{serum("- Init -")}};
    CHECK_EQ(only(track("1", kMidi, {rack})).label, std::string("Big Serum Bass"));
}

TEST_CASE("notes name what nothing else does") {
    TrackFacts bass = track("1", kMidi, {serum("- Init -")});
    bass.notes = line({36, 36, 43, 41}, 16, 0.5);
    CHECK_EQ(only(bass).label, std::string("Serum Sub Bass"));
    TrackFacts keys = track("1", kMidi, {plugin("Kontakt 7", "Instrument", true)});
    keys.notes = chords({60, 64, 67}, 8, 1.0, 1.0);
    CHECK_EQ(only(keys).label, std::string("Kontakt Chords"));
    // But not over what the instrument is: a trumpet's notes are a trumpet's.
    TrackFacts trumpet = track("1", kMidi, {plugin("BBC Symphony Orchestra", "Instrument|Synth", true, "Core - Trumpet")});
    trumpet.notes = line({72, 74, 76, 79}, 16, 0.5);
    CHECK_EQ(only(trumpet).label, std::string("Orchestral Trumpet"));
}

TEST_CASE("audio tracks by their files") {
    CHECK_EQ(only(audioTrack("1", {"C:/Loops/OS_LDNB_174_Amaj_Sad_Piano_Line.wav"})).label, std::string("Sad Piano Line"));
    CHECK_EQ(only(audioTrack("1", {"C_BellStab1.wav"}, {builtin("delay", "Delay", {percent("mix", 40)})})).label,
             std::string("Echoing Bell Stab"));
    CHECK_EQ(only(audioTrack("1", {"PHT_loop_acoustic_breakbeat_11_150.wav"})).label, std::string("Acoustic Breakbeat Loop"));
    CHECK_EQ(only(audioTrack("1", {"ESM_drums_full_loop_cinematic.wav"})).label, std::string("Cinematic Drum Loop"));
    CHECK_EQ(only(audioTrack("1", {"WindStormForest_S011SSFX.40.wav"})).label, std::string("Wind Ambience"));
    // The file most played names it.
    TrackFacts mixed = audioTrack("1", {"snare_01.wav"});
    mixed.audio.push_back({"Kick_Deep.wav", 32.0});
    CHECK_EQ(only(mixed).label, std::string("Deep Kick"));
    // A take recorded here; a file nothing names (its own words); nothing at all.
    CHECK_EQ(only(audioTrack("1", {"Vox 2026-10-08 141500.wav"})).label, std::string("Vocal Recording"));
    CHECK_EQ(only(audioTrack("1", {"Vox 2026-10-08 141500 2_.wav"})).label, std::string("Vocal Recording"));
    CHECK_EQ(only(audioTrack("1", {"Vox 2026-10-08 1415.wav"})).label, std::string("Vocal"));  // (no take's time)
    CHECK_EQ(only(audioTrack("1", {"Rousey_Thing_04.wav"})).label, std::string("Rousey Thing"));
    CHECK_EQ(only(track("1", kAudio)).label, std::string("Empty Audio"));
    CHECK_EQ(only(track("1", kMidi)).label, std::string("Empty MIDI"));
}

TEST_CASE("the user's own name counts most") {
    TrackFacts vocals = audioTrack("1", {"take_01.wav"});
    vocals.name = "lead vocals";
    vocals.namedByUser = true;
    CHECK_EQ(only(vocals).label, std::string("Vocal Lead"));
    vocals.namedByUser = false;  // the same words given by SUBstation say nothing
    CHECK_EQ(only(vocals).label, std::string("Audio"));
}

TEST_CASE("the mixer and the sends") {
    TrackFacts quiet = track("1", kMidi, {serum("PL - Quiet")});
    quiet.volumeDb = -32.0;
    quiet.pan = -0.8;
    const TrackLabel label = only(quiet);
    CHECK_EQ(label.label, std::string("Quiet Serum Pluck"));  // (a second trait only if strong: labels stay short)
    CHECK(has(label.traits, "hard-left"));
    CHECK(mentions(label.details, "Mixer: -32.0 dB, 80% left"));

    // Sent to a reverb return: washed out as much as its own reverb would.
    TrackFacts sent = track("1", kMidi, {serum("PL - Dry")});
    sent.sends = {{"r", -2.0}};
    TrackFacts verb = track("r", kReturn, {reverb(100)});
    verb.name = "A";
    const std::vector<TrackLabel> labels = labelTracks({sent, verb});
    CHECK_EQ(labels[0].label, std::string("Washed Out Serum Pluck"));
    CHECK(mentions(labels[0].details, "Send: A (reverb) at -2.0 dB"));
    CHECK_EQ(labels[1].label, std::string("Reverb Return"));  // (all wet, as a return is: not washed out)
    CHECK_EQ(labels[1].role, std::string("return"));
}

TEST_CASE("groups, returns and the master") {
    TrackFacts kick = audioTrack("k", {"Kick_01.wav"});
    TrackFacts snare = audioTrack("s", {"Snare_01.wav"});
    TrackFacts hats = audioTrack("h", {"Hats_Loop.wav"});
    TrackFacts drums = track("d", kGroup, {builtin("sidechain", "Sidechain", {percent("depth", 100)})});
    TrackFacts empty = track("e", kGroup);
    TrackFacts named = track("n", kGroup);
    named.name = "synths";
    named.namedByUser = true;
    kick.parent = snare.parent = hats.parent = "d";
    TrackFacts master = track("master", kMaster, {plugin("kHs Limiter", "Fx|Dynamics")});
    const std::vector<TrackLabel> labels = labelTracks({drums, kick, snare, hats, empty, named, master});
    CHECK_EQ(labels[0].label, std::string("Pumping Drum Group"));
    CHECK_EQ(labels[0].family, std::string("drums"));
    CHECK(mentions(labels[0].details, "Holds: Kick, Snare, Hi-Hat Loop"));
    CHECK_EQ(labels[4].label, std::string("Empty Group"));
    CHECK_EQ(labels[5].label, std::string("Synth Group"));  // what the user called it
    CHECK_EQ(labels[6].label, std::string("Master"));       // (limited, as masters are: said in its details)
    CHECK(mentions(labels[6].details, "kHs Limiter: limiter"));

    // A group that holds most of the song is its mix bus; groups of a kind are told apart by what they hold.
    TrackFacts bus = track("b", kGroup);
    TrackFacts other = track("o", kGroup);
    TrackFacts lead = track("l", kMidi, {serum("LD - Big")});
    TrackFacts pad = track("p", kMidi, {plugin("Kontakt 7", "Instrument", true, "Strings Ensemble")});
    TrackFacts vox = audioTrack("v", {"vocal_hook.wav"});
    lead.parent = pad.parent = vox.parent = "b";
    TrackFacts kick2 = audioTrack("k2", {"Kick_02.wav"});
    TrackFacts hats2 = audioTrack("h2", {"OpenHat_02.wav"});
    kick2.parent = hats2.parent = "o";
    TrackFacts kick3 = audioTrack("k3", {"Kick_03.wav"});
    TrackFacts snare3 = audioTrack("s3", {"Snare_03.wav"});
    TrackFacts third = track("t", kGroup);
    kick3.parent = snare3.parent = "t";
    const std::vector<TrackLabel> more = labelTracks({bus, lead, pad, vox, other, kick2, hats2, third, kick3, snare3});
    CHECK_EQ(more[0].label, std::string("Mixed Group"));
    CHECK_EQ(more[4].label, std::string("Drum Group (Kick, Open Hat)"));
    CHECK_EQ(more[7].label, std::string("Drum Group (Kick, Snare)"));
}

TEST_CASE("traits most tracks share weigh less") {
    // A reverb a third wet on every track says little of any one; one track washed out says much.
    std::vector<TrackFacts> tracks;
    for (const char* preset : {"PL - One", "LD - Two", "BS - Three", "PD - Four"}) {
        tracks.push_back(track(std::to_string(tracks.size()), kMidi, {serum(preset), reverb(30)}));
    }
    tracks.push_back(track("5", kMidi, {serum("KY - Five"), reverb(60)}));
    const std::vector<TrackLabel> labels = labelTracks(tracks);
    CHECK_EQ(labels[0].label, std::string("Serum Pluck"));
    CHECK_EQ(labels[1].label, std::string("Serum Lead"));
    CHECK_EQ(labels[2].label, std::string("Serum Bass"));
    CHECK_EQ(labels[3].label, std::string("Serum Pad"));
    CHECK_EQ(labels[4].label, std::string("Washed Out Serum Keys"));
    CHECK(has(labels[0].traits, "spacious"));  // (still a trait of it, for agents)
}

TEST_CASE("tracks alike are told apart") {
    // By a trait the others lack.
    TrackFacts a = track("a", kMidi, {serum("PL - Glass")});
    TrackFacts b = track("b", kMidi, {serum("PL - Glass"), builtin("delay", "Delay", {percent("mix", 10)})});
    std::vector<TrackLabel> labels = labelTracks({a, b});
    CHECK_EQ(labels[0].label, std::string("Serum Pluck"));
    CHECK_EQ(labels[1].label, std::string("Echoing Serum Pluck"));
    // By their register.
    TrackFacts high = track("h", kMidi, {serum("- Init -")});
    TrackFacts low = track("l", kMidi, {serum("- Init -")});
    high.notes = chords({72, 76, 79}, 8, 1.0, 1.0);
    low.notes = chords({48, 52, 55}, 8, 1.0, 1.0);
    labels = labelTracks({high, low});
    CHECK_EQ(labels[0].label, std::string("High Serum Chords"));
    CHECK_EQ(labels[1].label, std::string("Low Serum Chords"));
    // By their presets' own names.
    labels = labelTracks({track("1", kMidi, {serum("PL - Electric Flow")}), track("2", kMidi, {serum("PL - Dynasty")})});
    CHECK_EQ(labels[0].label, std::string("Serum Pluck (Electric Flow)"));
    CHECK_EQ(labels[1].label, std::string("Serum Pluck (Dynasty)"));
    // By their files' own words.
    labels = labelTracks({audioTrack("1", {"VR_fx_riser_stratosphere.wav"}), audioTrack("2", {"riser_sustained.wav"})});
    CHECK_EQ(labels[0].label, std::string("Riser (Stratosphere)"));
    CHECK_EQ(labels[1].label, std::string("Riser (Sustained)"));
    // By how much a trait says of a track itself, however many share it: of four arps alike, the quiet ones.
    std::vector<TrackFacts> arps;
    for (const double db : {0.0, -32.0, -32.0, -40.0}) {
        TrackFacts arp = track(std::to_string(arps.size()), kMidi, {serum("- Init -")});
        arp.notes = line({60, 64, 67, 72, 76, 79}, 32, 0.25);
        arp.volumeDb = db;
        arps.push_back(std::move(arp));
    }
    labels = labelTracks(arps);
    CHECK_EQ(labels[0].label, std::string("Serum Arp"));
    CHECK_EQ(labels[1].label, std::string("Quiet Serum Arp"));
    CHECK_EQ(labels[2].label, std::string("Quiet Serum Arp 2"));
    CHECK_EQ(labels[3].label, std::string("Quiet Serum Arp 3"));
    // Else numbered, in order.
    labels = labelTracks({audioTrack("1", {"Kick.wav"}), audioTrack("2", {"Kick.wav"}), audioTrack("3", {"Kick.wav"})});
    CHECK_EQ(labels[0].label, std::string("Kick"));
    CHECK_EQ(labels[1].label, std::string("Kick 2"));
    CHECK_EQ(labels[2].label, std::string("Kick 3"));
}

TEST_CASE("the same facts give the same labels, fast") {
    std::vector<TrackFacts> tracks;
    for (int i = 0; i < 200; ++i) {
        TrackFacts t = track(std::to_string(i), kMidi, {serum(i % 2 ? "PL - Glass" : "BS - Deep"), reverb(i % 70)});
        t.notes = line({48 + i % 24, 52, 55, 60}, 500, 0.25);
        tracks.push_back(std::move(t));
    }
    const auto start = std::chrono::steady_clock::now();
    const std::vector<TrackLabel> first = labelTracks(tracks);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const std::vector<TrackLabel> second = labelTracks(tracks);
    for (size_t i = 0; i < first.size(); ++i) {
        CHECK_EQ(first[i].label, second[i].label);
        CHECK(first[i].details == second[i].details);
    }
    INFO("200 tracks of 500 notes: " + std::to_string(ms) + " ms");
    CHECK(ms < 2000.0);  // (a debug build's bound; a release build takes a few milliseconds)
}
