// Plug-in CPU experiments (perf-experiments branch): a song made of real VST3
// plug-ins (the open-source ones Ubuntu packages: LSP, Dragonfly, ZamAudio,
// DISTRHO), rendered and played with each of the engine's experiments
// (rt/Experiments.h) on and off.
//
//     plugin_cpu_bench survey                      each candidate plug-in alone: cost per second of audio
//     plugin_cpu_bench profile [--bars 48]         the song offline, where the time goes (per plug-in)
//     plugin_cpu_bench offline [--threads 1,2,4]   the song offline with each idea on and off
//     plugin_cpu_bench blocks                      the song offline at block sizes 32..1024
//     plugin_cpu_bench live [--buffers 64,128,256] [--threads 1,4] [--seconds 20]
//                                                  the song played through the Bench driver
//
// The song: 16 tracks in sections of 8 bars (intro, verse, build, chorus, break,
// chorus), each playing in some of them, as an arrangement does: drums (audio
// one-shots in loops) into a drum group, bass, pads, leads and keys (the
// built-in Synth and DISTRHO's Kars) into a music group, audio "vocals", a
// riser, sends into a reverb and a delay return, and a mastering chain.

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Engine.h"
#include "Fixtures.h"
#include "backends/BenchBackend.h"
#include "plugins/Vst3Format.h"
#include "rt/Experiments.h"

namespace subtest {
void fail(const char* file, int line, const std::string& message) {
    std::fprintf(stderr, "%s:%d: %s\n", std::filesystem::path(file).filename().string().c_str(), line,
                 message.c_str());
}
}  // namespace subtest

namespace {

using Clock = std::chrono::steady_clock;
namespace ex = sub::experiments;

constexpr int kRate = 48000;
constexpr double kBeatSec = 0.5;  // 120 BPM
constexpr int kBarBeats = 4;
constexpr int kSectionBars = 8;
constexpr double kSectionSec = kSectionBars * kBarBeats * kBeatSec;  // 16 s
const char* const kLsp = "/usr/lib/vst3/lsp-plugins.vst3";

std::string vst3(const std::string& name) { return "/usr/lib/vst3/" + name + ".vst3"; }

struct Stop : std::runtime_error {
    using std::runtime_error::runtime_error;
};

double cpuSeconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec * 1e-6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec * 1e-6;
}

