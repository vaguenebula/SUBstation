#include "labels/Effects.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "labels/Words.h"

namespace sub::intelligence::labels {

namespace {

using E = Effect;

// A rule for plug-ins' names: `needle` in the name (lower case), or a whole word
// of it if `word`. The first rule that matches says what the plug-in is.
struct NameRule {
    std::string_view needle;
    Effect effect;
    bool word = false;
};

const NameRule kNameRules[] = {
    // Plug-ins whose names would mislead the rules after them, or say nothing.
    {"de-verb", E::Restoration},
    {"deverb", E::Restoration},
    {"dereverb", E::Restoration},
    {"trackspacer", E::Ducking},
    {"kickstart", E::Ducking},
    {"lfotool", E::Ducking},
    {"pumper", E::Ducking},
    {"volumeshaper", E::Ducking},
    {"shaperbox", E::Ducking},
    {"sidechain", E::Ducking},
    {"duck", E::Ducking},
    {"soothe", E::Resonance},
    {"smooth operator", E::Resonance},
    {"gullfoss", E::Eq},
    {"crystalline", E::Reverb},
    {"supermassive", E::Reverb},
    {"seventh heaven", E::Reverb},
    {"blackhole", E::Reverb},
    {"shimmer", E::Reverb},
    {"convolver", E::Reverb},
    {"pro-r", E::Reverb},
    {"xvox space", E::Reverb},
    {"xvox tone", E::Eq},
    {"xvox ds", E::DeEsser},
    {"ott", E::Ott, true},
    {"over the top", E::Ott},
    {"tape stop", E::Glitch},
    {"trance gate", E::Tremolo},
    {"loudness war", E::Distortion},
    {"manipulator", E::PitchShift},
    {"ubermod", E::Modulation},
    {"spacemodulator", E::Flanger},
    {"freqecho", E::Delay},
    {"timeless", E::Delay},
    {"echoboy", E::Delay},
    {"deelay", E::Delay},
    {"saturn", E::Saturation},
    {"decapitator", E::Saturation},
    {"heatwave", E::Saturation},
    {"faturator", E::Distortion},
    {"trash", E::Distortion},
    {"scream", E::Distortion},
    {"ruina", E::Distortion},
    {"punish", E::Distortion},
    {"rc-20", E::LoFi},
    {"retro color", E::LoFi},
    {"vinyl", E::LoFi},
    {"sp950", E::LoFi},
    {"sa2rate", E::LoFi},
    {"retrocraft", E::LoFi},
    {"volcano", E::Filter},
    {"simplon", E::Filter},
    {"resonator", E::Filter},
    {"pro-q", E::Eq},
    {"pro-c", E::Compressor},
    {"pro-mb", E::Compressor},
    {"pro-l", E::Limiter},
    {"pro-ds", E::DeEsser},
    {"pro-g", E::Gate},
    {"maximizer", E::Limiter},
    {"imager", E::Width},
    {"wider", E::Width},
    {"microshift", E::Width},
    {"haas", E::Width},
    {"doubler", E::Width},
    {"fresh air", E::Exciter},
    {"enhancer", E::Exciter},
    {"exciter", E::Exciter},
    {"auto-tune", E::PitchCorrection},
    {"autotune", E::PitchCorrection},
    {"mautopitch", E::PitchCorrection},
    {"melodyne", E::PitchCorrection},
    {"graillon", E::PitchCorrection},
    {"xpitch", E::PitchCorrection},
    {"auto-key", E::Analyzer},
    {"rider", E::Rider},
    {"vocoder", E::Vocoder},
    {"vocalsynth", E::Vocoder},
    {"stutter", E::Glitch},
    {"glitch", E::Glitch},
    {"buffr", E::Glitch},
    {"reverser", E::Glitch},
    {"rhythmizer", E::Glitch},
    {"granular", E::Granular},
    {"paulxstretch", E::Granular},
    {"portal", E::Granular},
    {"relay", E::Utility},
    {"guitar rig", E::Amp},
    {"amplitube", E::Amp},
    // What the words of a name say: "verb" is a reverb, "dist" a distortion...
    {"verb", E::Reverb},
    {"plate", E::Reverb},
    {"hall", E::Reverb, true},
    {"room", E::Reverb, true},
    {"spring", E::Reverb, true},
    {"dist", E::Distortion},
    {"drive", E::Distortion},
    {"fuzz", E::Distortion},
    {"crush", E::LoFi},
    {"lofi", E::LoFi},
    {"lo-fi", E::LoFi},
    {"decimat", E::LoFi},
    {"satur", E::Saturation},
    {"tape", E::Saturation},
    {"tube", E::Saturation},
    {"clip", E::Clipper},
    {"delay", E::Delay},
    {"echo", E::Delay},
    {"comp", E::Compressor},
    {"squash", E::Compressor},
    {"dynamics", E::Compressor},
    {"limit", E::Limiter},
    {"gate", E::Gate},
    {"transient", E::Transient},
    {"de-ess", E::DeEsser},
    {"deess", E::DeEsser},
    {"chorus", E::Chorus},
    {"ensemble", E::Chorus},
    {"dimension", E::Chorus},
    {"flang", E::Flanger},
    {"phase", E::Phaser},
    {"tremolo", E::Tremolo},
    {"vibrato", E::Modulation},
    {"rotary", E::Modulation},
    {"ring mod", E::Modulation},
    {"filter", E::Filter},
    {"eq", E::Eq, true},
    {"equaliz", E::Eq},
    {"pitch", E::PitchShift},
    {"tune", E::PitchCorrection, true},
    {"stereo", E::Width},
    {"width", E::Width},
    {"wide", E::Width},
    {"amp", E::Amp, true},
    {"meter", E::Analyzer},
    {"analyz", E::Analyzer},
    {"spectrum", E::Analyzer},
    {"oscilloscope", E::Analyzer},
    {"tuner", E::Analyzer},
    {"loudness", E::Analyzer},
    {"gain", E::Utility, true},
    {"utility", E::Utility},
    {"channel mixer", E::Utility},
    {"ozone", E::Mastering},
    {"master", E::Mastering},
};

Effect fromName(const std::string& name) {
    const std::string lower = lowered(name);
    const std::vector<std::string> words = splitWords(name);
    for (const NameRule& rule : kNameRules) {
        const bool found = rule.word ? std::find(words.begin(), words.end(), rule.needle) != words.end()
                                     : lower.find(rule.needle) != std::string::npos;
        if (found) return rule.effect;
    }
    return E::None;
}

// The first of a VST3 category's sub-categories that says what an effect is.
Effect fromCategory(const std::string& category) {
    const std::map<std::string, Effect, std::less<>> kinds = {
        {"reverb", E::Reverb},       {"delay", E::Delay},        {"distortion", E::Distortion},
        {"dynamics", E::Compressor}, {"eq", E::Eq},              {"filter", E::Filter},
        {"modulation", E::Modulation}, {"pitch shift", E::PitchShift}, {"spatial", E::Width},
        {"stereo", E::Width},        {"restoration", E::Restoration}, {"analyzer", E::Analyzer},
        {"tools", E::Utility},       {"mastering", E::Mastering},
    };
    size_t start = 0;
    while (start <= category.size()) {
        size_t end = category.find('|', start);
        if (end == std::string::npos) end = category.size();
        if (const auto it = kinds.find(lowered(category.substr(start, end - start))); it != kinds.end()) return it->second;
        start = end + 1;
    }
    return E::None;
}

// The first number in a text ("35.0 %" -> 35, "4.0:1" -> 4); none if there is none.
std::optional<double> leadingNumber(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            ((c == '-' || c == '.') && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
            char* end = nullptr;
            const double value = std::strtod(text.c_str() + i, &end);
            if (end != text.c_str() + i) return value;
        }
    }
    return std::nullopt;
}

double fromDecibels(double db) { return db <= -70.0 ? 0.0 : std::min(1.0, std::pow(10.0, db / 20.0)); }

std::string percent(double share) { return std::to_string(static_cast<int>(std::lround(share * 100.0))) + "%"; }

std::string hertz(double hz) {
    char text[32];
    if (hz < 1000.0) {
        std::snprintf(text, sizeof text, "%.0f Hz", hz);
    } else {
        std::snprintf(text, sizeof text, "%.1f kHz", hz / 1000.0);
    }
    return text;
}

// The parameters readDevice() reads, by their squeezed names.
constexpr std::initializer_list<std::string_view> kWetNames = {
    "drywet", "wetdry", "mix", "wet", "blend", "wetmix", "drywetmix", "mixamount", "globalmix", "mastermix", "fxmix"};
constexpr std::initializer_list<std::string_view> kFeedbackNames = {"feedback", "fb", "feedbackamount", "regen",
                                                                    "regeneration"};
constexpr std::initializer_list<std::string_view> kDriveNames = {"drive", "distortion", "amount", "saturation",
                                                                 "drivegain"};
constexpr std::initializer_list<std::string_view> kOtherNames = {"pingpong", "ratio", "depth", "soundgoodize"};

std::optional<double> share(const DeviceFact& device, std::initializer_list<std::string_view> names) {
    const ParamFact* param = findParam(device, names);
    return param ? std::optional<double>(fraction(*param)) : std::nullopt;
}

std::optional<double> wetShare(const DeviceFact& device) {
    return share(device, kWetNames);
}


// What a built-in EQ's bands cut and lift: the highest low cut, the lowest high
// cut (a band pass is both), a high shelf's gain.
void readEq(const DeviceFact& device, DeviceReading& reading) {
    enum { Bell, LowShelf, LowCut, HighShelf, HighCut, Notch, BandPass, TiltShelf };
    double lowCut = 0.0, highCut = 1e9, shelf = 0.0;
    for (int band = 1;; ++band) {
        const std::string id = "b" + std::to_string(band) + "_";
        const auto type = builtinParam(device, id + "type");
        if (!type) break;
        if (builtinParam(device, id + "used").value_or(0.0) < 0.5 || builtinParam(device, id + "on").value_or(1.0) < 0.5) continue;
        const double freq = builtinParam(device, id + "freq").value_or(1000.0);
        const auto kind = static_cast<int>(std::lround(*type));
        if (kind == LowCut || kind == BandPass) lowCut = std::max(lowCut, freq);
        if (kind == HighCut || kind == BandPass) highCut = std::min(highCut, freq);
        if (kind == HighShelf) shelf += builtinParam(device, id + "gain").value_or(0.0);
    }
    std::string what;
    if (lowCut > 0.0) what += ", low cut " + hertz(lowCut);
    if (highCut < 1e9) what += ", high cut " + hertz(highCut);
    reading.summary += what;
    if (lowCut >= 300.0 && highCut <= 4000.0) {
        reading.traits.push_back({"Telephone", 0.7, "tone"});
        return;
    }
    if (highCut <= 1500.0) {
        reading.traits.push_back({"Muffled", 0.7, "tone"});
    } else if (highCut <= 4000.0) {
        reading.traits.push_back({"Dark", 0.5, "tone"});
    } else if (shelf >= 4.0) {
        reading.traits.push_back({"Bright", 0.4, "tone"});
    } else if (shelf <= -6.0) {
        reading.traits.push_back({"Dark", 0.4, "tone"});
    }
    if (lowCut >= 800.0) {
        reading.traits.push_back({"Thin", 0.55, "body"});
    } else if (lowCut >= 400.0) {
        reading.traits.push_back({"Thin", 0.4, "body"});
    }
}

// A built-in synth's filter.
void readSynth(const DeviceFact& device, DeviceReading& reading) {
    if (const auto cutoff = builtinParam(device, "cutoff")) {
        if (*cutoff <= 400.0) {
            reading.traits.push_back({"Dark", 0.5, "tone"});
        } else if (*cutoff <= 1200.0) {
            reading.traits.push_back({"Mellow", 0.3, "tone"});
        }
    }
    if (builtinParam(device, "resonance").value_or(0.0) >= 60.0) reading.traits.push_back({"Resonant", 0.4, "resonance"});
}

}  // namespace

