#include "labels/Words.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace sub::intelligence::labels {

namespace {

using K = WordKind;
using F = Family;

// Which sources a word is read in (Source as bits).
constexpr uint8_t kName = 1, kFile = 2, kPreset = 4, kAll = kName | kFile | kPreset;

uint8_t bit(Source source) {
    switch (source) {
    case Source::Name: return kName;
    case Source::File: return kFile;
    case Source::Preset: return kPreset;
    }
    return kAll;
}

struct Entry {
    std::string_view text;  // lower case; words of a phrase separated by one space
    WordKind kind;
    Family family;
    int rank;
    std::string_view display;
    uint8_t sources = kAll;
};

// The vocabulary. Phrases are at most three words. A preset's category codes
// (Serum's and most sound designers': "PL - ", "BS - ", "LD - ") are read in
// presets only: in a file's name, short capitals are a sample pack's code.
const Entry kEntries[] = {
    // Drums.
    {"kick", K::Instrument, F::Drums, 3, "Kick"},
    {"kicks", K::Instrument, F::Drums, 3, "Kick"},
    {"kik", K::Instrument, F::Drums, 3, "Kick"},
    {"kickdrum", K::Instrument, F::Drums, 3, "Kick"},
    {"bass drum", K::Instrument, F::Drums, 3, "Kick"},
    {"bd", K::Instrument, F::Drums, 3, "Kick", kFile},
    {"kk", K::Instrument, F::Drums, 3, "Kick", kPreset},
    {"snare", K::Instrument, F::Drums, 3, "Snare"},
    {"snares", K::Instrument, F::Drums, 3, "Snare"},
    {"snr", K::Instrument, F::Drums, 3, "Snare"},
    {"snare drum", K::Instrument, F::Drums, 3, "Snare"},
    {"sn", K::Instrument, F::Drums, 3, "Snare", kPreset},
    {"clap", K::Instrument, F::Drums, 3, "Clap"},
    {"claps", K::Instrument, F::Drums, 3, "Clap"},
    {"handclap", K::Instrument, F::Drums, 3, "Clap"},
    {"hat", K::Instrument, F::Drums, 3, "Hi-Hat"},
    {"hats", K::Instrument, F::Drums, 3, "Hi-Hat"},
    {"hihat", K::Instrument, F::Drums, 3, "Hi-Hat"},
    {"hihats", K::Instrument, F::Drums, 3, "Hi-Hat"},
    {"hi hat", K::Instrument, F::Drums, 3, "Hi-Hat"},
    {"hi hats", K::Instrument, F::Drums, 3, "Hi-Hat"},
    {"hh", K::Instrument, F::Drums, 3, "Hi-Hat"},
    {"closed hat", K::Instrument, F::Drums, 4, "Closed Hat"},
    {"closed hihat", K::Instrument, F::Drums, 4, "Closed Hat"},
    {"closed hi hat", K::Instrument, F::Drums, 4, "Closed Hat"},
    {"closedhat", K::Instrument, F::Drums, 4, "Closed Hat"},
    {"clhat", K::Instrument, F::Drums, 4, "Closed Hat"},
    {"chh", K::Instrument, F::Drums, 4, "Closed Hat", kFile},
    {"open hat", K::Instrument, F::Drums, 4, "Open Hat"},
    {"open hihat", K::Instrument, F::Drums, 4, "Open Hat"},
    {"open hi hat", K::Instrument, F::Drums, 4, "Open Hat"},
    {"openhat", K::Instrument, F::Drums, 4, "Open Hat"},
    {"ophat", K::Instrument, F::Drums, 4, "Open Hat"},
    {"ohat", K::Instrument, F::Drums, 4, "Open Hat"},
    {"ohh", K::Instrument, F::Drums, 4, "Open Hat", kFile},
    {"ride", K::Instrument, F::Drums, 3, "Ride"},
    {"crash", K::Instrument, F::Drums, 3, "Crash"},
    {"cymbal", K::Instrument, F::Drums, 2, "Cymbal"},
    {"cymbals", K::Instrument, F::Drums, 2, "Cymbal"},
    {"tom", K::Instrument, F::Drums, 3, "Tom"},
    {"toms", K::Instrument, F::Drums, 3, "Tom"},
    {"rim", K::Instrument, F::Drums, 3, "Rim"},
    {"rimshot", K::Instrument, F::Drums, 3, "Rim"},
    {"rim shot", K::Instrument, F::Drums, 3, "Rim"},
    {"shaker", K::Instrument, F::Drums, 3, "Shaker"},
    {"tambourine", K::Instrument, F::Drums, 3, "Tambourine"},
    {"tamb", K::Instrument, F::Drums, 3, "Tambourine"},
    {"conga", K::Instrument, F::Drums, 3, "Conga"},
    {"congas", K::Instrument, F::Drums, 3, "Conga"},
    {"bongo", K::Instrument, F::Drums, 3, "Bongo"},
    {"bongos", K::Instrument, F::Drums, 3, "Bongo"},
    {"cowbell", K::Instrument, F::Drums, 3, "Cowbell"},
    {"snap", K::Instrument, F::Drums, 3, "Snap"},
    {"snaps", K::Instrument, F::Drums, 3, "Snap"},
    {"perc", K::Instrument, F::Drums, 2, "Percussion"},
    {"percs", K::Instrument, F::Drums, 2, "Percussion"},
    {"percussion", K::Instrument, F::Drums, 2, "Percussion"},
    {"drum", K::Instrument, F::Drums, 1, "Drums"},
    {"drums", K::Instrument, F::Drums, 1, "Drums"},
    {"drumkit", K::Instrument, F::Drums, 1, "Drums"},
    {"drummer", K::Instrument, F::Drums, 1, "Drums"},
    {"dr", K::Instrument, F::Drums, 1, "Drums", kPreset},
    {"drm", K::Instrument, F::Drums, 1, "Drums", kPreset},
    {"breakbeat", K::Instrument, F::Drums, 3, "Breakbeat"},
    {"breakbeats", K::Instrument, F::Drums, 3, "Breakbeat"},
    {"break", K::Instrument, F::Drums, 3, "Breakbeat", kFile},
    {"breaks", K::Instrument, F::Drums, 3, "Breakbeat", kFile},
    {"top loop", K::Instrument, F::Drums, 3, "Top Loop"},
    {"toploop", K::Instrument, F::Drums, 3, "Top Loop"},
    {"fill", K::Pattern, F::Drums, 1, "Fill"},
    {"fills", K::Pattern, F::Drums, 1, "Fill"},
    {"roll", K::Pattern, F::Drums, 1, "Roll"},
    {"rolls", K::Pattern, F::Drums, 1, "Roll"},
    {"groove", K::Pattern, F::Drums, 0, "Groove"},
    {"beat", K::Pattern, F::Drums, 0, "Beat"},
    // Bass.
    {"bass", K::Sound, F::Bass, 2, "Bass"},
    {"ba", K::Sound, F::Bass, 2, "Bass", kPreset},
    {"bs", K::Sound, F::Bass, 2, "Bass", kPreset},
    {"bas", K::Sound, F::Bass, 2, "Bass", kPreset},
    {"sub", K::Sound, F::Bass, 3, "Sub Bass"},
    {"subs", K::Sound, F::Bass, 3, "Sub Bass"},
    {"sub bass", K::Sound, F::Bass, 4, "Sub Bass"},
    {"subbass", K::Sound, F::Bass, 4, "Sub Bass"},
    {"sb", K::Sound, F::Bass, 4, "Sub Bass", kPreset},
    {"808", K::Sound, F::Bass, 4, "808"},
    {"reese", K::Sound, F::Bass, 4, "Reese Bass"},
    {"rs", K::Sound, F::Bass, 4, "Reese Bass", kPreset},
    {"wobble", K::Sound, F::Bass, 3, "Wobble Bass"},
    {"wob", K::Sound, F::Bass, 3, "Wobble Bass"},
    {"growl", K::Sound, F::Bass, 3, "Growl Bass"},
    {"bass guitar", K::Instrument, F::Bass, 4, "Bass Guitar"},
    {"double bass", K::Instrument, F::Strings, 4, "Double Bass"},
    {"upright bass", K::Instrument, F::Strings, 4, "Double Bass"},
    {"contrabass", K::Instrument, F::Strings, 4, "Double Bass"},
    {"basses", K::Instrument, F::Strings, 4, "Double Bass"},  // (an orchestra's: "Core - Basses")
    // Keys.
    {"piano", K::Instrument, F::Keys, 3, "Piano"},
    {"pianos", K::Instrument, F::Keys, 3, "Piano"},
    {"pno", K::Instrument, F::Keys, 3, "Piano"},
    {"pn", K::Instrument, F::Keys, 3, "Piano", kPreset},
    {"grand piano", K::Instrument, F::Keys, 4, "Grand Piano"},
    {"upright piano", K::Instrument, F::Keys, 4, "Upright Piano"},
    {"felt piano", K::Instrument, F::Keys, 4, "Felt Piano"},
    {"toy piano", K::Instrument, F::Keys, 4, "Toy Piano"},
    {"rhodes", K::Instrument, F::Keys, 4, "Rhodes"},
    {"wurli", K::Instrument, F::Keys, 4, "Wurlitzer"},
    {"wurlitzer", K::Instrument, F::Keys, 4, "Wurlitzer"},
    {"epiano", K::Instrument, F::Keys, 4, "E-Piano"},
    {"electric piano", K::Instrument, F::Keys, 4, "E-Piano"},
    {"ep", K::Instrument, F::Keys, 4, "E-Piano", kPreset},
    {"keys", K::Sound, F::Keys, 2, "Keys"},
    {"ky", K::Sound, F::Keys, 2, "Keys", kPreset},
    {"key", K::Sound, F::Keys, 2, "Keys", kPreset},
    {"organ", K::Instrument, F::Keys, 3, "Organ"},
    {"org", K::Instrument, F::Keys, 3, "Organ", kPreset},
    {"clav", K::Instrument, F::Keys, 3, "Clavinet"},
    {"clavinet", K::Instrument, F::Keys, 3, "Clavinet"},
    {"harpsichord", K::Instrument, F::Keys, 3, "Harpsichord"},
    {"accordion", K::Instrument, F::Keys, 3, "Accordion"},
    // Synths.
    {"synth", K::Sound, F::Synth, 1, "Synth"},
    {"synths", K::Sound, F::Synth, 1, "Synth"},
    {"syn", K::Sound, F::Synth, 1, "Synth"},
    {"sy", K::Sound, F::Synth, 1, "Synth", kPreset},
    {"lead", K::Sound, F::Synth, 2, "Lead"},
    {"leads", K::Sound, F::Synth, 2, "Lead"},
    {"ld", K::Sound, F::Synth, 2, "Lead", kPreset},
    {"pluck", K::Sound, F::Synth, 2, "Pluck"},
    {"plucks", K::Sound, F::Synth, 2, "Pluck"},
    {"plk", K::Sound, F::Synth, 2, "Pluck"},
    {"pl", K::Sound, F::Synth, 2, "Pluck", kPreset},
    {"pad", K::Sound, F::Synth, 2, "Pad"},
    {"pads", K::Sound, F::Synth, 2, "Pad"},
    {"pd", K::Sound, F::Synth, 2, "Pad", kPreset},
    {"stab", K::Sound, F::Synth, 2, "Stab"},
    {"stabs", K::Sound, F::Synth, 2, "Stab"},
    {"stb", K::Sound, F::Synth, 2, "Stab", kPreset},
    {"supersaw", K::Sound, F::Synth, 3, "Supersaw"},
    {"super saw", K::Sound, F::Synth, 3, "Supersaw"},
    {"hoover", K::Sound, F::Synth, 3, "Hoover"},
    {"drone", K::Sound, F::Ambience, 2, "Drone"},
    {"drones", K::Sound, F::Ambience, 2, "Drone"},
    {"arp", K::Pattern, F::None, 2, "Arp"},
    {"arps", K::Pattern, F::None, 2, "Arp"},
    {"arpeggio", K::Pattern, F::None, 2, "Arp"},
    {"arpeggiated", K::Pattern, F::None, 2, "Arp"},
    {"seq", K::Pattern, F::None, 1, "Sequence"},
    {"sequence", K::Pattern, F::None, 1, "Sequence"},
    {"sq", K::Pattern, F::None, 1, "Sequence", kPreset},
    {"chord", K::Pattern, F::None, 1, "Chords"},
    {"chords", K::Pattern, F::None, 1, "Chords"},
    {"chd", K::Pattern, F::None, 1, "Chords"},
    {"crd", K::Pattern, F::None, 1, "Chords"},
    {"ch", K::Pattern, F::None, 1, "Chords", kPreset},
    {"progression", K::Pattern, F::None, 1, "Chords"},
    // Strings.
    {"strings", K::Instrument, F::Strings, 2, "Strings"},
    {"string", K::Instrument, F::Strings, 2, "Strings"},
    {"str", K::Instrument, F::Strings, 2, "Strings"},
    {"violin", K::Instrument, F::Strings, 4, "Violins"},
    {"violins", K::Instrument, F::Strings, 4, "Violins"},
    {"viola", K::Instrument, F::Strings, 4, "Violas"},
    {"violas", K::Instrument, F::Strings, 4, "Violas"},
    {"cello", K::Instrument, F::Strings, 4, "Cellos"},
    {"cellos", K::Instrument, F::Strings, 4, "Cellos"},
    {"celli", K::Instrument, F::Strings, 4, "Cellos"},
    {"harp", K::Instrument, F::Strings, 4, "Harp"},
    {"pizzicato", K::Adjective, F::Strings, 0, "Pizzicato"},
    {"pizz", K::Adjective, F::Strings, 0, "Pizzicato"},
    {"staccato", K::Adjective, F::None, 0, "Staccato"},
    // Brass and woodwinds.
    {"brass", K::Instrument, F::Brass, 2, "Brass"},
    {"brs", K::Instrument, F::Brass, 2, "Brass"},
    {"br", K::Instrument, F::Brass, 2, "Brass", kPreset},
    {"trumpet", K::Instrument, F::Brass, 4, "Trumpet"},
    {"trumpets", K::Instrument, F::Brass, 4, "Trumpets"},
    {"horn", K::Instrument, F::Brass, 4, "Horn"},
    {"horns", K::Instrument, F::Brass, 4, "Horns"},
    {"french horn", K::Instrument, F::Brass, 4, "French Horn"},
    {"french horns", K::Instrument, F::Brass, 4, "French Horns"},
    {"trombone", K::Instrument, F::Brass, 4, "Trombone"},
    {"trombones", K::Instrument, F::Brass, 4, "Trombones"},
    {"tuba", K::Instrument, F::Brass, 4, "Tuba"},
    {"woodwinds", K::Instrument, F::Woodwinds, 2, "Woodwinds"},
    {"woodwind", K::Instrument, F::Woodwinds, 2, "Woodwinds"},
    {"flute", K::Instrument, F::Woodwinds, 4, "Flute"},
    {"flutes", K::Instrument, F::Woodwinds, 4, "Flutes"},
    {"pan flute", K::Instrument, F::Woodwinds, 4, "Pan Flute"},
    {"panflute", K::Instrument, F::Woodwinds, 4, "Pan Flute"},
    {"piccolo", K::Instrument, F::Woodwinds, 4, "Piccolo"},
    {"clarinet", K::Instrument, F::Woodwinds, 4, "Clarinet"},
    {"oboe", K::Instrument, F::Woodwinds, 4, "Oboe"},
    {"bassoon", K::Instrument, F::Woodwinds, 4, "Bassoon"},
    {"sax", K::Instrument, F::Woodwinds, 4, "Sax"},
    {"saxophone", K::Instrument, F::Woodwinds, 4, "Sax"},
    // Voices.
    {"vocal", K::Instrument, F::Vocals, 3, "Vocal"},
    {"vocals", K::Instrument, F::Vocals, 3, "Vocal"},
    {"vox", K::Instrument, F::Vocals, 3, "Vocal"},
    {"voc", K::Instrument, F::Vocals, 3, "Vocal"},
    {"vx", K::Instrument, F::Vocals, 3, "Vocal", kPreset},
    {"voice", K::Instrument, F::Vocals, 3, "Vocal"},
    {"voices", K::Instrument, F::Vocals, 3, "Vocal"},
    {"singer", K::Instrument, F::Vocals, 3, "Vocal"},
    {"acapella", K::Instrument, F::Vocals, 4, "Acapella"},
    {"acappella", K::Instrument, F::Vocals, 4, "Acapella"},
    {"accapella", K::Instrument, F::Vocals, 4, "Acapella"},
    {"a cappella", K::Instrument, F::Vocals, 4, "Acapella"},
    {"choir", K::Instrument, F::Vocals, 4, "Choir"},
    {"choirs", K::Instrument, F::Vocals, 4, "Choir"},
    {"adlib", K::Instrument, F::Vocals, 4, "Ad-Lib"},
    {"adlibs", K::Instrument, F::Vocals, 4, "Ad-Libs"},
    {"ad lib", K::Instrument, F::Vocals, 4, "Ad-Lib"},
    {"ad libs", K::Instrument, F::Vocals, 4, "Ad-Libs"},
    {"chant", K::Instrument, F::Vocals, 4, "Chant"},
    {"chants", K::Instrument, F::Vocals, 4, "Chant"},
    {"chop", K::Pattern, F::Vocals, 2, "Chop"},
    {"chops", K::Pattern, F::Vocals, 2, "Chops"},
    {"hook", K::Pattern, F::None, 1, "Hook"},
    {"verse", K::Pattern, F::None, 1, "Verse"},
    // Guitars.
    {"guitar", K::Instrument, F::Guitar, 3, "Guitar"},
    {"guitars", K::Instrument, F::Guitar, 3, "Guitar"},
    {"gtr", K::Instrument, F::Guitar, 3, "Guitar"},
    {"gt", K::Instrument, F::Guitar, 3, "Guitar", kPreset},
    {"acoustic guitar", K::Instrument, F::Guitar, 4, "Acoustic Guitar"},
    {"electric guitar", K::Instrument, F::Guitar, 4, "Electric Guitar"},
    {"ukulele", K::Instrument, F::Guitar, 4, "Ukulele"},
    {"uke", K::Instrument, F::Guitar, 4, "Ukulele"},
    {"banjo", K::Instrument, F::Guitar, 4, "Banjo"},
    {"mandolin", K::Instrument, F::Guitar, 4, "Mandolin"},
    {"sitar", K::Instrument, F::Guitar, 4, "Sitar"},
    {"riff", K::Pattern, F::None, 1, "Riff"},
    {"riffs", K::Pattern, F::None, 1, "Riff"},
    {"strum", K::Pattern, F::Guitar, 1, "Strum"},
    {"strums", K::Pattern, F::Guitar, 1, "Strum"},
    // Bells and mallets.
    {"bell", K::Instrument, F::Mallets, 3, "Bell"},
    {"bells", K::Instrument, F::Mallets, 3, "Bells"},
    {"bl", K::Instrument, F::Mallets, 3, "Bell", kPreset},
    {"chime", K::Instrument, F::Mallets, 3, "Chime"},
    {"chimes", K::Instrument, F::Mallets, 3, "Chimes"},
    {"kalimba", K::Instrument, F::Mallets, 4, "Kalimba"},
    {"marimba", K::Instrument, F::Mallets, 4, "Marimba"},
    {"vibraphone", K::Instrument, F::Mallets, 4, "Vibraphone"},
    {"vibes", K::Instrument, F::Mallets, 4, "Vibraphone"},
    {"glockenspiel", K::Instrument, F::Mallets, 4, "Glockenspiel"},
    {"glock", K::Instrument, F::Mallets, 4, "Glockenspiel"},
    {"xylophone", K::Instrument, F::Mallets, 4, "Xylophone"},
    {"celesta", K::Instrument, F::Mallets, 4, "Celesta"},
    {"music box", K::Instrument, F::Mallets, 4, "Music Box"},
    {"musicbox", K::Instrument, F::Mallets, 4, "Music Box"},
    {"steel drum", K::Instrument, F::Mallets, 4, "Steel Drum"},
    {"steel pan", K::Instrument, F::Mallets, 4, "Steel Drum"},
    {"handpan", K::Instrument, F::Mallets, 4, "Handpan"},
    {"mallet", K::Instrument, F::Mallets, 2, "Mallets"},
    {"mallets", K::Instrument, F::Mallets, 2, "Mallets"},
    // Effects and transitions.
    {"fx", K::Sound, F::Fx, 1, "FX"},
    {"sfx", K::Sound, F::Fx, 1, "FX"},
    {"riser", K::Sound, F::Fx, 3, "Riser"},
    {"risers", K::Sound, F::Fx, 3, "Riser"},
    {"uplifter", K::Sound, F::Fx, 3, "Riser"},
    {"upsweep", K::Sound, F::Fx, 3, "Riser"},
    {"sweep up", K::Sound, F::Fx, 3, "Riser"},
    {"buildup", K::Sound, F::Fx, 3, "Riser"},
    {"ri", K::Sound, F::Fx, 3, "Riser", kPreset},
    {"downlifter", K::Sound, F::Fx, 3, "Downlifter"},
    {"downsweep", K::Sound, F::Fx, 3, "Downlifter"},
    {"sweep down", K::Sound, F::Fx, 3, "Downlifter"},
    {"down sweep", K::Sound, F::Fx, 3, "Downlifter"},
    {"sweep", K::Sound, F::Fx, 2, "Sweep"},
    {"sweeps", K::Sound, F::Fx, 2, "Sweep"},
    {"impact", K::Sound, F::Fx, 3, "Impact"},
    {"impacts", K::Sound, F::Fx, 3, "Impact"},
    {"boom", K::Sound, F::Fx, 3, "Impact"},
    {"slam", K::Sound, F::Fx, 3, "Impact"},
    {"hit", K::Sound, F::Fx, 1, "Hit"},
    {"hits", K::Sound, F::Fx, 1, "Hit"},
    {"whoosh", K::Sound, F::Fx, 3, "Whoosh"},
    {"swoosh", K::Sound, F::Fx, 3, "Whoosh"},
    {"swish", K::Sound, F::Fx, 3, "Whoosh"},
    {"noise", K::Sound, F::Fx, 2, "Noise"},
    {"white noise", K::Sound, F::Fx, 3, "White Noise"},
    {"ns", K::Sound, F::Fx, 2, "Noise", kPreset},
    {"transition", K::Sound, F::Fx, 2, "Transition"},
    {"tape stop", K::Sound, F::Fx, 3, "Tape Stop"},
    {"sub drop", K::Sound, F::Fx, 4, "Sub Drop"},
    {"subdrop", K::Sound, F::Fx, 4, "Sub Drop"},
    {"zap", K::Sound, F::Fx, 3, "Zap"},
    {"laser", K::Sound, F::Fx, 3, "Zap"},
    {"air horn", K::Sound, F::Fx, 4, "Air Horn"},
    {"airhorn", K::Sound, F::Fx, 4, "Air Horn"},
    {"siren", K::Sound, F::Fx, 3, "Siren"},
    {"scratch", K::Sound, F::Fx, 3, "Scratch"},
    {"scratches", K::Sound, F::Fx, 3, "Scratch"},
    // Ambiences and textures.
    {"ambience", K::Sound, F::Ambience, 2, "Ambience"},
    {"atmos", K::Sound, F::Ambience, 2, "Atmosphere"},
    {"atmo", K::Sound, F::Ambience, 2, "Atmosphere"},
    {"atmosphere", K::Sound, F::Ambience, 2, "Atmosphere"},
    {"atmospheres", K::Sound, F::Ambience, 2, "Atmosphere"},
    {"atm", K::Sound, F::Ambience, 2, "Atmosphere", kPreset},
    {"at", K::Sound, F::Ambience, 2, "Atmosphere", kPreset},
    {"texture", K::Sound, F::Ambience, 2, "Texture"},
    {"textures", K::Sound, F::Ambience, 2, "Texture"},
    {"tx", K::Sound, F::Ambience, 2, "Texture", kPreset},
    {"field recording", K::Sound, F::Ambience, 3, "Field Recording"},
    {"foley", K::Sound, F::Ambience, 2, "Foley"},
    {"crowd", K::Sound, F::Ambience, 3, "Crowd"},
    {"applause", K::Sound, F::Ambience, 3, "Crowd"},
    {"cheering", K::Sound, F::Ambience, 3, "Crowd"},
    {"wind", K::Sound, F::Ambience, 3, "Wind"},
    {"rain", K::Sound, F::Ambience, 3, "Rain"},
    {"storm", K::Sound, F::Ambience, 3, "Storm"},
    {"thunder", K::Sound, F::Ambience, 3, "Thunder"},
    {"forest", K::Sound, F::Ambience, 3, "Forest"},
    {"ocean", K::Sound, F::Ambience, 3, "Ocean"},
    {"water", K::Sound, F::Ambience, 3, "Water"},
    {"birds", K::Sound, F::Ambience, 3, "Birds"},
    {"birdsong", K::Sound, F::Ambience, 3, "Birds"},
    {"room tone", K::Sound, F::Ambience, 3, "Room Tone"},
    {"traffic", K::Sound, F::Ambience, 3, "City"},
    {"vinyl crackle", K::Sound, F::Ambience, 3, "Vinyl Crackle"},
    {"crackle", K::Sound, F::Ambience, 3, "Vinyl Crackle"},
    {"full mix", K::Sound, F::Mix, 3, "Full Mix"},
    {"songstarter", K::Sound, F::Mix, 2, "Song Starter"},
    {"song starter", K::Sound, F::Mix, 2, "Song Starter"},
    {"fullmix", K::Sound, F::Mix, 3, "Full Mix"},
    {"mixdown", K::Sound, F::Mix, 3, "Full Mix"},
    // What is played.
    {"loop", K::Pattern, F::None, 1, "Loop"},
    {"loops", K::Pattern, F::None, 1, "Loop"},
    {"one shot", K::Pattern, F::None, 0, "One-Shot"},
    {"oneshot", K::Pattern, F::None, 0, "One-Shot"},
    {"line", K::Pattern, F::None, 1, "Line"},
    {"lines", K::Pattern, F::None, 1, "Line"},
    {"melody", K::Pattern, F::None, 1, "Melody"},
    {"melodies", K::Pattern, F::None, 1, "Melody"},
    {"phrase", K::Pattern, F::None, 0, "Phrase"},
    // How it sounds.
    {"soft", K::Adjective, F::None, 0, "Soft"},
    {"dark", K::Adjective, F::None, 0, "Dark"},
    {"bright", K::Adjective, F::None, 0, "Bright"},
    {"warm", K::Adjective, F::None, 0, "Warm"},
    {"big", K::Adjective, F::None, 0, "Big"},
    {"huge", K::Adjective, F::None, 0, "Huge"},
    {"giant", K::Adjective, F::None, 0, "Huge"},
    {"massive", K::Adjective, F::None, 0, "Massive"},
    {"fat", K::Adjective, F::None, 0, "Fat"},
    {"phat", K::Adjective, F::None, 0, "Fat"},
    {"wide", K::Adjective, F::None, 0, "Wide"},
    {"dirty", K::Adjective, F::None, 0, "Dirty"},
    {"clean", K::Adjective, F::None, 0, "Clean"},
    {"lush", K::Adjective, F::None, 0, "Lush"},
    {"airy", K::Adjective, F::None, 0, "Airy"},
    {"breathy", K::Adjective, F::None, 0, "Breathy"},
    {"sad", K::Adjective, F::None, 0, "Sad"},
    {"sadcore", K::Adjective, F::None, 0, "Sad"},
    {"happy", K::Adjective, F::None, 0, "Happy"},
    {"epic", K::Adjective, F::None, 0, "Epic"},
    {"cinematic", K::Adjective, F::None, 0, "Cinematic"},
    {"orchestral", K::Adjective, F::None, 0, "Orchestral"},
    {"orchestra", K::Adjective, F::None, 0, "Orchestral"},
    {"orch", K::Adjective, F::None, 0, "Orchestral"},
    {"detuned", K::Adjective, F::None, 0, "Detuned"},
    {"analog", K::Adjective, F::None, 0, "Analog"},
    {"analogue", K::Adjective, F::None, 0, "Analog"},
    {"vintage", K::Adjective, F::None, 0, "Vintage"},
    {"retro", K::Adjective, F::None, 0, "Retro"},
    {"lofi", K::Adjective, F::None, 0, "Lo-Fi"},
    {"lo fi", K::Adjective, F::None, 0, "Lo-Fi"},
    {"ambient", K::Adjective, F::None, 0, "Ambient"},
    {"evolving", K::Adjective, F::None, 0, "Evolving"},
    {"glassy", K::Adjective, F::None, 0, "Glassy"},
    {"metallic", K::Adjective, F::None, 0, "Metallic"},
    {"distorted", K::Adjective, F::None, 0, "Distorted"},
    {"distortion", K::Adjective, F::None, 0, "Distorted"},
    {"dist", K::Adjective, F::None, 0, "Distorted"},
    {"gritty", K::Adjective, F::None, 0, "Gritty"},
    {"crunchy", K::Adjective, F::None, 0, "Crunchy"},
    {"dusty", K::Adjective, F::None, 0, "Dusty"},
    {"smooth", K::Adjective, F::None, 0, "Smooth"},
    {"mellow", K::Adjective, F::None, 0, "Mellow"},
    {"deep", K::Adjective, F::None, 0, "Deep"},
    {"hard", K::Adjective, F::None, 0, "Hard"},
    {"punchy", K::Adjective, F::None, 0, "Punchy"},
    {"tight", K::Adjective, F::None, 0, "Tight"},
    {"boomy", K::Adjective, F::None, 0, "Boomy"},
    {"heavy", K::Adjective, F::None, 0, "Heavy"},
    {"quiet", K::Adjective, F::None, 0, "Quiet"},
    {"loud", K::Adjective, F::None, 0, "Loud"},
    {"dreamy", K::Adjective, F::None, 0, "Dreamy"},
    {"ethereal", K::Adjective, F::None, 0, "Ethereal"},
    {"haunting", K::Adjective, F::None, 0, "Haunting"},
    {"eerie", K::Adjective, F::None, 0, "Eerie"},
    {"creepy", K::Adjective, F::None, 0, "Creepy"},
    {"spooky", K::Adjective, F::None, 0, "Creepy"},
    {"chiptune", K::Adjective, F::None, 0, "Chiptune"},
    {"8bit", K::Adjective, F::None, 0, "Chiptune"},
    {"acoustic", K::Adjective, F::None, 0, "Acoustic"},
    {"electric", K::Adjective, F::None, 0, "Electric"},
    {"organic", K::Adjective, F::None, 0, "Organic"},
    {"hollow", K::Adjective, F::None, 0, "Hollow"},
    {"thin", K::Adjective, F::None, 0, "Thin"},
    {"sharp", K::Adjective, F::None, 0, "Sharp"},
    {"aggressive", K::Adjective, F::None, 0, "Aggressive"},
    {"angry", K::Adjective, F::None, 0, "Aggressive"},
    {"chill", K::Adjective, F::None, 0, "Chill"},
    {"emotional", K::Adjective, F::None, 0, "Emotional"},
    {"melancholic", K::Adjective, F::None, 0, "Melancholic"},
    {"melancholy", K::Adjective, F::None, 0, "Melancholic"},
    {"euphoric", K::Adjective, F::None, 0, "Euphoric"},
    {"uplifting", K::Adjective, F::None, 0, "Uplifting"},
    {"intense", K::Adjective, F::None, 0, "Intense"},
    {"melodic", K::Adjective, F::None, 0, "Melodic"},
    {"wobbly", K::Adjective, F::None, 0, "Wobbly"},
    {"bouncy", K::Adjective, F::None, 0, "Bouncy"},
    {"groovy", K::Adjective, F::None, 0, "Groovy"},
    {"funky", K::Adjective, F::None, 0, "Funky"},
    {"jazzy", K::Adjective, F::None, 0, "Jazzy"},
    {"reversed", K::Adjective, F::None, 0, "Reversed"},
    {"reverse", K::Adjective, F::None, 0, "Reversed"},
    {"washed", K::Adjective, F::None, 0, "Washed Out"},
    {"washed out", K::Adjective, F::None, 0, "Washed Out"},
    {"washedout", K::Adjective, F::None, 0, "Washed Out"},
    {"distant", K::Adjective, F::None, 0, "Distant"},
    {"shimmer", K::Adjective, F::None, 0, "Shimmering"},
    {"shimmering", K::Adjective, F::None, 0, "Shimmering"},
    {"gated", K::Adjective, F::None, 0, "Gated"},
    {"filtered", K::Adjective, F::None, 0, "Filtered"},
    {"pitched", K::Adjective, F::None, 0, "Pitched"},
    {"vocoded", K::Adjective, F::None, 0, "Vocoded"},
    {"spacey", K::Adjective, F::None, 0, "Spacey"},
    {"spacy", K::Adjective, F::None, 0, "Spacey"},
    {"glitch", K::Adjective, F::None, 0, "Glitchy"},
    {"glitchy", K::Adjective, F::None, 0, "Glitchy"},
    {"granular", K::Adjective, F::None, 0, "Granular"},
    {"saturated", K::Adjective, F::None, 0, "Saturated"},
    {"squashed", K::Adjective, F::None, 0, "Squashed"},
    {"pumping", K::Adjective, F::None, 0, "Pumping"},
    {"sidechained", K::Adjective, F::None, 0, "Pumping"},
    {"noisy", K::Adjective, F::None, 0, "Noisy"},
    {"harsh", K::Adjective, F::None, 0, "Harsh"},
    {"raspy", K::Adjective, F::None, 0, "Raspy"},
    {"fast", K::Adjective, F::None, 0, "Fast"},
    {"slow", K::Adjective, F::None, 0, "Slow"},
    {"saw", K::Adjective, F::None, 0, "Saw"},
    {"square", K::Adjective, F::None, 0, "Square"},
    {"sine", K::Adjective, F::None, 0, "Sine"},
    {"high", K::Adjective, F::None, 0, "High"},
    {"low", K::Adjective, F::None, 0, "Low"},
    // Styles.
    {"future bass", K::Genre, F::None, 0, "Future Bass"},
    {"dnb", K::Genre, F::None, 0, "DnB"},
    {"d b", K::Genre, F::None, 0, "DnB"},
    {"d n b", K::Genre, F::None, 0, "DnB"},
    {"drum and bass", K::Genre, F::None, 0, "DnB"},
    {"drum n bass", K::Genre, F::None, 0, "DnB"},
    {"drumnbass", K::Genre, F::None, 0, "DnB"},
    {"ldnb", K::Genre, F::None, 0, "Liquid DnB"},
    {"liquid", K::Genre, F::None, 0, "Liquid DnB"},
    {"neuro", K::Genre, F::None, 0, "Neurofunk"},
    {"jungle", K::Genre, F::None, 0, "Jungle"},
    {"bass house", K::Genre, F::None, 0, "Bass House"},
    {"bass music", K::Genre, F::None, 0, "Bass Music"},
    {"dubstep", K::Genre, F::None, 0, "Dubstep"},
    {"riddim", K::Genre, F::None, 0, "Riddim"},
    {"creepstep", K::Genre, F::None, 0, "Dubstep"},
    {"trap", K::Genre, F::None, 0, "Trap"},
    {"house", K::Genre, F::None, 0, "House"},
    {"techno", K::Genre, F::None, 0, "Techno"},
    {"trance", K::Genre, F::None, 0, "Trance"},
    {"edm", K::Genre, F::None, 0, "EDM"},
    {"hip hop", K::Genre, F::None, 0, "Hip-Hop"},
    {"hiphop", K::Genre, F::None, 0, "Hip-Hop"},
    {"rnb", K::Genre, F::None, 0, "R&B"},
    {"r b", K::Genre, F::None, 0, "R&B"},
    {"pop", K::Genre, F::None, 0, "Pop"},
    {"rock", K::Genre, F::None, 0, "Rock"},
    {"garage", K::Genre, F::None, 0, "Garage"},
    {"ukg", K::Genre, F::None, 0, "Garage"},
    {"synthwave", K::Genre, F::None, 0, "Synthwave"},
    {"phonk", K::Genre, F::None, 0, "Phonk"},
    {"drill", K::Genre, F::None, 0, "Drill"},
    {"afrobeat", K::Genre, F::None, 0, "Afrobeat"},
    {"afrobeats", K::Genre, F::None, 0, "Afrobeat"},
    {"reggaeton", K::Genre, F::None, 0, "Reggaeton"},
    {"disco", K::Genre, F::None, 0, "Disco"},
    {"funk", K::Genre, F::None, 0, "Funk"},
    {"jazz", K::Genre, F::None, 0, "Jazz"},
    {"hardstyle", K::Genre, F::None, 0, "Hardstyle"},
    {"hyperpop", K::Genre, F::None, 0, "Hyperpop"},
};

// Words that say nothing of the sound: a name's filler, a file's formats and
// sections, a sample library's product words.
const std::unordered_set<std::string_view> kNoise = {
    "the", "an", "and", "of", "in", "on", "to", "my", "for", "with", "by", "only", "from", "like", "sounds",
    "audio", "midi", "track", "group", "copy", "new", "final", "take", "main", "init", "default", "setting",
    "settings", "preset", "presets", "patch", "sample", "samples", "pack", "kit", "vol", "wav", "mp3", "aif",
    "aiff", "flac", "ogg", "bpm", "hz", "khz", "maj", "min", "major", "minor", "flat", "dry", "wet", "processed",
    "stem", "stems", "edit", "version", "mono", "stereo", "left", "right", "mix", "full", "part", "section",
    "bridge", "intro", "outro", "drop", "chorus", "bar", "bars", "beats", "core", "discover", "originals",
    "selection", "symphony", "bbc", "spitfire", "splice", "instrument", "instruments", "rack", "chain", "macro",
    "short", "long", "simple", "shot", "one", "random", "octave", "shift",
    // Sample labels and their artists, as their files are prefixed.
    "zenhiser", "kshmr", "cymatics", "vengeance", "loopmasters", "ghosthack", "samplephonics", "producerloops",
    "sonokinetic", "landr", "loopcloud", "touchloops", "syndicate",
};

struct Vocabulary {
    std::unordered_map<std::string_view, std::vector<const Entry*>> entries;