class TempFolder {
public:
    TempFolder() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() / ("sub-plugin-bench-" + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// --- Plug-ins ---------------------------------------------------------------------------------

struct PluginRef {
    std::string path;
    std::string name;
};

std::string uidOf(const PluginRef& plugin) {
    static std::map<std::string, std::vector<sub::PluginDescription>> scanned;
    auto it = scanned.find(plugin.path);
    if (it == scanned.end()) it = scanned.emplace(plugin.path, sub::vst3::Vst3Format::instance().scanFile(plugin.path)).first;
    for (const auto& d : it->second)
        if (d.name == plugin.name) return d.uid;
    throw Stop("No plug-in named '" + plugin.name + "' in " + plugin.path);
}

uint32_t addPlugin(sub::Engine& engine, uint32_t chain, const PluginRef& plugin) {
    return engine.addPluginProcessor(chain, "VST3", plugin.path, uidOf(plugin), -1);
}

const PluginRef kEq{kLsp, "Parametric Equalizer x8 Stereo"};
const PluginRef kComp{kLsp, "Compressor Stereo"};
const PluginRef kChorus{kLsp, "Chorus Stereo"};
const PluginRef kFilter{kLsp, "Filter Stereo"};
const PluginRef kMultiband{kLsp, "Multiband Compressor Stereo x8"};
const PluginRef kLimiter{kLsp, "Limiter Stereo"};
const PluginRef kHall{vst3("DragonflyHallReverb"), "Dragonfly Hall Reverb"};
const PluginRef kPlate{vst3("DragonflyPlateReverb"), "Dragonfly Plate Reverb"};
const PluginRef kDelay{vst3("ZamDelay"), "ZamDelay"};
const PluginRef kSoulForce{vst3("SoulForce"), "Soul Force"};
const PluginRef kCompX2{vst3("ZamCompX2"), "ZamCompX2"};
const PluginRef kEq2{vst3("ZamEQ2"), "ZamEQ2"};  // (puts out NaN here: not in the song)
const PluginRef kDynamics{kLsp, "Dynamics Processor Stereo"};
const PluginRef kKars{vst3("Kars"), "Kars"};
const PluginRef kNekobi{vst3("Nekobi"), "Nekobi"};

std::vector<PluginRef> surveyPlugins() {
    return {kEq,
            kComp,
            kChorus,
            kFilter,
            kMultiband,
            kLimiter,
            {kLsp, "Graphic Equalizer x32 Stereo"},
            {kLsp, "Parametric Equalizer x32 Stereo"},
            {kLsp, "Artistic Delay Stereo"},
            {kLsp, "Impulse Reverb Stereo"},
            {kLsp, "Dynamics Processor Stereo"},
            {kLsp, "GOTT Compressor Stereo"},
            {kLsp, "Spectrum Analyzer x2"},
            kHall,
            kPlate,
            {vst3("DragonflyRoomReverb"), "Dragonfly Room Reverb"},
            {vst3("ZamVerb"), "ZamVerb"},
            {vst3("MVerb"), "MVerb"},
            kDelay,
            kSoulForce,
            kCompX2,
            kEq2,
            {vst3("ZaMultiCompX2"), "ZaMultiCompX2"},
            {vst3("ZaMaximX2"), "ZaMaximX2"},
            {vst3("ZamTube"), "ZamTube"}};
}

// --- Audio for the song --------------------------------------------------------------------------

struct Sounds {
    std::string kick, snare, hats, perc, vocal, guitar, riser, impact, noise;
};

// An 8-bar loop (stereo) built from one-shots at beat positions.
std::vector<float> loopOf(const std::vector<float>& hit, const std::vector<double>& beatsInBar) {
    const auto frames = static_cast<size_t>(kSectionSec * kRate);
    std::vector<float> out(frames * 2, 0.f);
    for (int bar = 0; bar < kSectionBars; ++bar) {
        for (const double beat : beatsInBar) {
            const auto at = static_cast<size_t>((bar * kBarBeats + beat) * kBeatSec * kRate);
            for (size_t i = 0; i < hit.size() && at + i < frames; ++i) {
                out[2 * (at + i)] += hit[i];
                out[2 * (at + i) + 1] += hit[i];
            }
        }
    }
    return out;
}

Sounds makeSounds(const std::filesystem::path& folder) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> uniform(-1.f, 1.f);
    const auto decaying = [&](double seconds, double decay, double freq, double noise) {
        std::vector<float> hit(static_cast<size_t>(seconds * kRate));
        double phase = 0.0;
        for (size_t i = 0; i < hit.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            const double f = freq * (1.0 + 2.0 * std::exp(-t * 40.0));  // a pitch drop, as a kick's
            phase += 2.0 * M_PI * f / kRate;
            const double env = std::exp(-t / decay);
            hit[i] = static_cast<float>(0.7 * env * ((1.0 - noise) * std::sin(phase) + noise * uniform(rng)));
        }
        return hit;
    };
    Sounds s;
    s.kick = subtest::writeWav(folder / "kick.wav", loopOf(decaying(0.35, 0.08, 50, 0.05), {0, 1, 2, 3}), 2, kRate);
    s.snare = subtest::writeWav(folder / "snare.wav", loopOf(decaying(0.25, 0.06, 180, 0.7), {1, 3}), 2, kRate);
    s.hats = subtest::writeWav(folder / "hats.wav",
                               loopOf(decaying(0.06, 0.015, 8000, 0.95), {0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5}), 2, kRate);
    s.perc = subtest::writeWav(folder / "perc.wav", loopOf(decaying(0.15, 0.04, 400, 0.4), {0.75, 2.25, 3.5}), 2, kRate);
    // "Vocal" and "guitar": phrases of a few beats with gaps, tones with vibrato and some noise.
    const auto phrases = [&](double freq, double noise, double phraseBeats, double gapBeats) {
        const auto frames = static_cast<size_t>(kSectionSec * kRate);
        std::vector<float> out(frames * 2, 0.f);
        double phase = 0.0;
        for (size_t i = 0; i < frames; ++i) {
            const double beat = static_cast<double>(i) / kRate / kBeatSec;
            const double inPhrase = std::fmod(beat, phraseBeats + gapBeats);
            if (inPhrase >= phraseBeats) continue;
            const double env = std::min({1.0, inPhrase * 8.0, (phraseBeats - inPhrase) * 8.0});
            phase += 2.0 * M_PI * freq * (1.0 + 0.01 * std::sin(beat * 10.0)) / kRate;
            const auto v = static_cast<float>(0.3 * env * ((1.0 - noise) * std::sin(phase) + noise * uniform(rng)));
            out[2 * i] = v;
            out[2 * i + 1] = v;
        }
        return out;
    };
    s.vocal = subtest::writeWav(folder / "vocal.wav", phrases(330, 0.2, 6, 2), 2, kRate);
    s.guitar = subtest::writeWav(folder / "guitar.wav", phrases(196, 0.1, 3, 1), 2, kRate);
    {
        const auto frames = static_cast<size_t>(kSectionSec * kRate);
        std::vector<float> riser(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            const auto v = static_cast<float>(0.25 * static_cast<double>(i) / frames * uniform(rng));
            riser[2 * i] = v;
            riser[2 * i + 1] = v;
        }
        s.riser = subtest::writeWav(folder / "riser.wav", riser, 2, kRate);
    }
    s.impact = subtest::writeWav(folder / "impact.wav", loopOf(decaying(1.5, 0.4, 60, 0.5), {0}), 2, kRate);
    {
        const auto frames = static_cast<size_t>(10.0 * kRate);
        std::vector<float> noise(frames * 2);
        for (auto& v : noise) v = 0.25f * uniform(rng);
        s.noise = subtest::writeWav(folder / "noise.wav", noise, 2, kRate);
    }
    return s;
}

// --- The song ------------------------------------------------------------------------------------

struct Song {
    std::vector<uint32_t> tracks;  // the 16 instruments
    uint32_t drums = 0, music = 0, reverb = 0, delay = 0;
    int bars = 0;
    std::map<uint32_t, uint32_t> outputOf;                // instrument -> its group (absent: the master)
    std::map<uint32_t, std::vector<uint32_t>> sendsOf;    // instrument -> the returns it sends to
    std::vector<uint32_t> masterDevices;
};

// Sections: 0 intro, 1 verse, 2 build, 3 chorus, 4 break, 5 chorus.
using Sections = std::vector<int>;

std::vector<sub::ClipDesc> clipsIn(const std::string& path, const Sections& sections, int numSections) {
    std::vector<sub::ClipDesc> clips;
    for (const int s : sections)
        if (s < numSections) clips.push_back(subtest::clip(path, s * kSectionBars * kBarBeats, kSectionSec));
    return clips;
}

std::vector<sub::NoteDesc> notesIn(const Sections& sections, int numSections, const std::vector<int>& chord,
                                   double every, double length, int octaveShiftEvery = 0) {
    std::vector<sub::NoteDesc> notes;
    const int roots[4] = {0, 5, 3, 7};
    for (const int s : sections) {
        if (s >= numSections) continue;
        const double start = s * kSectionBars * kBarBeats;
        for (double beat = 0; beat < kSectionBars * kBarBeats; beat += every) {
            const int root = roots[static_cast<int>(beat / 8) % 4];
            const int shift = octaveShiftEvery ? 12 * (static_cast<int>(beat / every) % octaveShiftEvery) : 0;
            for (const int key : chord) notes.push_back({start + beat, length, key + root + shift, 100});
        }
    }
    return notes;
}

Song buildSong(sub::Engine& engine, const Sounds& sounds, int bars, int copies = 1) {
    Song song;
    song.bars = bars;
    const int numSections = (bars + kSectionBars - 1) / kSectionBars;
    for (const std::string& path : {sounds.kick, sounds.snare, sounds.hats, sounds.perc, sounds.vocal, sounds.guitar,
                                    sounds.riser, sounds.impact})
        engine.loadSource(path);

    // Groups and returns.
    song.drums = engine.addTrack();
    addPlugin(engine, engine.trackChain(song.drums), kComp);
    addPlugin(engine, engine.trackChain(song.drums), kSoulForce);
    song.music = engine.addTrack();
    addPlugin(engine, engine.trackChain(song.music), kEq);
    addPlugin(engine, engine.trackChain(song.music), kCompX2);
    song.reverb = engine.addTrack();
    addPlugin(engine, engine.trackChain(song.reverb), kHall);
    addPlugin(engine, engine.trackChain(song.reverb), kEq);
    song.delay = engine.addTrack();
    addPlugin(engine, engine.trackChain(song.delay), kDelay);
    addPlugin(engine, engine.trackChain(song.delay), kFilter);
    // The master.
    const uint32_t master = engine.trackChain(sub::Engine::kMaster);
    song.masterDevices.push_back(addPlugin(engine, master, kEq));
    song.masterDevices.push_back(addPlugin(engine, master, kMultiband));
    song.masterDevices.push_back(addPlugin(engine, master, kLimiter));

    const auto audioTrack = [&](const std::string& path, const Sections& sections, uint32_t group,
                                const std::vector<PluginRef>& chain, float reverbSend, float delaySend) {
        const uint32_t track = engine.addTrack();
        engine.setTrackClips(track, clipsIn(path, sections, numSections));
        for (const auto& plugin : chain) addPlugin(engine, engine.trackChain(track), plugin);
        if (group) engine.setTrackOutput(track, group);
        if (group) song.outputOf[track] = group;
        if (reverbSend > 0) engine.setTrackSend(track, song.reverb, reverbSend, false);
        if (delaySend > 0) engine.setTrackSend(track, song.delay, delaySend, false);
        if (reverbSend > 0) song.sendsOf[track].push_back(song.reverb);
        if (delaySend > 0) song.sendsOf[track].push_back(song.delay);
        engine.setTrackGain(track, 0.5f);
        song.tracks.push_back(track);
        return track;
    };
    const auto synthTrack = [&](const std::vector<sub::NoteDesc>& notes, int waveform, const PluginRef* instrument,
                                uint32_t group, const std::vector<PluginRef>& chain, float reverbSend, float delaySend) {
        const uint32_t track = engine.addTrack();
        if (instrument) {
            addPlugin(engine, engine.trackChain(track), *instrument);
        } else {
            const uint32_t synth = engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
            engine.setProcessorParam(synth, 0, static_cast<float>(waveform));
        }
        engine.setTrackNotes(track, notes);
        for (const auto& plugin : chain) addPlugin(engine, engine.trackChain(track), plugin);
        if (group) engine.setTrackOutput(track, group);
        if (group) song.outputOf[track] = group;
        if (reverbSend > 0) engine.setTrackSend(track, song.reverb, reverbSend, false);
        if (delaySend > 0) engine.setTrackSend(track, song.delay, delaySend, false);
        if (reverbSend > 0) song.sendsOf[track].push_back(song.reverb);
        if (delaySend > 0) song.sendsOf[track].push_back(song.delay);
        engine.setTrackGain(track, 0.3f);
        song.tracks.push_back(track);
        return track;
    };

    for (int copy = 0; copy < copies; ++copy) {
    // Drums.
    audioTrack(sounds.kick, {1, 2, 3, 5}, song.drums, {kEq, kComp}, 0, 0);
    audioTrack(sounds.snare, {1, 3, 5}, song.drums, {kEq, kComp}, 0.3f, 0);
    audioTrack(sounds.hats, {0, 1, 2, 3, 5}, song.drums, {kEq}, 0, 0);
    audioTrack(sounds.perc, {2, 3, 5}, song.drums, {kEq, kDelay}, 0.2f, 0);
    // Music.
    synthTrack(notesIn({1, 2, 3, 5}, numSections, {36}, 0.5, 0.4), 1, nullptr, song.music, {kEq, kComp, kSoulForce}, 0, 0);
    synthTrack(notesIn({3, 5}, numSections, {24}, 2.0, 1.9), 0, nullptr, song.music, {kEq, kComp}, 0, 0);
    synthTrack(notesIn({0, 2, 4, 5}, numSections, {48, 52, 55, 59}, 8.0, 7.9), 2, nullptr, song.music, {kEq, kChorus}, 0.4f, 0);
    synthTrack(notesIn({3, 5}, numSections, {72}, 0.75, 0.5, 2), 1, nullptr, song.music, {kEq, kComp}, 0.3f, 0.3f);
    synthTrack(notesIn({1, 2, 3}, numSections, {60, 67}, 0.25, 0.2, 3), 3, nullptr, song.music, {kEq, kFilter}, 0, 0.2f);
    synthTrack(notesIn({1, 4}, numSections, {60, 64, 67}, 2.0, 1.5), 0, &kKars, song.music, {kEq, kComp}, 0.2f, 0);
    audioTrack(sounds.vocal, {1, 3, 5}, 0, {kEq, kComp, kDynamics}, 0.3f, 0.2f);
    audioTrack(sounds.riser, {2}, 0, {kFilter}, 0.5f, 0);
    synthTrack(notesIn({4, 5}, numSections, {55, 59, 62, 67}, 4.0, 3.9), 2, nullptr, song.music, {kEq}, 0.5f, 0);
    audioTrack(sounds.guitar, {1, 3}, song.music, {kSoulForce, kEq}, 0.2f, 0);
    audioTrack(sounds.vocal, {3, 5}, 0, {kEq, kComp}, 0.3f, 0);
    audioTrack(sounds.impact, {2, 3, 4}, 0, {kEq}, 0.6f, 0);
    }
    engine.idle();
    return song;
}

// --- Modes ---------------------------------------------------------------------------------------

struct Options {
    std::string mode;
    int bars = 48;
    std::vector<int> threads{1};
    std::vector<int> buffers{128};
    double seconds = 20.0;
    double holdMs = 500;
    std::string only;  // offline/live: only these configurations (a,b,c)
    int copies = 1;    // the song's instrument tracks, this many times over
    int block = 1024;  // profile: the offline block size
};

struct Config {
    std::string name;
    bool suspend = false;
    bool skipUnheard = false;
    bool muteHalf = false;  // half the instrument tracks muted (mixing: comparing parts)
    bool solo = false;      // one track soloed
    bool stopped = false;   // live: the transport stopped
    bool pipeline = false;  // the master's devices a chunk late, beside the next chunk's tracks
};

std::vector<Config> configs(bool live) {
    std::vector<Config> out = {
        {"baseline", false, false, false, false, false},
        {"suspend", true, false, false, false, false},
        {"4 muted", false, false, true, false, false},
        {"4 muted+skip", false, true, true, false, false},
        {"solo", false, false, false, true, false},
        {"solo+skip", false, true, false, true, false},
        {"solo+skip+suspend", true, true, false, true, false},
        {"pipeline", false, false, false, false, false, true},
        {"pipeline+suspend", true, false, false, false, false, true},
    };
    if (live) {
        out.push_back({"stopped", false, false, false, false, true});
        out.push_back({"stopped+suspend", true, false, false, false, true});
    }
    return out;
}

// --only a,b,c: those configurations (and the baseline).
bool wanted(const Options& options, const Config& config) {
    if (options.only.empty() || config.name == "baseline") return true;
    size_t at = 0;
    while (at <= options.only.size()) {
        const size_t comma = std::min(options.only.find(',', at), options.only.size());
        if (options.only.substr(at, comma - at) == config.name) return true;
        at = comma + 1;
    }
    return false;
}

void apply(sub::Engine& engine, const Song& song, const Config& config) {
    ex::suspend = config.suspend;
    ex::skipUnheard = config.skipUnheard;
    ex::pipelineMaster = config.pipeline;
    for (size_t i = 0; i < song.tracks.size(); ++i) {
        engine.setTrackMute(song.tracks[i], config.muteHalf && (i % 4 == 1));
        engine.setTrackSolo(song.tracks[i], config.solo && i % 16 == 6);  // the pads
    }
}

// Each candidate on stereo noise: what comes out (RMS, dB), against what goes in.
void check(const Options&) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    for (const PluginRef& plugin : surveyPlugins()) {
        sub::Engine engine;
        engine.loadSource(sounds.noise);
        const uint32_t track = engine.addTrack();
        engine.setTrackClips(track, {subtest::clip(sounds.noise, 0, 10.0)});
        const auto dry = engine.renderOffline(0.0, 4 * kRate);
        addPlugin(engine, engine.trackChain(track), plugin);
        engine.idle();
        const auto wet = engine.renderOffline(0.0, 4 * kRate);
        const auto rmsDb = [](const std::vector<float>& v) {
            double sum = 0;
            for (size_t i = v.size() / 2; i < v.size(); ++i) sum += static_cast<double>(v[i]) * v[i];
            return 10 * std::log10(sum / static_cast<double>(v.size() / 2) + 1e-30);
        };
        std::printf("%-36s in %6.1f dB  out %6.1f dB\n", plugin.name.c_str(), rmsDb(dry), rmsDb(wet));
        if (std::isnan(rmsDb(wet))) {  // every parameter sent to it again, as a host restoring it would
            uint32_t id = 0;
            for (const auto& p : engine.processorProfiles()) id = p.processorId;
            const auto params = engine.processorParams(id);
            for (int i = 0; i < static_cast<int>(params.size()); ++i) engine.setProcessorParam(id, i, engine.processorParam(id, i));
            engine.idle();
            const auto again = engine.renderOffline(0.0, 4 * kRate);
            std::printf("%-36s with its parameters sent: out %6.1f dB\n", "", rmsDb(again));
        }
        std::fflush(stdout);
    }
}