bool readsParam(std::string_view name) {
    const std::string key = squeezed(name);
    for (const auto& names : {kWetNames, kFeedbackNames, kDriveNames, kOtherNames}) {
        if (std::find(names.begin(), names.end(), key) != names.end()) return true;
    }
    return false;
}

std::vector<ParamFact> paramsInState(std::string_view state) {
    constexpr size_t kScanLimit = 64 * 1024;  // (as presets' names: a header)
    state = state.substr(0, kScanLimit);
    std::vector<ParamFact> found;
    const auto take = [&](std::string_view name, std::string_view text) {
        if (name.empty() || !readsParam(name)) return;
        const std::string key = squeezed(name);
        for (const ParamFact& param : found) {
            if (squeezed(param.name) == key) return;
        }
        char* end = nullptr;
        const std::string number(text);
        const double value = std::strtod(number.c_str(), &end);
        if (end == number.c_str() || *end != '\0' || !(value >= 0.0 && value <= 1.0)) return;
        found.push_back({key, std::string(name), value, value, {}, {}});
    };
    // name="value" anywhere; <PARAM id="name" value="value"/> as JUCE writes them.
    for (size_t at = state.find("=\""); at != std::string_view::npos; at = state.find("=\"", at + 2)) {
        size_t start = at;
        while (start > 0 && (std::isalnum(static_cast<unsigned char>(state[start - 1])) || state[start - 1] == '_')) {
            --start;
        }
        const size_t close = state.find('"', at + 2);
        if (close == std::string_view::npos || close - at > 40) continue;
        const std::string_view name = state.substr(start, at - start);
        const std::string_view value = state.substr(at + 2, close - at - 2);
        if (name == "id") {
            const size_t valueAt = state.find("value=\"", close);
            const size_t valueEnd = valueAt == std::string_view::npos ? valueAt : state.find('"', valueAt + 7);
            if (valueEnd != std::string_view::npos && valueAt - close < 8) {
                take(value, state.substr(valueAt + 7, valueEnd - valueAt - 7));
            }
        } else {
            take(name, value);
        }
    }
    return found;
}

