#pragma once
// Sounds for the intelligence tests: drum hits, tones and loops made from
// formulas (with a random generator of our own, so every compiler makes the
// same ones), and WAV files of them.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "harness/Test.h"

namespace subtest {

inline constexpr int kRate = 44100;

using Samples = std::vector<float>;

// xorshift64*: the same numbers everywhere (std's distributions aren't).
class Random {
public:
    explicit Random(uint64_t seed) : state_(seed * 2654435761ull + 1) {}
    uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 2685821657736338717ull;
    }
    double uniform() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }  // [0, 1)
    double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    float noise() { return static_cast<float>(uniform() * 2.0 - 1.0); }

private:
    uint64_t state_;
};

inline size_t frames(double seconds, int rate = kRate) { return static_cast<size_t>(seconds * rate); }

// A kick: a sine falling from `startHz` to `endHz`, dying away over `decay` seconds, and a click.
inline Samples kick(double startHz = 150.0, double endHz = 50.0, double decay = 0.25, double seconds = 0.6,
                    uint64_t seed = 1, int rate = kRate) {
    Random random(seed);
    Samples s(frames(seconds, rate));
    double phase = 0.0;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double f = endHz + (startHz - endHz) * std::exp(-t / 0.03);
        phase += 2.0 * std::numbers::pi * f / rate;
        const double click = t < 0.004 ? 0.3 * random.noise() * (1.0 - t / 0.004) : 0.0;
        s[i] = static_cast<float>(0.9 * std::sin(phase) * std::exp(-t / decay) + click);
    }
    return s;
}

// A snare: a tone at `toneHz` and noise (brightened by differencing), the noise dying over `decay`.
inline Samples snare(double toneHz = 190.0, double decay = 0.12, double noiseShare = 0.7, double seconds = 0.5,
                     uint64_t seed = 2, int rate = kRate) {
    Random random(seed);
    Samples s(frames(seconds, rate));
    float previous = 0.f;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const float n = random.noise();
        const double bright = n - 0.6 * previous;
        previous = n;
        const double tone = std::sin(2.0 * std::numbers::pi * toneHz * t) * std::exp(-t / 0.05);
        s[i] = static_cast<float>(0.6 * ((1.0 - noiseShare) * tone + noiseShare * bright * std::exp(-t / decay)));
    }
    return s;
}

// A hi-hat: high-passed noise (a three-point difference) dying over `decay`.
inline Samples hat(double decay = 0.03, double seconds = 0.4, uint64_t seed = 3) {
    Random random(seed);
    Samples s(frames(seconds));
    float a = 0.f, b = 0.f;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        const float n = random.noise();
        const double high = n - 2.0 * a + b;
        b = a;
        a = n;
        s[i] = static_cast<float>(0.3 * high * std::exp(-t / decay));
    }
    return s;
}

// A clap: three short bursts of noise around 1.2 kHz (a resonator), then a tail.
inline Samples clap(double spacing = 0.011, double tail = 0.08, double seconds = 0.5, uint64_t seed = 4) {
    Random random(seed);
    Samples s(frames(seconds));
    const double r = 0.97, w = 2.0 * std::numbers::pi * 1200.0 / kRate;
    double y1 = 0.0, y2 = 0.0;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double y = random.noise() + 2.0 * r * std::cos(w) * y1 - r * r * y2;
        y2 = y1;
        y1 = y;
        const double mid = 0.06 * y;
        double env = std::exp(-std::max(0.0, t - 2.0 * spacing) / tail);
        for (int burst = 0; burst < 3; ++burst) {
            const double since = t - burst * spacing;
            if (since >= 0.0 && since < spacing) env = std::max(env, std::exp(-since / 0.004));
        }
        s[i] = static_cast<float>(0.7 * mid * env);
    }
    return s;
}

// A tone at `hz` (a few harmonics), dying over `decay`.
inline Samples tone(double hz, double seconds = 1.0, double decay = 0.5, double amplitude = 0.5, int rate = kRate) {
    Samples s(frames(seconds, rate));
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double w = 2.0 * std::numbers::pi * hz * t;
        s[i] = static_cast<float>(amplitude * (std::sin(w) + 0.3 * std::sin(2 * w) + 0.1 * std::sin(3 * w)) / 1.4 *
                                  std::exp(-t / decay));
    }
    return s;
}