void survey(const Options& options) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    std::printf("%-36s %10s %10s %8s %8s\n", "plug-in (alone, stereo noise)", "us/s", "% of 1 core", "latency", "tail");
    for (const PluginRef& plugin : surveyPlugins()) {
        sub::Engine engine;
        engine.loadSource(sounds.noise);
        const uint32_t track = engine.addTrack();
        engine.setTrackClips(track, {subtest::clip(sounds.noise, 0, 10.0)});
        uint32_t id = 0;
        try {
            id = addPlugin(engine, engine.trackChain(track), plugin);
        } catch (const std::exception& e) {
            std::printf("%-36s %s\n", plugin.name.c_str(), e.what());
            continue;
        }
        engine.idle();
        const int64_t frames = 10 * kRate;
        engine.renderOffline(0.0, kRate);  // warm up
        double best = 1e9;
        for (int i = 0; i < 3; ++i) {
            const auto start = Clock::now();
            engine.renderOffline(0.0, frames);
            best = std::min(best, std::chrono::duration<double>(Clock::now() - start).count());
        }
        // The empty track's cost: the engine alone.
        const double perSecond = best / 10.0;
        int latency = 0, tail = 0;
        ex::profile = true;
        for (const auto& p : engine.processorProfiles())
            if (p.processorId == id) {
                latency = p.latency;
                tail = p.tail;
            }
        ex::profile = false;
        std::printf("%-36s %10.0f %9.2f%% %8d %8d\n", plugin.name.c_str(), perSecond * 1e6, perSecond * 100, latency, tail);
        std::fflush(stdout);
    }
}