    Vocabulary() {
        for (const Entry& entry : kEntries) entries[entry.text].push_back(&entry);
    }

    const Entry* find(std::string_view text, uint8_t source) const {
        const auto it = entries.find(text);
        if (it == entries.end()) return nullptr;
        for (const Entry* entry : it->second) {
            if (entry->sources & source) return entry;
        }
        return nullptr;
    }
};

const Vocabulary& vocabulary() {
    static const Vocabulary instance;
    return instance;
}

bool isLetter(unsigned char c) { return std::isalpha(c) || c >= 0x80 || c == '#'; }
bool isDigit(unsigned char c) { return std::isdigit(c) != 0; }
bool isUpper(unsigned char c) { return std::isupper(c) != 0; }
bool isLower(unsigned char c) { return std::islower(c) || c == '#' || c >= 0x80; }

struct Token {
    std::string text;   // lower case
    bool capitals = false;  // written in capitals, two letters or more ("LDNB", "PL")
};

// A run of hex digits with both digits and letters, long enough to be a hash
// (a file named by its checksum says nothing).
bool isHash(std::string_view run) {
    if (run.size() < 12) return false;
    bool digit = false, letter = false;
    for (const char c : run) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        (std::isdigit(static_cast<unsigned char>(c)) ? digit : letter) = true;
    }
    return digit && letter;
}