std::optional<double> builtinParam(const DeviceFact& device, std::string_view id) {
    for (const ParamFact& param : device.params) {
        if (param.id == id) return param.value;
    }
    return std::nullopt;
}

const ParamFact* findParam(const DeviceFact& device, std::initializer_list<std::string_view> names) {
    for (const std::string_view name : names) {
        for (const ParamFact& param : device.params) {
            if (squeezed(param.name) == name || param.id == name) return &param;
        }
    }
    return nullptr;
}

double fraction(const ParamFact& param) {
    double share = param.normalized;
    const auto number = leadingNumber(param.text);
    if (number && param.text.find('%') != std::string::npos) {
        share = *number / 100.0;
    } else if (number && lowered(param.text).find("db") != std::string::npos) {
        share = fromDecibels(*number);
    } else if (param.unit == "%") {
        share = param.value / 100.0;
    } else if (lowered(param.unit) == "db") {
        share = fromDecibels(param.value);
    }
    return std::clamp(share, 0.0, 1.0);
}

Effect classify(const DeviceFact& device) {
    static const std::map<std::string, Effect, std::less<>> kBuiltins = {
        {"synth", E::Instrument},     {"sampler", E::Instrument}, {"compressor", E::Compressor},
        {"delay", E::Delay},          {"eq", E::Eq},              {"ott", E::Ott},
        {"sidechain", E::Ducking},    {"utility", E::Utility},
    };
    if (device.kind == "rack") return E::None;
    if (device.kind != "plugin") {
        const auto it = kBuiltins.find(device.kind);
        if (it != kBuiltins.end()) return it->second;
        return device.instrument ? E::Instrument : E::None;
    }
    if (device.instrument) return E::Instrument;
    if (const Effect effect = fromName(device.name); effect != E::None) return effect;
    return fromCategory(device.category);
}