double renderSeconds(sub::Engine& engine, int64_t frames, std::vector<float>* out = nullptr) {
    const auto start = Clock::now();
    auto rendered = engine.renderOffline(0.0, frames);
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    if (out) *out = std::move(rendered);
    return seconds;
}

void profile(const Options& options) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    sub::Engine engine;
    engine.setAudioThreads(1);
    const Song song = buildSong(engine, sounds, options.bars, options.copies);
    const int64_t frames = static_cast<int64_t>(song.bars * kBarBeats * kBeatSec * kRate);
    renderSeconds(engine, kRate);  // warm up
    engine.resetProcessorProfiles();
    ex::vst3WrapperNs = 0;
    ex::vst3PluginNs = 0;
    ex::vst3OutParams = 0;
    ex::vst3OutParamsDropped = 0;
    ex::offlineBlock = options.block;
    ex::profile = true;
    const double total = renderSeconds(engine, frames);
    ex::profile = false;
    ex::offlineBlock = 1024;
    const double audioSeconds = static_cast<double>(frames) / kRate;
    std::printf("Blocks of %d frames. VST3 calls: the plug-ins' own time %.3f s, the host's part of the calls %.3f s "
                "(%.2f%%); output parameter values to the main thread: %.0f per second of audio (%.0f dropped)\n",
                options.block, ex::vst3PluginNs * 1e-9, ex::vst3WrapperNs * 1e-9,
                100.0 * static_cast<double>(ex::vst3WrapperNs) / static_cast<double>(ex::vst3PluginNs + ex::vst3WrapperNs),
                static_cast<double>(ex::vst3OutParams) / (static_cast<double>(frames) / kRate),
                static_cast<double>(ex::vst3OutParamsDropped));
    std::printf("The song, %d bars (%.0f s), offline on one thread: %.3f s (%.1f%% of one core in real time)\n\n",
                song.bars, audioSeconds, total, total / audioSeconds * 100);
    auto profiles = engine.processorProfiles();
    uint64_t pluginNs = 0, quietNs = 0, calls = 0, quietCalls = 0, quietInCalls = 0;
    std::map<std::string, std::array<double, 4>> byName;  // ns, quiet ns, count, calls
    for (const auto& p : profiles) {
        pluginNs += p.ns;
        quietNs += p.quietNs;
        calls += p.calls;
        quietCalls += p.quietCalls;
        quietInCalls += p.quietInCalls;
        auto& entry = byName[p.name];
        entry[0] += static_cast<double>(p.ns);
        entry[1] += static_cast<double>(p.quietNs);
        entry[2] += 1;
        entry[3] += static_cast<double>(p.calls);
    }
    std::printf("Devices: %.3f s of it (%.1f%%); %.3f s (%.1f%% of the devices' time) processing silence in and out\n",
                pluginNs * 1e-9, pluginNs * 1e-9 / total * 100, quietNs * 1e-9,
                pluginNs ? 100.0 * static_cast<double>(quietNs) / static_cast<double>(pluginNs) : 0.0);
    std::printf("Calls: %llu; with quiet input and no events %llu (%.1f%%); quiet in and out %llu (%.1f%%)\n\n",
                static_cast<unsigned long long>(calls), static_cast<unsigned long long>(quietInCalls),
                100.0 * static_cast<double>(quietInCalls) / static_cast<double>(calls),
                static_cast<unsigned long long>(quietCalls), 100.0 * static_cast<double>(quietCalls) / static_cast<double>(calls));
    std::vector<std::pair<std::string, std::array<double, 4>>> sorted(byName.begin(), byName.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second[0] > b.second[0]; });
    std::printf("%-34s %5s %10s %8s %10s %12s\n", "device", "count", "total s", "% total", "quiet %", "us per call");
    for (const auto& [name, v] : sorted) {
        std::printf("%-34s %5.0f %10.3f %7.1f%% %9.1f%% %12.2f\n", name.c_str(), v[2], v[0] * 1e-9, v[0] * 1e-9 / total * 100,
                    v[0] > 0 ? 100.0 * v[1] / v[0] : 0.0, v[3] > 0 ? v[0] / v[3] / 1000 : 0.0);
    }
    std::printf("\nPer instance:\n%-5s %-34s %10s %8s %8s %8s\n", "id", "device", "total s", "quiet %", "latency", "tail");
    for (const auto& p : profiles) {
        std::printf("%-5u %-34s %10.3f %7.1f%% %8d %8d\n", p.processorId, p.name.c_str(), p.ns * 1e-9,
                    p.ns ? 100.0 * static_cast<double>(p.quietNs) / static_cast<double>(p.ns) : 0.0, p.latency, p.tail);
    }
}