std::vector<Token> tokenize(std::string_view text) {
    std::vector<Token> tokens;
    const auto add = [&](std::string_view piece) {
        if (piece.empty()) return;
        Token token{lowered(piece), false};
        int letters = 0;
        bool allUpper = true;
        for (const char c : piece) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                ++letters;
                allUpper = allUpper && isUpper(static_cast<unsigned char>(c));
            }
        }
        token.capitals = letters >= 2 && allUpper;
        tokens.push_back(std::move(token));
    };
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !isLetter(static_cast<unsigned char>(text[i])) &&
               !isDigit(static_cast<unsigned char>(text[i])))
            ++i;
        size_t end = i;
        while (end < text.size() &&
               (isLetter(static_cast<unsigned char>(text[end])) || isDigit(static_cast<unsigned char>(text[end]))))
            ++end;
        const std::string_view run = text.substr(i, end - i);
        if (!run.empty() && !isHash(run)) {
            // Split at camelCase ("BellStab"), before the last capital of a run
            // of them followed by a lower case letter ("XMLParser"), and between
            // letters and digits ("fill110", "808s").
            size_t start = 0;
            for (size_t k = 1; k < run.size(); ++k) {
                const auto a = static_cast<unsigned char>(run[k - 1]);
                const auto b = static_cast<unsigned char>(run[k]);
                const bool split = (isLower(a) && a != '#' && isUpper(b)) ||
                                   (isUpper(a) && isUpper(b) && k + 1 < run.size() &&
                                    std::islower(static_cast<unsigned char>(run[k + 1]))) ||
                                   (isDigit(a) != isDigit(b));
                if (split) {
                    add(run.substr(start, k - start));
                    start = k;
                }
            }
            add(run.substr(start));
        }
        i = end;
    }
    return tokens;
}