std::string effectName(Effect effect) {
    switch (effect) {
    case E::None: return {};
    case E::Instrument: return "instrument";
    case E::Reverb: return "reverb";
    case E::Delay: return "delay";
    case E::Distortion: return "distortion";
    case E::Saturation: return "saturation";
    case E::LoFi: return "lo-fi";
    case E::Clipper: return "clipper";
    case E::Compressor: return "compressor";
    case E::Ott: return "multiband compression";
    case E::Limiter: return "limiter";
    case E::Ducking: return "ducking";
    case E::Gate: return "gate";
    case E::Transient: return "transient shaper";
    case E::DeEsser: return "de-esser";
    case E::Rider: return "vocal rider";
    case E::Eq: return "EQ";
    case E::Filter: return "filter";
    case E::Resonance: return "resonance suppressor";
    case E::Exciter: return "exciter";
    case E::Chorus: return "chorus";
    case E::Flanger: return "flanger";
    case E::Phaser: return "phaser";
    case E::Tremolo: return "tremolo";
    case E::Modulation: return "modulation";
    case E::Width: return "stereo width";
    case E::PitchCorrection: return "pitch correction";
    case E::PitchShift: return "pitch shifter";
    case E::Vocoder: return "vocoder";
    case E::Glitch: return "glitch";
    case E::Granular: return "granular";
    case E::Amp: return "amp";
    case E::Utility: return "utility";
    case E::Analyzer: return "analyzer";
    case E::Restoration: return "restoration";
    case E::Mastering: return "mastering";
    }
    return {};
}