// How the song's work is shared out: each track's time and the longest chain
// of work that must happen one after the other (a track, its group, a return,
// the master), from the totals of a render on one thread.
void graph(const Options& options) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    sub::Engine engine;
    engine.setAudioThreads(1);
    const Song song = buildSong(engine, sounds, options.bars, options.copies);
    const int64_t frames = static_cast<int64_t>(song.bars * kBarBeats * kBeatSec * kRate);
    renderSeconds(engine, kRate);
    engine.resetProcessorProfiles();
    ex::profile = true;
    const double total = renderSeconds(engine, frames);
    ex::profile = false;
    std::map<uint32_t, double> cost;
    for (const auto& c : engine.trackCosts()) cost[c.trackId] = static_cast<double>(c.totalNs) * 1e-9;
    double master = 0;
    for (const auto& p : engine.processorProfiles())
        if (std::find(song.masterDevices.begin(), song.masterDevices.end(), p.processorId) != song.masterDevices.end())
            master += static_cast<double>(p.ns) * 1e-9;
    double tracks = 0;
    for (const auto& [id, c] : cost) tracks += c;
    // Finish times, as if every node's work were spread evenly over the song.
    std::map<uint32_t, double> finish;
    double leafMax = 0;
    for (const uint32_t t : song.tracks) {
        finish[t] = cost[t];
        leafMax = std::max(leafMax, cost[t]);
    }
    for (const uint32_t bus : {song.drums, song.music, song.reverb, song.delay}) {
        double ready = 0;
        for (const uint32_t t : song.tracks) {
            const bool feeds = (song.outputOf.count(t) && song.outputOf.at(t) == bus) ||
                               (song.sendsOf.count(t) && std::count(song.sendsOf.at(t).begin(), song.sendsOf.at(t).end(), bus));
            if (feeds) ready = std::max(ready, finish[t]);
        }
        finish[bus] = ready + cost[bus];
    }
    double masterReady = 0;
    for (const auto& [id, f] : finish) masterReady = std::max(masterReady, f);
    const double critical = masterReady + master;
    std::printf("One thread: %.3f s for %.0f s of audio. Tracks (all): %.3f s; master strip devices: %.3f s; the rest "
                "(prologue, summing, epilogue): %.3f s\n", total, static_cast<double>(frames) / kRate, tracks, master,
                total - tracks - master);
    std::printf("%-10s %10s %10s\n", "track", "own s", "finish s");
    for (const uint32_t t : song.tracks) std::printf("%-10u %10.3f %10.3f\n", t, cost[t], finish[t]);
    std::printf("%-10s %10.3f %10.3f\n", "drums", cost[song.drums], finish[song.drums]);
    std::printf("%-10s %10.3f %10.3f\n", "music", cost[song.music], finish[song.music]);
    std::printf("%-10s %10.3f %10.3f\n", "reverb", cost[song.reverb], finish[song.reverb]);
    std::printf("%-10s %10.3f %10.3f\n", "delay", cost[song.delay], finish[song.delay]);
    std::printf("%-10s %10.3f %10.3f\n", "master", master, critical);
    std::printf("\nCritical path ~ %.3f s of %.3f s: no number of threads renders this faster than %.1fx one thread; "
                "the master's devices alone are %.0f%% of the path (serial, after every track)\n",
                critical, total, total / critical, 100 * master / critical);
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}

// The difference's energy against the reference's, in dB (-inf: identical).
double errorDb(const std::vector<float>& out, const std::vector<float>& ref) {
    double e = 0, r = 0;
    for (size_t i = 0; i < std::min(out.size(), ref.size()); ++i) {
        const double d = static_cast<double>(out[i]) - ref[i];
        e += d * d;
        r += static_cast<double>(ref[i]) * ref[i];
    }
    return e > 0 ? 10 * std::log10(e / std::max(r, 1e-30)) : -999.0;
}