bool allDigits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return isDigit(c); });
}

// A key: "c", "a#min", "fm", "bb", "ebmaj", "gsharp".
bool isKey(std::string_view t) {
    if (t.empty() || t[0] < 'a' || t[0] > 'g') return false;
    std::string_view rest = t.substr(1);
    for (std::string_view accidental : {"sharp", "flat", "#", "b", "s"}) {
        if (rest.substr(0, accidental.size()) == accidental) {
            rest.remove_prefix(accidental.size());
            break;
        }
    }
    for (std::string_view quality : {"", "m", "min", "minor", "maj", "major", "dim"}) {
        if (rest == quality) return true;
    }
    return false;
}

// `word` split into two or three words the vocabulary knows, each at least
// three letters ("bassline": bass, line); empty if it can't be.
std::vector<const Entry*> segment(std::string_view word, uint8_t source, int parts = 3) {
    if (parts == 0 || word.size() < 3) return {};
    if (const Entry* whole = vocabulary().find(word, source); whole && parts < 3) return {whole};
    for (size_t cut = 3; cut + 3 <= word.size(); ++cut) {
        const Entry* head = vocabulary().find(word.substr(0, cut), source);
        if (!head) continue;
        std::vector<const Entry*> rest = segment(word.substr(cut), source, parts - 1);
        if (rest.empty()) continue;
        rest.insert(rest.begin(), head);
        return rest;
    }
    return {};
}