DeviceReading readDevice(const DeviceFact& device) {
    DeviceReading reading;
    reading.effect = classify(device);
    if (reading.effect == E::None || reading.effect == E::Instrument) {
        if (device.kind == "synth" && device.enabled) readSynth(device, reading);
        return reading;
    }
    const std::string name = device.name.empty() ? titleCase(device.kind) : device.name;
    const std::string what = effectName(reading.effect);
    reading.summary = lowered(name) == lowered(what) ? name : name + ": " + what;
    if (!device.enabled) {
        reading.summary += " (off)";
        return reading;
    }
    const auto add = [&](std::string word, double weight, std::string group) {
        if (weight > 0.0) reading.traits.push_back({std::move(word), weight, std::move(group)});
    };
    const auto wet = wetShare(device);
    if (wet && (reading.effect == E::Reverb || reading.effect == E::Delay || reading.effect == E::Distortion ||
                reading.effect == E::Saturation || reading.effect == E::Chorus || reading.effect == E::LoFi)) {
        reading.summary += ", " + percent(*wet) + " wet";
        if (*wet < 0.03) return reading;  // (as good as off)
    }
    switch (reading.effect) {
    case E::Reverb:
        if (!wet) {
            add("Spacious", 0.45, "space");
        } else if (*wet >= 0.45) {
            add("Washed Out", 0.9, "space");
        } else if (*wet >= 0.25) {
            add("Spacious", 0.65, "space");
        } else if (*wet >= 0.1) {
            add("Roomy", 0.35, "space");
        }
        if (lowered(device.name).find("shimmer") != std::string::npos) add("Shimmering", 0.55, "shimmer");
        break;
    case E::Delay: {
        const auto feedback = share(device, kFeedbackNames);
        if (feedback) reading.summary += ", " + percent(*feedback) + " feedback";
        if (const ParamFact* pingPong = findParam(device, {"pingpong"}); pingPong && pingPong->normalized >= 0.5) {
            reading.summary += ", ping-pong";
        }
        double weight = 0.45;
        if (wet) weight += *wet >= 0.35 ? 0.25 : *wet >= 0.2 ? 0.1 : 0.0;
        if (feedback && *feedback >= 0.7) weight += 0.1;
        add("Echoing", weight, "echo");
        break;
    }
    case E::Distortion: {
        const auto drive = share(device, kDriveNames);
        add("Distorted", drive ? 0.55 + 0.25 * *drive : 0.65, "drive");
        break;
    }
    case E::Saturation: {
        const auto drive = share(device, {"drive", "saturation", "amount"});
        if (drive && *drive >= 0.7) {
            add("Distorted", 0.6, "drive");
        } else {
            add("Saturated", 0.5, "drive");
        }
        break;
    }
    case E::LoFi: add("Lo-Fi", 0.75, "lofi"); break;
    case E::Clipper: add("Clipped", 0.2, "dynamics"); break;
    case E::Compressor: {
        std::optional<double> ratio;
        if (const ParamFact* param = findParam(device, {"ratio"})) {
            ratio = leadingNumber(param->text);
            if (!ratio && param->unit == ":1") ratio = param->value;
        }
        if (ratio) reading.summary += ", ratio " + std::to_string(static_cast<int>(std::lround(*ratio))) + ":1";
        add(ratio && *ratio >= 8.0 ? "Squashed" : "Compressed", ratio && *ratio >= 8.0 ? 0.55 : 0.15, "dynamics");
        break;
    }
    case E::Ott: {
        const auto depth = share(device, {"depth", "soundgoodize", "amount"});
        if (depth) reading.summary += ", " + percent(*depth) + " depth";
        add("Squashed", !depth ? 0.55 : *depth >= 0.6 ? 0.75 : *depth >= 0.3 ? 0.5 : *depth >= 0.1 ? 0.25 : 0.0,
            "dynamics");
        break;
    }
    case E::Limiter: add("Limited", 0.15, "dynamics"); break;
    case E::Ducking: {
        if (lowered(device.name).find("trackspacer") != std::string::npos) {
            add("Ducked", 0.4, "pump");
            break;
        }
        const auto depth = device.kind == "sidechain" ? share(device, {"depth"}) : std::nullopt;
        if (depth) reading.summary += ", " + percent(*depth) + " depth";
        add("Pumping", !depth ? 0.6 : *depth >= 0.4 ? 0.7 : 0.45, "pump");
        break;
    }
    case E::Gate: add("Gated", 0.45, "gate"); break;
    case E::Transient: add("Punchy", 0.3, "punch"); break;
    case E::Utility:
        if (const auto width = builtinParam(device, "width")) {
            reading.summary += ", width " + std::to_string(static_cast<int>(std::lround(*width))) + "%";
            if (*width >= 140.0) {
                add("Wide", 0.6, "width");
            } else if (*width <= 25.0) {
                add("Mono", *width <= 1.0 ? 0.6 : 0.5, "width");
            }
        }
        break;
    case E::Eq:
        if (device.kind == "eq") readEq(device, reading);
        break;
    case E::Filter: add("Filtered", 0.4, "filter"); break;
    case E::Resonance: add("Smooth", 0.2, "tone"); break;
    case E::Exciter: add("Bright", 0.35, "tone"); break;
    case E::Chorus: add("Chorused", 0.5, "modulation"); break;
    case E::Flanger: add("Flanged", 0.55, "modulation"); break;
    case E::Phaser: add("Phased", 0.55, "modulation"); break;
    case E::Tremolo: add("Pulsing", 0.45, "modulation"); break;
    case E::Modulation: add("Modulated", 0.35, "modulation"); break;
    case E::Width: add("Wide", 0.55, "width"); break;
    case E::PitchCorrection: add("Auto-Tuned", 0.6, "pitch"); break;
    case E::PitchShift: add("Pitched", 0.4, "pitch"); break;
    case E::Vocoder: add("Vocoded", 0.75, "voice"); break;
    case E::Glitch: add("Glitchy", 0.6, "glitch"); break;
    case E::Granular: add("Granular", 0.55, "glitch"); break;
    case E::Amp: add("Overdriven", 0.55, "drive"); break;
    case E::Mastering: add("Mastered", 0.3, "master"); break;
    default: break;
    }
    return reading;
}

}  // namespace sub::intelligence::labels