// How much audio each plug-in must hear before a point (fresh from a reset) for
// its output from there on to match a render that ran from the start: what a
// cache-to-live switch must pre-roll. Program material: vocal-like phrases with
// noise (3 s on, 1 s off); the point inside a phrase; compared over the 2 s after it.
void warmup(const Options&) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    const double point = 13.5;  // seconds: inside a phrase (they run 12-15 s)
    const double compare = 2.0;
    const std::vector<double> windows{0.0, 0.01, 0.05, 0.2, 0.5, 1.0, 2.0, 4.0, 8.0};
    std::printf("Error (dB, difference energy / signal energy) over the 2 s after the switch, by pre-roll\n");
    std::printf("%-34s %8s", "plug-in", "control");
    for (const double w : windows) std::printf(" %7gs", w);
    std::printf("\n");
    std::vector<PluginRef> plugins = surveyPlugins();
    plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [](const PluginRef& p) { return p.name == "ZamEQ2"; }),
                  plugins.end());
    for (const PluginRef& plugin : plugins) {
        sub::Engine engine;
        engine.loadSource(sounds.vocal);
        const uint32_t track = engine.addTrack();
        engine.setTrackClips(track, {subtest::clip(sounds.vocal, 0, kSectionSec)});
        addPlugin(engine, engine.trackChain(track), plugin);
        engine.idle();
        const auto total = std::llround((point + compare) * kRate);
        const auto ref = engine.renderOffline(0.0, total);
        const auto tailOf = [&](const std::vector<float>& v) {
            return std::vector<float>(v.end() - std::llround(compare * kRate) * 2, v.end());
        };
        const auto refTail = tailOf(ref);
        std::printf("%-34s %8.1f", plugin.name.c_str(), errorDb(tailOf(engine.renderOffline(0.0, total)), refTail));
        for (const double w : windows) {
            const double startSec = point - w;
            const auto frames = std::llround((w + compare) * kRate);
            const auto out = engine.renderOffline(startSec / kBeatSec, frames);
            std::printf(" %8.1f", errorDb(tailOf(out), refTail));
        }
        std::printf("\n");
        std::fflush(stdout);
    }
}

// The pipelined master against the plain one, with plug-ins that render the
// same every time: the output must be the same, a chunk later.
void pipecheck(const Options&) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    sub::Engine engine;
    engine.setAudioThreads(4);
    engine.loadSource(sounds.vocal);
    engine.loadSource(sounds.kick);
    for (int t = 0; t < 4; ++t) {
        const uint32_t track = engine.addTrack();
        engine.setTrackClips(track, {subtest::clip(t % 2 ? sounds.vocal : sounds.kick, 0, kSectionSec)});
        addPlugin(engine, engine.trackChain(track), kEq);
        addPlugin(engine, engine.trackChain(track), kComp);
    }
    const uint32_t master = engine.trackChain(sub::Engine::kMaster);
    addPlugin(engine, master, kEq);
    addPlugin(engine, master, kMultiband);
    addPlugin(engine, master, kLimiter);
    engine.idle();
    const int64_t frames = static_cast<int64_t>(kSectionSec * kRate);
    ex::pipelineMaster = false;
    const auto plain = engine.renderOffline(0.0, frames);
    const auto again = engine.renderOffline(0.0, frames);
    ex::pipelineMaster = true;
    auto piped = engine.renderOffline(0.0, frames);
    ex::pipelineMaster = false;
    piped.erase(piped.begin(), piped.begin() + 2048);
    std::printf("plain twice: max diff %.1f dB; pipelined (a chunk later) vs plain: max diff %.1f dB, error %.1f dB\n",
                maxDiff(again, plain) > 0 ? 20 * std::log10(maxDiff(again, plain)) : -999.0,
                maxDiff(piped, plain) > 0 ? 20 * std::log10(maxDiff(piped, plain)) : -999.0, errorDb(piped, plain));
}

// Each candidate on a signal with gaps (2 s on, 3 s off): with suspension on
// and off, how far apart the two renders are.
void sleeptest(const Options& options) {
    TempFolder folder;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> uniform(-1.f, 1.f);
    const auto frames = static_cast<size_t>(30.0 * kRate);
    std::vector<float> gated(frames * 2, 0.f);
    double phase = 0;
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        if (std::fmod(t, 5.0) >= 2.0) continue;
        phase += 2 * M_PI * 220 / kRate;
        const auto v = static_cast<float>(0.3 * (0.7 * std::sin(phase) + 0.3 * uniform(rng)));
        gated[2 * i] = v;
        gated[2 * i + 1] = v;
    }
    const std::string path = subtest::writeWav(folder.path() / "gated.wav", gated, 2, kRate);
    ex::holdMs = static_cast<int>(options.holdMs);
    std::printf("Each plug-in alone on 2 s of signal then 3 s of silence, 30 s; hold %g ms\n", options.holdMs);
    std::printf("%-36s %9s %10s %12s %10s %14s\n", "plug-in", "saved", "slept", "max diff dB", "error dB",
                "control err dB");
    for (const PluginRef& plugin : surveyPlugins()) {
        sub::Engine engine;
        engine.loadSource(path);
        const uint32_t track = engine.addTrack();
        engine.setTrackClips(track, {subtest::clip(path, 0, 30.0)});
        addPlugin(engine, engine.trackChain(track), plugin);
        engine.idle();
        ex::suspend = false;
        ex::profile = true;
        engine.resetProcessorProfiles();
        const auto t0 = Clock::now();
        const auto ref = engine.renderOffline(0.0, static_cast<int64_t>(frames));
        const double off = std::chrono::duration<double>(Clock::now() - t0).count();
        const auto again = engine.renderOffline(0.0, static_cast<int64_t>(frames));  // the control: off again
        ex::suspend = true;
        engine.resetProcessorProfiles();
        const auto t1 = Clock::now();
        const auto out = engine.renderOffline(0.0, static_cast<int64_t>(frames));
        const double on = std::chrono::duration<double>(Clock::now() - t1).count();
        ex::suspend = false;
        ex::profile = false;
        uint64_t skipped = 0, total = 0;
        for (const auto& p : engine.processorProfiles()) {
            skipped += p.skippedFrames;
            total += p.frames + p.skippedFrames;
        }
        const double diff = maxDiff(out, ref);
        std::printf("%-36s %8.0f%% %9.0f%% %12.1f %10.1f %14.1f\n", plugin.name.c_str(), 100 * (1 - on / off),
                    total ? 100.0 * static_cast<double>(skipped) / static_cast<double>(total) : 0.0,
                    diff > 0 ? 20 * std::log10(diff) : -999.0, errorDb(out, ref), errorDb(again, ref));
        std::fflush(stdout);
    }
}