// A synth stab: a detuned sawtooth chord (root, fifth, octave) through a
// low-pass filter whose cutoff falls from `cutoffHz` over `filterDecay`, the
// level dying over `decay`.
inline Samples stab(double hz = 220.0, double cutoffHz = 6000.0, double filterDecay = 0.12, double decay = 0.35,
                    double seconds = 0.8) {
    Samples s(frames(seconds));
    const double ratios[] = {1.0, 1.003, 1.5, 2.0};
    double phases[4] = {0.0, 0.25, 0.5, 0.75};
    double low = 0.0, low2 = 0.0;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        double saw = 0.0;
        for (int v = 0; v < 4; ++v) {
            phases[v] += hz * ratios[v] / kRate;
            phases[v] -= std::floor(phases[v]);
            saw += 2.0 * phases[v] - 1.0;
        }
        const double cutoff = 150.0 + cutoffHz * std::exp(-t / filterDecay);
        const double a = 1.0 - std::exp(-2.0 * std::numbers::pi * cutoff / kRate);
        low += a * (saw - low);  // two one-pole low-passes
        low2 += a * (low - low2);
        const double attack = std::min(1.0, t / 0.003);
        s[i] = static_cast<float>(0.15 * low2 * attack * std::exp(-t / decay));
    }
    return s;
}

// A plucked string (Karplus-Strong) at `hz`: a burst of noise in a delay line
// that averages it away, `damping` (0..1) how fast the brightness goes.
inline Samples pluck(double hz = 330.0, double damping = 0.5, double seconds = 1.0, uint64_t seed = 6) {
    Random random(seed);
    Samples s(frames(seconds));
    std::vector<double> line(static_cast<size_t>(kRate / hz), 0.0);
    for (double& x : line) x = random.noise();
    size_t at = 0;
    const double keep = 0.996;
    for (size_t i = 0; i < s.size(); ++i) {
        const size_t next = (at + 1) % line.size();
        const double out = line[at];
        line[at] = keep * ((1.0 - damping * 0.5) * line[at] + damping * 0.5 * line[next]);
        at = next;
        s[i] = static_cast<float>(0.5 * out);
    }
    return s;
}

inline Samples noise(double seconds, uint64_t seed = 5) {
    Random random(seed);
    Samples s(frames(seconds));
    for (float& x : s) x = 0.3f * random.noise();
    return s;
}

// Sounds one after another, every `step` seconds, for `seconds`.
inline Samples sequence(const std::vector<Samples>& hits, double step, double seconds) {
    Samples s(frames(seconds), 0.f);
    size_t n = 0;
    for (double at = 0.0; at < seconds; at += step, ++n) {
        const Samples& hit = hits[n % hits.size()];
        const size_t start = frames(at);
        for (size_t i = 0; i < hit.size() && start + i < s.size(); ++i) s[start + i] += hit[i];
    }
    return s;
}

inline Samples scaled(Samples s, float gain) {
    for (float& x : s) x *= gain;
    return s;
}

inline Samples delayed(const Samples& s, double seconds) {
    Samples out(frames(seconds), 0.f);
    out.insert(out.end(), s.begin(), s.end());
    return out;
}

inline std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

inline std::filesystem::path pathOf(const std::string& utf8Path) {
    return std::filesystem::path(std::u8string(utf8Path.begin(), utf8Path.end()));
}

// Writes 16-bit PCM (interleaved when `channels` > 1). Returns the path as UTF-8.
inline std::string writeWav(const std::filesystem::path& path, const Samples& interleaved, int channels = 1,
                            int rate = kRate) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    const auto count = static_cast<uint32_t>(interleaved.size());
    const uint32_t dataBytes = count * 2;
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(static_cast<uint16_t>(channels));
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate * channels * 2));
    u16(static_cast<uint16_t>(channels * 2));
    u16(16);
    out.write("data", 4);
    u32(dataBytes);
    for (const float x : interleaved) {
        double v = std::round(static_cast<double>(x) * 32767.0);
        v = std::clamp(v, -32768.0, 32767.0);
        u16(static_cast<uint16_t>(static_cast<int16_t>(v)));
    }
    return utf8(path);
}

inline std::string wav(const std::string& name, const Samples& samples, int rate = kRate) {
    return writeWav(tempDir() / name, samples, 1, rate);
}

}  // namespace subtest