Word toWord(const Entry& entry) {
    return {std::string(entry.display), entry.kind, entry.family, entry.rank};
}

}  // namespace

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string squeezed(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::vector<std::string> splitWords(std::string_view text) {
    std::vector<std::string> words;
    for (Token& token : tokenize(text)) words.push_back(std::move(token.text));
    return words;
}

Reading readWords(std::string_view text, Source source) {
    const uint8_t where = bit(source);
    const std::vector<Token> tokens = tokenize(text);
    Reading reading;
    size_t i = 0;
    while (i < tokens.size()) {
        // The longest phrase the vocabulary knows from here.
        const Entry* found = nullptr;
        size_t length = 0;
        for (size_t n = std::min<size_t>(3, tokens.size() - i); n >= 1 && !found; --n) {
            std::string phrase = tokens[i].text;
            for (size_t k = 1; k < n; ++k) phrase += ' ' + tokens[i + k].text;
            if ((found = vocabulary().find(phrase, where))) length = n;
        }
        if (found) {
            reading.words.push_back(toWord(*found));
            i += length;
            continue;
        }
        const std::string& t = tokens[i].text;
        ++i;
        if (t.size() < 2 || allDigits(t) || isKey(t) || kNoise.contains(t)) continue;
        // A plural it knows the singular of ("risers"), a word glued from others ("bassline").
        if (t.size() > 3 && t.back() == 's') {
            if (const Entry* singular = vocabulary().find(std::string_view(t).substr(0, t.size() - 1), where)) {
                reading.words.push_back(toWord(*singular));
                continue;
            }
        }
        if (t.size() >= 6) {
            if (const std::vector<const Entry*> parts = segment(t, where); !parts.empty()) {
                for (const Entry* part : parts) reading.words.push_back(toWord(*part));
                continue;
            }
        }
        // Capitals are a sample pack's code or name ("OS_", "VRB2_", "DECODEDDRUMBASS");
        // other words it doesn't know are kept (names of things: "Missme", "Dynasty").
        const bool letters = std::all_of(t.begin(), t.end(), [](char c) { return isLetter(c); });
        if (tokens[i - 1].capitals) continue;
        if (letters && t.size() >= 3) reading.unknown.push_back(t);
    }
    return reading;
}

std::string familyId(Family family) {
    switch (family) {
    case Family::None: return {};
    case Family::Drums: return "drums";
    case Family::Bass: return "bass";
    case Family::Keys: return "keys";
    case Family::Synth: return "synth";
    case Family::Strings: return "strings";
    case Family::Brass: return "brass";
    case Family::Woodwinds: return "woodwinds";
    case Family::Vocals: return "vocals";
    case Family::Guitar: return "guitar";
    case Family::Mallets: return "mallets";
    case Family::Fx: return "fx";
    case Family::Ambience: return "ambience";
    case Family::Mix: return "mix";
    }
    return {};
}

std::string familyTitle(Family family) {
    switch (family) {
    case Family::None: return {};
    case Family::Drums: return "Drum";
    case Family::Bass: return "Bass";
    case Family::Keys: return "Keys";
    case Family::Synth: return "Synth";
    case Family::Strings: return "String";
    case Family::Brass: return "Brass";
    case Family::Woodwinds: return "Woodwind";
    case Family::Vocals: return "Vocal";
    case Family::Guitar: return "Guitar";
    case Family::Mallets: return "Bell";
    case Family::Fx: return "FX";
    case Family::Ambience: return "Ambience";
    case Family::Mix: return "Mix";
    }
    return {};
}

std::string titleCase(std::string_view text) {
    std::string out(text);
    bool start = true;
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (start && std::isalpha(u)) c = static_cast<char>(std::toupper(u));
        start = !(std::isalnum(u) || u >= 0x80 || c == '\'');
    }
    return out;
}

}  // namespace sub::intelligence::labels