void offline(const Options& options) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    sub::Engine engine;
    const Song song = buildSong(engine, sounds, options.bars, options.copies);
    const int64_t frames = static_cast<int64_t>(song.bars * kBarBeats * kBeatSec * kRate);
    ex::holdMs = static_cast<int>(options.holdMs);
    std::printf("The song, %d bars (%.0f s) offline, best of 3; diff: largest sample difference from the baseline\n",
                song.bars, static_cast<double>(frames) / kRate);
    std::printf("%-20s %8s %9s %10s %9s %12s %10s\n", "config", "threads", "time", "x realtime", "vs base", "max diff dB",
                "error dB");
    for (const int threads : options.threads) {
        engine.setAudioThreads(threads);
        std::map<std::string, double> base;
        std::map<std::string, std::vector<float>> reference;
        for (const Config& config : configs(false)) {
            if (!wanted(options, config)) continue;
            apply(engine, song, config);
            engine.resetProcessorProfiles();
            renderSeconds(engine, kRate);
            double best = 1e9;
            std::vector<float> out;
            for (int i = 0; i < 3; ++i) {
                engine.resetProcessorProfiles();
                best = std::min(best, renderSeconds(engine, frames, &out));
            }
            // What it is compared with: the same mix (muted, soloed) without the ideas.
            std::string key = config.muteHalf ? "mute" : config.solo ? "solo" : "plain";
            const bool isBase = !config.suspend && !config.skipUnheard && !config.pipeline;
            if (isBase) {
                base[key] = best;
                reference[key] = out;
                // The control: the same render once more (do plug-ins render alike twice?).
                engine.resetProcessorProfiles();
                const auto again = engine.renderOffline(0.0, frames);
                std::printf("%-20s %8s %9s %10s %9s %12.1f %10.1f\n", ("  (" + config.name + " again)").c_str(), "", "",
                            "", "", maxDiff(again, out) > 0 ? 20 * std::log10(maxDiff(again, out)) : -999.0,
                            errorDb(again, out));
            }
            if (config.pipeline && out.size() > 2048) out.erase(out.begin(), out.begin() + 2048);  // a chunk later
            const double diff = reference.count(key) ? maxDiff(out, reference[key]) : 0;
            const double error = reference.count(key) ? errorDb(out, reference[key]) : -999.0;
            std::printf("%-20s %8d %8.3fs %9.1fx %8.0f%% %12.1f %10.1f\n", config.name.c_str(), threads, best,
                        static_cast<double>(frames) / kRate / best, base.count(key) ? 100.0 * best / base[key] : 100.0,
                        diff > 0 ? 20 * std::log10(diff) : -999.0, error);
            std::fflush(stdout);
        }
    }
    apply(engine, song, {});
}

void blocks(const Options& options) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    sub::Engine engine;
    engine.setAudioThreads(1);
    const Song song = buildSong(engine, sounds, options.bars, options.copies);
    const int64_t frames = static_cast<int64_t>(song.bars * kBarBeats * kBeatSec * kRate);
    std::printf("The song, %d bars, offline on one thread, by block size (devices' time from the profile)\n", song.bars);
    std::printf("%6s %9s %10s %12s %12s\n", "block", "time", "vs 1024", "devices", "vs 1024");
    double base = 0, baseDevices = 0;
    for (const int block : {1024, 512, 256, 128, 64, 32}) {
        ex::offlineBlock = block;
        renderSeconds(engine, kRate);
        double best = 1e9, devices = 0;
        for (int i = 0; i < 3; ++i) {
            engine.resetProcessorProfiles();
            ex::profile = true;
            const double t = renderSeconds(engine, frames);
            ex::profile = false;
            if (t < best) {
                best = t;
                devices = 0;
                for (const auto& p : engine.processorProfiles()) devices += static_cast<double>(p.ns) * 1e-9;
            }
        }
        if (block == 1024) {
            base = best;
            baseDevices = devices;
        }
        std::printf("%6d %8.3fs %9.0f%% %11.3fs %11.0f%%\n", block, best, 100 * best / base, devices,
                    100 * devices / baseDevices);
        std::fflush(stdout);
    }
    ex::offlineBlock = 1024;
}

struct LiveStats {
    double meanUs = 0, p50Us = 0, p99Us = 0, p999Us = 0, maxUs = 0, late99Us = 0;
    int overruns = 0;
    size_t callbacks = 0;
    double cpuCores = 0;  // process CPU time over wall time
    bool realtime = false;
};

LiveStats play(sub::Engine& engine, double seconds, int buffer) {
    sub::BenchBackend::takeStats();  // (drop what came before)
    const double cpu0 = cpuSeconds();
    const auto start = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - start).count() < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        engine.idle();
    }
    const double wall = std::chrono::duration<double>(Clock::now() - start).count();
    const double cpu = cpuSeconds() - cpu0;
    auto stats = sub::BenchBackend::takeStats();
    LiveStats out;
    out.realtime = stats.realtime;
    auto& t = stats.callbackUs;
    if (t.empty()) return out;
    out.callbacks = t.size();
    const double budgetUs = 1e6 * buffer / kRate;
    out.overruns = static_cast<int>(std::count_if(t.begin(), t.end(), [&](float v) { return v > budgetUs; }));
    out.meanUs = std::accumulate(t.begin(), t.end(), 0.0) / static_cast<double>(t.size());
    std::sort(t.begin(), t.end());
    const auto pct = [&](double p) { return t[std::min(t.size() - 1, static_cast<size_t>(p * static_cast<double>(t.size())))]; };
    out.p50Us = pct(0.5);
    out.p99Us = pct(0.99);
    out.p999Us = pct(0.999);
    out.maxUs = t.back();
    auto& late = stats.lateUs;
    std::sort(late.begin(), late.end());
    out.late99Us = late[std::min(late.size() - 1, static_cast<size_t>(0.99 * static_cast<double>(late.size())))];
    out.cpuCores = cpu / wall;
    return out;
}

