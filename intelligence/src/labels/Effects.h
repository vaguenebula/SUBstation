#pragma once
// What a track's devices do to its sound.
//
// classify() says what a device is: a built-in one by its kind; a plug-in by
// what a few well-known ones are (Trackspacer ducks, soothe2 tames resonances,
// Crystalline is a reverb), then by the words of its name ("verb": a reverb,
// "dist": a distortion, "delay" or "echo": a delay, "comp": a compressor...),
// then by its VST3 category ("Fx|Reverb"). readDevice() then reads how much it
// does, from its parameters where they are known (a reverb's mix, a delay's
// feedback, an OTT's depth, an EQ's cuts, a utility's width), and says what
// that does to the sound: traits ("Washed Out", "Echoing", "Squashed", "Wide"),
// each with how much it says.

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "labels/Facts.h"

namespace sub::intelligence::labels {

enum class Effect {
    None,  // nothing known of it (or a rack)
    Instrument,
    Reverb,
    Delay,
    Distortion,
    Saturation,
    LoFi,
    Clipper,
    Compressor,
    Ott,  // upward and downward multiband compression, squashing (Xfer's OTT, Over The Top)
    Limiter,
    Ducking,  // a sidechain's pumping, a volume shaper, Trackspacer
    Gate,
    Transient,
    DeEsser,
    Rider,
    Eq,
    Filter,
    Resonance,  // resonance suppression (soothe2)
    Exciter,
    Chorus,
    Flanger,
    Phaser,
    Tremolo,
    Modulation,
    Width,
    PitchCorrection,
    PitchShift,
    Vocoder,
    Glitch,
    Granular,
    Amp,
    Utility,
    Analyzer,
    Restoration,
    Mastering,
};

Effect classify(const DeviceFact& device);
// "reverb", "pitch correction"...
std::string effectName(Effect effect);

// Something a device does to the sound.
struct Trait {
    std::string word;   // "Washed Out"
    double weight = 0.0;  // how much it says of the sound, 0..1
    // Traits of one group say the same thing: a label takes one of each
    // ("space": Washed Out, Spacious, Roomy; "echo"; "drive"; "dynamics"...).
    std::string group;
};

struct DeviceReading {
    Effect effect = Effect::None;
    std::vector<Trait> traits;
    // What it does, for the details: "ValhallaVintageVerb: reverb, 52% wet" ("" if nothing to say).
    std::string summary;
};

// A device's effect on the sound (an instrument's, a rack's: nothing; a device
// switched off: nothing but its summary).
DeviceReading readDevice(const DeviceFact& device);

// The first parameter whose name, lower case without spaces or punctuation, is
// one of `names` ("dry/wet" is "drywet"); null if none.
const ParamFact* findParam(const DeviceFact& device, std::initializer_list<std::string_view> names);
// A built-in device's parameter by id, in its units; none if it has none.
std::optional<double> builtinParam(const DeviceFact& device, std::string_view id);
// Whether readDevice() reads a plug-in's parameter of this name: the
// application asks plug-ins for only those (a plug-in may have thousands).
bool readsParam(std::string_view name);
// What a plug-in's saved state says of the parameters readDevice() reads, for
// one that isn't loaded (frozen, missing, still loading): many keep their values
// as text near its start (Valhalla's Mix="0.5", JUCE's <PARAM id="mix"
// value="0.5"/>). Only values 0..1 are taken (the scale of others is unknown),
// the first of each name.
std::vector<ParamFact> paramsInState(std::string_view state);
// A parameter as a share of its range, 0..1: from its text or unit if in percent
// ("35.0 %"), from decibels if in them (-6 dB: 0.5), else its normalized value.
double fraction(const ParamFact& param);

}  // namespace sub::intelligence::labels