void live(const Options& options) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    sub::Engine engine;
    const Song song = buildSong(engine, sounds, options.bars, options.copies);
    ex::holdMs = static_cast<int>(options.holdMs);
    std::printf("The song played live through the Bench driver from bar 9 (the verse), %g s per run\n", options.seconds);
    std::printf("load: mean callback time / buffer length; overruns: callbacks longer than their buffer (dropouts)\n");
    std::printf("cores: the process's CPU time / wall time (spinning workers included)\n");
    std::printf("%-20s %6s %7s %8s %8s %8s %8s %8s %7s %8s %6s\n", "config", "buffer", "threads", "mean", "p50", "p99",
                "p99.9", "max", "load", "overruns", "cores");
    for (const int buffer : options.buffers) {
        sub::DeviceConfig device;
        device.driver = sub::BenchBackend::kName;
        device.sampleRate = kRate;
        device.bufferFrames = static_cast<uint32_t>(buffer);
        engine.openDevice(device);
        bool warned = false;
        for (const int threads : options.threads) {
            engine.setAudioThreads(threads);
            for (const Config& config : configs(true)) {
                if (!wanted(options, config)) continue;
                engine.stop();
                apply(engine, song, config);
                engine.setLoop(true, 32.0, 32.0 + 4 * kSectionBars * kBarBeats);  // verse to the break, looped
                engine.setPositionBeats(32.0);
                if (!config.stopped) engine.play();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                const LiveStats s = play(engine, options.seconds, buffer);
                if (!s.realtime && !warned) {
                    std::printf("(the Bench driver's thread didn't get SCHED_FIFO)\n");
                    warned = true;
                }
                std::printf("%-20s %6d %7d %6.0fus %6.0fus %6.0fus %6.0fus %6.0fus %6.1f%% %8d %6.2f\n", config.name.c_str(),
                            buffer, threads, s.meanUs, s.p50Us, s.p99Us, s.p999Us, s.maxUs,
                            100.0 * s.meanUs / (1e6 * buffer / kRate), s.overruns, s.cpuCores);
                std::fflush(stdout);
            }
        }
        engine.stop();
        engine.closeDevice();
    }
    apply(engine, song, {});
}

// The scheduler's workers: how long they spin before sleeping, and whether they
// run at real-time priority, live with the song playing.
void sched(const Options& options) {
    TempFolder folder;
    const Sounds sounds = makeSounds(folder.path());
    sub::Engine engine;
    const Song song = buildSong(engine, sounds, options.bars, options.copies);
    std::printf("Workers live (the song from the verse, looped), %g s per run\n", options.seconds);
    std::printf("%-26s %6s %7s %8s %8s %8s %8s %8s %7s %8s %6s\n", "workers", "buffer", "threads", "mean", "p50", "p99",
                "p99.9", "max", "load", "overruns", "cores");
    struct Variant {
        const char* name;
        int spinUs;
        bool rt;
        int preWakeUs;
    };
    const Variant variants[] = {{"spin 50us (now)", 50, false, 0},
                                {"spin 50us, RT workers", 50, true, 0},
                                {"pre-wake 150us", 50, false, 150},
                                {"pre-wake 150us, RT workers", 50, true, 150},
                                {"spin 1 buffer", -1, false, 0}};
    for (const int buffer : options.buffers) {
        sub::DeviceConfig device;
        device.driver = sub::BenchBackend::kName;
        device.sampleRate = kRate;
        device.bufferFrames = static_cast<uint32_t>(buffer);
        engine.openDevice(device);
        for (const int threads : options.threads) {
            for (const Variant& v : variants) {
                ex::spinUs = v.spinUs < 0 ? static_cast<int>(1.2e6 * buffer / kRate) : v.spinUs;
                ex::workerRealtime = v.rt;
                ex::preWakeUs = v.preWakeUs;
                engine.setAudioThreads(1);
                engine.setAudioThreads(threads);  // (new workers: they read workerRealtime as they start)
                engine.stop();
                apply(engine, song, {});
                engine.setLoop(true, 32.0, 32.0 + 4 * kSectionBars * kBarBeats);
                engine.setPositionBeats(32.0);
                engine.play();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                const LiveStats s = play(engine, options.seconds, buffer);
                std::printf("%-26s %6d %7d %6.0fus %6.0fus %6.0fus %6.0fus %6.0fus %6.1f%% %8d %6.2f\n", v.name, buffer,
                            threads, s.meanUs, s.p50Us, s.p99Us, s.p999Us, s.maxUs, 100.0 * s.meanUs / (1e6 * buffer / kRate),
                            s.overruns, s.cpuCores);
                std::fflush(stdout);
            }
        }
        engine.stop();
        engine.closeDevice();
    }
    ex::spinUs = 50;
    ex::workerRealtime = false;
    ex::preWakeUs = 0;
}

std::vector<int> parseList(const std::string& text) {
    std::vector<int> out;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t comma = std::min(text.find(',', at), text.size());
        if (comma > at) out.push_back(std::stoi(text.substr(at, comma - at)));
        at = comma + 1;
    }
    return out;
}

Options parse(int argc, char** argv) {
    if (argc < 2) throw Stop("usage: plugin_cpu_bench survey|profile|offline|blocks|live [options]");
    Options options;
    options.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string name = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw Stop(name + " needs a value");
            return argv[++i];
        };
        if (name == "--bars") options.bars = std::stoi(value());
        else if (name == "--threads") options.threads = parseList(value());
        else if (name == "--buffers") options.buffers = parseList(value());
        else if (name == "--seconds") options.seconds = std::stod(value());
        else if (name == "--hold") options.holdMs = std::stod(value());
        else if (name == "--only") options.only = value();
        else if (name == "--block") options.block = std::stoi(value());
        else if (name == "--copies") options.copies = std::stoi(value());
        else throw Stop("unknown option " + name);
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        if (options.mode == "survey") survey(options);
        else if (options.mode == "check") check(options);
        else if (options.mode == "graph") graph(options);
        else if (options.mode == "sleeptest") sleeptest(options);
        else if (options.mode == "sched") sched(options);
        else if (options.mode == "pipecheck") pipecheck(options);
        else if (options.mode == "warmup") warmup(options);
        else if (options.mode == "profile") profile(options);
        else if (options.mode == "offline") offline(options);
        else if (options.mode == "blocks") blocks(options);
        else if (options.mode == "live") live(options);
        else throw Stop("unknown mode " + options.mode);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "plugin_cpu_bench: %s\n", e.what());
        return 1;
    }
}
