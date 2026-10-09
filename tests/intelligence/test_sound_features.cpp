// Fingerprints: decoding and resampling, what Essentia's descriptors tell
// apart, what doesn't count (level, rate, leading silence, length), damaged and
// odd input, comparing them, the library's statistics, and the store.

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>

#include "Sounds.h"
#include "core/AudioReader.h"
#include "core/Hash.h"
#include "similarity/EssentiaExtractor.h"
#include "similarity/Similarity.h"
#include "similarity/SoundStore.h"

using namespace subtest;
using namespace sub::intelligence;

namespace {

using Fingerprint = std::vector<float>;

Fingerprint fingerprint(const Samples& s, int rate = kRate) {
    static EssentiaExtractor extractor;  // (the tests run on one thread)
    Fingerprint fp(kDims);
    const Extraction result = extractor.extract(SoundBuffer{s.data(), s.size(), static_cast<uint32_t>(rate)}, fp.data());
    REQUIRE(result == Extraction::Done);
    for (const float v : fp) REQUIRE(std::isfinite(v));
    return fp;
}

const FeatureSchema& schema() { return essentiaSchema(); }

// The distance with the features' own minimum spreads (no library to measure them by).
float distance(const Fingerprint& a, const Fingerprint& b) {
    const Comparison c(schema(), FeatureStatistics{});
    return c.distance(a.data(), b.data());
}

// Each sound's nearest others (by a comparison fitted to all of them): how many
// of each one's `k` nearest are of its kind.
struct Ranking {
    int right = 0, total = 0;
    std::map<std::string, std::pair<int, int>> byKind;
};

Ranking rank(const std::vector<std::pair<std::string, Fingerprint>>& sounds, size_t k) {
    std::vector<float> matrix;
    for (const auto& s : sounds) matrix.insert(matrix.end(), s.second.begin(), s.second.end());
    const Comparison comparison = Comparison::fit(schema(), matrix.data(), sounds.size());
    Ranking r;
    for (size_t q = 0; q < sounds.size(); ++q) {
        std::vector<std::pair<float, size_t>> nearest;
        for (size_t o = 0; o < sounds.size(); ++o)
            if (o != q) nearest.push_back({comparison.distance(sounds[q].second.data(), sounds[o].second.data()), o});
        std::sort(nearest.begin(), nearest.end());
        for (size_t i = 0; i < k; ++i) {
            const bool same = sounds[nearest[i].second].first == sounds[q].first;
            r.right += same;
            ++r.total;
            r.byKind[sounds[q].first].first += same;
            ++r.byKind[sounds[q].first].second;
        }
    }
    return r;
}

std::string describe(const Ranking& r) {
    std::string text = std::to_string(r.right) + " of " + std::to_string(r.total) + ":";
    for (const auto& [kind, counts] : r.byKind)
        text += " " + kind + " " + std::to_string(counts.first) + "/" + std::to_string(counts.second);
    return text;
}

}  // namespace

TEST_CASE("audio files are read as mono, from where asked, at no more than 48 kHz, or at the rate asked") {
    // Stereo: the channels' mean.
    Samples stereo;
    for (size_t i = 0; i < frames(1.0); ++i) {
        stereo.push_back(0.5f);
        stereo.push_back(0.1f);
    }
    const std::string path = writeWav(tempDir() / "stereo.wav", stereo, 2);
    MonoAudio audio = readMono(path, 0.0, 10.0);
    CHECK_EQ(audio.sampleRate, 44100u);
    CHECK_EQ(audio.samples.size(), frames(1.0));
    CHECK_NEAR(audio.samples[1000], 0.3, 1e-3);
    CHECK_NEAR(audio.fileSeconds, 1.0, 1e-6);
    CHECK(!audio.truncated);

    // A part: 0.25 s from 0.5 s in, of a ramp.
    Samples ramp(frames(1.0));
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / static_cast<float>(ramp.size());
    audio = readMono(wav("ramp.wav", ramp), 0.5, 0.25);
    CHECK_EQ(audio.samples.size(), frames(0.25));
    CHECK_NEAR(audio.samples.front(), 0.5, 1e-3);
    CHECK(audio.truncated);
    CHECK_NEAR(audio.fileSeconds, 1.0, 1e-6);

    // 96 kHz comes as 48 kHz; any rate as the one asked.
    const std::string high = wav("high.wav", Samples(96000, 0.25f), 96000);
    audio = readMono(high, 0.0, 10.0);
    CHECK_EQ(audio.sampleRate, 48000u);
    CHECK_NEAR(static_cast<double>(audio.samples.size()), 48000.0, 64.0);
    audio = readMonoAt(high, 0.0, 10.0, 44100);
    CHECK_EQ(audio.sampleRate, 44100u);
    CHECK_NEAR(static_cast<double>(audio.samples.size()), 44100.0, 64.0);
    CHECK_NEAR(audio.fileSeconds, 1.0, 1e-3);
    audio = readMonoAt(wav("low.wav", tone(440.0, 1.0, 10.0, 0.5, 22050), 22050), 0.0, 10.0, 44100);
    CHECK_EQ(audio.sampleRate, 44100u);
    CHECK_NEAR(static_cast<double>(audio.samples.size()), 44100.0, 64.0);

    // Not audio.
    const auto junk = tempDir() / "junk.wav";
    std::ofstream(junk, std::ios::binary) << "this is not a wave file at all";
    CHECK_THROWS_AS(readMono(utf8(junk), 0.0, 1.0), AudioError);
    CHECK_THROWS_AS(readMonoAt(utf8(junk), 0.0, 1.0, 44100), AudioError);
}

TEST_CASE("resampling keeps a sound's pitch, length, timing and brightness") {
    // A 1 kHz tone starting 0.1 s in, at 22.05 kHz, brought to 44.1 kHz.
    Samples in(frames(0.5, 22050), 0.f);
    for (size_t i = frames(0.1, 22050); i < in.size(); ++i)
        in[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / 22050.0));
    const std::vector<float> out = resampleMono(in.data(), in.size(), 22050, 44100);
    CHECK_EQ(out.size(), in.size() * 2);
    size_t first = 0;
    while (first < out.size() && std::fabs(out[first]) < 0.05f) ++first;
    CHECK_NEAR(static_cast<double>(first), 4410.0, 2.0);  // (no delay)
    size_t crossings = 0;
    for (size_t i = frames(0.2) + 1; i < frames(0.4); ++i) crossings += (out[i - 1] < 0.f) != (out[i] < 0.f);
    CHECK_NEAR(static_cast<double>(crossings), 400.0, 2.0);  // 0.2 s of 1 kHz
    // Flat to the top of what is analysed: noise taken to 48 kHz and back is
    // as bright, its share above 8 kHz the same to a tenth of a dB.
    const Samples hiss = noise(1.0);
    const std::vector<float> up = resampleMono(hiss.data(), hiss.size(), kRate, 48000);
    const Samples back = resampleMono(up.data(), up.size(), 48000, kRate);
    REQUIRE(back.size() == hiss.size());
    const Fingerprint before = fingerprint(hiss), after = fingerprint(back);
    CHECK_NEAR(after[feature::Air], before[feature::Air], 0.1);
    CHECK_NEAR(after[feature::Centroid], before[feature::Centroid], 0.01);
    CHECK_NEAR(after[feature::Rolloff], before[feature::Rolloff], 0.01);
    // The same rate: a copy.
    CHECK(resampleMono(in.data(), in.size(), 22050, 22050) == std::vector<float>(in.begin(), in.end()));
    // From common rates (each phase's filter made once) and odd ones (made
    // for each sample), a tone comes out as the tone, to -60 dB.
    for (const uint32_t from : {48000u, 192000u, 8000u, 44101u, 30001u}) {
        INFO(std::to_string(from));
        Samples sine(from / 2);
        for (size_t i = 0; i < sine.size(); ++i)
            sine[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / from));
        const std::vector<float> at = resampleMono(sine.data(), sine.size(), from, kRate);
        CHECK_NEAR(static_cast<double>(at.size()), kRate / 2.0, 1.0);
        double worst = 0.0;
        for (size_t j = 2000; j + 2000 < at.size(); ++j)
            worst = std::max(worst, std::fabs(at[j] - 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(j) / kRate)));
        CHECK(worst < 5e-4);
    }
    // Cancelled: nothing.
    const std::atomic<bool> stop{true};
    CHECK(resampleMono(hiss.data(), hiss.size(), kRate, 48000, &stop).empty());
}

TEST_CASE("Essentia's spectrum: a sine's brightness is its frequency, at any rate") {
    for (const int rate : {16000, 22050, 44100, 48000, 96000}) {
        for (const double hz : {250.0, 1000.0, 4000.0}) {
            INFO(std::to_string(hz) + " Hz at " + std::to_string(rate));
            Samples s(static_cast<size_t>(rate));
            for (size_t i = 0; i < s.size(); ++i)
                s[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
            const Fingerprint fp = fingerprint(s, rate);
            CHECK_NEAR(fp[feature::Centroid], std::log2(hz / 1000.0), 0.03);  // octaves
            CHECK_NEAR(fp[feature::Rolloff], std::log2(hz / 1000.0), 0.06);
            CHECK(fp[feature::Flatness] < -20.f);
        }
    }
}

TEST_CASE("a fingerprint ignores the level and leading silence") {
    const Fingerprint loud = fingerprint(kick());
    CHECK(distance(loud, fingerprint(scaled(kick(), 0.2f))) < 0.01f);
    CHECK(distance(loud, fingerprint(scaled(kick(), 0.001f))) < 0.01f);  // (-60 dB)
    // Leading silence is skipped; only the file's length differs.
    const Fingerprint late = fingerprint(delayed(kick(), 0.3));
    Fingerprint a = loud, b = late;
    a[feature::Length] = b[feature::Length] = 0.f;
    CHECK(distance(a, b) < 0.02f);
    CHECK(late[feature::Length] > loud[feature::Length]);
}

TEST_CASE("a sound's sample rate doesn't change its fingerprint") {
    // A kick and a tone made at four rates, and a snare recorded at 44.1 kHz
    // and resampled to 48 and 96 kHz (each analysed at 44.1 kHz), among other
    // sounds: in the scale of them all, each sound's copies are a fraction as
    // far from each other as from anything else. (Not a snare made at each
    // rate: its noise's colour is set per sample, so it is another sound at
    // another rate; and at 22 kHz there is nothing above 11 kHz to hear.)
    std::vector<std::pair<std::string, Fingerprint>> sounds;
    for (const int rate : {22050, 44100, 48000, 96000}) {
        sounds.push_back({"kick", fingerprint(kick(150, 50, 0.25, 0.6, 1, rate), rate)});
        sounds.push_back({"tone", fingerprint(tone(220.0, 1.0, 0.5, 0.5, rate), rate)});
    }
    const Samples recorded = snare();
    sounds.push_back({"snare", fingerprint(recorded)});
    for (const int rate : {48000, 96000})
        sounds.push_back({"snare", fingerprint(resampleMono(recorded.data(), recorded.size(), kRate, rate), rate)});
    const size_t copies = sounds.size();
    for (const Samples& other : {hat(), clap(), hat(0.3, 1.5), stab(), pluck(), snare(230, 0.08, 0.6, 0.5, 7)})
        sounds.push_back({"other", fingerprint(other)});
    std::vector<float> matrix;
    for (const auto& s : sounds) matrix.insert(matrix.end(), s.second.begin(), s.second.end());
    const Comparison c = Comparison::fit(schema(), matrix.data(), sounds.size());
    for (size_t a = 0; a < copies; ++a) {
        INFO(sounds[a].first + " #" + std::to_string(a));
        float same = 0.f, other = 1e9f;
        for (size_t b = 0; b < sounds.size(); ++b) {
            if (b == a) continue;
            const float d = c.distance(sounds[a].second.data(), sounds[b].second.data());
            if (sounds[b].first == sounds[a].first) same = std::max(same, d);
            else other = std::min(other, d);
        }
        INFO("copies " + std::to_string(same) + " apart, the nearest other " + std::to_string(other));
        CHECK(same < 0.2f);
        CHECK(same < other / 4.f);
    }
}

TEST_CASE("silence has no fingerprint, and a file that isn't audio can't be analysed") {
    EssentiaExtractor extractor;
    std::vector<float> fp(kDims);
    const Samples silence(frames(0.5), 0.f);
    CHECK(extractor.extract(SoundBuffer{silence.data(), silence.size(), kRate}, fp.data()) == Extraction::Silent);
    const Samples hiss(frames(0.5), 1e-7f);  // (-140 dB)
    CHECK(extractor.extract(SoundBuffer{hiss.data(), hiss.size(), kRate}, fp.data()) == Extraction::Silent);
    CHECK(extractor.extract(SoundBuffer{}, fp.data()) == Extraction::Silent);
    CHECK(extractor.extractFile(wav("silent.wav", silence)).empty());
    for (const std::string name : {"junk.mp3", "junk.wav", "empty.wav"}) {
        INFO(name);
        const auto junk = tempDir() / name;
        std::ofstream(junk, std::ios::binary) << (name == "empty.wav" ? "" : "ID3 not really");
        CHECK_THROWS_AS(extractor.extractFile(utf8(junk)), AudioError);
    }
    // A WAV cut short (its header says there is more): what is there is analysed.
    const std::string whole = wav("whole.wav", kick());
    std::string bytes;
    {
        std::ifstream in(pathOf(whole), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    std::ofstream(tempDir() / "cut.wav", std::ios::binary) << bytes.substr(0, bytes.size() / 3);
    CHECK(extractor.extractFile(utf8(tempDir() / "cut.wav")).size() == kDims);
}

TEST_CASE("very short sounds, odd rates and broken samples are analysed safely") {
    EssentiaExtractor extractor;
    std::vector<float> fp(kDims);
    // A click of three samples, 5 ms of noise, one sample.
    for (const Samples& s : {Samples{0.5f, -0.5f, 0.25f}, noise(0.005), Samples{0.9f}}) {
        CHECK(extractor.extract(SoundBuffer{s.data(), s.size(), kRate}, fp.data()) == Extraction::Done);
        for (const float v : fp) CHECK(std::isfinite(v));
    }
    // A 4 ms click is still near clicks, and a short hat near hats.
    CHECK(distance(fingerprint(hat(0.002, 0.004)), fingerprint(hat(0.002, 0.006, 9))) <
          distance(fingerprint(hat(0.002, 0.004)), fingerprint(kick())));
    // Low and odd rates.
    for (const int rate : {8000, 11025, 32000, 192000}) {
        INFO(std::to_string(rate));
        const Samples s = kick(150, 50, 0.25, 0.6, 1, rate);
        CHECK(extractor.extract(SoundBuffer{s.data(), s.size(), static_cast<uint32_t>(rate)}, fp.data()) == Extraction::Done);
    }
    const Samples k = kick();
    CHECK_THROWS_AS(extractor.extract(SoundBuffer{k.data(), k.size(), 100}, fp.data()), AudioError);
    // NaN and infinity among the samples (a broken buffer) count as silence.
    Samples broken = snare();
    broken[100] = std::numeric_limits<float>::quiet_NaN();
    broken[200] = std::numeric_limits<float>::infinity();
    broken[300] = -std::numeric_limits<float>::infinity();
    CHECK(extractor.extract(SoundBuffer{broken.data(), broken.size(), kRate}, fp.data()) == Extraction::Done);
    for (const float v : fp) CHECK(std::isfinite(v));
    CHECK(distance(fp, fingerprint(snare())) < 0.05f);
}

TEST_CASE("a part of a file is as long as asked, or as what is left of the file") {
    EssentiaExtractor extractor;
    const std::string path = wav("part.wav", tone(330.0, 2.0, 10.0));
    // 10 s asked from 1 s into a 2 s file: the second that is there.
    const std::vector<float> asked = extractor.extractFile(path, 1.0, 10.0);
    REQUIRE(asked.size() == kDims);
    CHECK(asked == extractor.extractFile(path, 1.0, 1.0));
    CHECK_NEAR(asked[feature::Length], 0.0, 0.01);  // (log10 of 1 s)
    CHECK_NEAR(extractor.extractFile(path, 0.5, 0.25)[feature::Length], std::log10(0.25), 0.01);
}

TEST_CASE("pitch: a tone's is found, noise has none") {
    for (const double hz : {55.0, 110.0, 440.0, 880.0}) {
        INFO(std::to_string(hz) + " Hz");
        const Fingerprint fp = fingerprint(tone(hz));
        const float confidence = fp[feature::PitchConfidence];
        CHECK(confidence > 0.75f);
        CHECK_NEAR(fp[feature::Pitch] / confidence, std::log2(hz / 220.0), 0.02);  // a quarter of a semitone
    }
    CHECK(fingerprint(noise(1.0))[feature::PitchConfidence] < 0.2f);
    CHECK(fingerprint(hat())[feature::PitchConfidence] < 0.2f);
    // An 808 (its pitch falling fast from 120 Hz to 41 Hz, then ringing) is
    // heard at its tail's pitch, within a semitone.
    const Fingerprint boom = fingerprint(kick(120, 41.2, 0.6, 1.5));
    CHECK(boom[feature::PitchConfidence] > 0.6f);
    CHECK_NEAR(boom[feature::Pitch] / boom[feature::PitchConfidence], std::log2(41.2 / 220.0), 1.0 / 12.0);
}

TEST_CASE("the envelope: attack, how long it rings, the level after the peak") {
    const Fingerprint closed = fingerprint(hat(0.02));
    const Fingerprint open = fingerprint(hat(0.3, 1.5));
    CHECK(closed[feature::Duration] < open[feature::Duration]);
    CHECK(closed[feature::TemporalCentroid] < open[feature::TemporalCentroid]);
    for (int i = 2; i < 6; ++i) CHECK(closed[feature::Contour + i] < open[feature::Contour + i] - 10.f);
    // A slow swell has a long attack; a hit, one under a block (2 ms).
    Samples swell = tone(220.0, 1.0, 10.0);
    for (size_t i = 0; i < swell.size(); ++i) swell[i] *= std::min(1.f, static_cast<float>(i) / static_cast<float>(frames(0.4)));
    CHECK(fingerprint(swell)[feature::AttackTime] > fingerprint(tone(220.0))[feature::AttackTime] + 1.f);
    CHECK(fingerprint(snare())[feature::AttackTime] < -2.5f);
    // Past the end of a short file is silence.
    CHECK_NEAR(fingerprint(hat(0.01, 0.1))[feature::Contour + 7], -60.0, 1e-4);
}

TEST_CASE("the spectrum: bright, dark, noisy, sub-bass, and the attack's own") {
    const Fingerprint k = fingerprint(kick());
    const Fingerprint h = fingerprint(hat());
    CHECK(k[feature::Centroid] < h[feature::Centroid] - 2.f);  // octaves
    CHECK(k[feature::Bandwidth] < h[feature::Bandwidth]);
    CHECK(k[feature::Rolloff] < h[feature::Rolloff] - 2.f);
    CHECK(k[feature::SubBass] > h[feature::SubBass] + 20.f);  // dB
    CHECK(h[feature::Air] > k[feature::Air] + 20.f);
    CHECK(h[feature::Flatness] > fingerprint(tone(440.0))[feature::Flatness] + 10.f);
    // A kick's click: its attack is brighter and noisier than its body.
    CHECK(k[feature::AttackCentroid] > k[feature::Centroid]);
    CHECK(k[feature::AttackFlatness] > k[feature::Flatness]);
}

TEST_CASE("the spectrum's peaks, contrast, shape and change: a tone, noise, drums and a loop") {
    const Fingerprint toneFp = fingerprint(tone(440.0, 1.0, 10.0));
    const Fingerprint steady = fingerprint(tone(220.0, 2.0, 100.0));
    const Fingerprint noiseFp = fingerprint(noise(1.0));
    const Fingerprint k = fingerprint(kick());
    const Fingerprint h = fingerprint(hat());
    const Fingerprint s = fingerprint(stab());
    const Fingerprint loop = fingerprint(sequence({kick(), hat(), snare(), hat()}, 0.25, 4.0));
    // A tone is a few peaks standing out of nothing; noise, many, filling the valleys.
    CHECK(noiseFp[feature::Complexity] > toneFp[feature::Complexity] + 2.f);
    CHECK(h[feature::Complexity] > k[feature::Complexity] + 2.f);
    CHECK(toneFp[feature::Crest] > noiseFp[feature::Crest] + 1.f);
    CHECK(toneFp[feature::Contrast + 2] > noiseFp[feature::Contrast + 2] + 0.2f);
    CHECK(noiseFp[feature::Valley + 2] > toneFp[feature::Valley + 2] + 2.f);
    // Peaks close enough to beat are dissonant: noise's and a detuned stab's; a tone's harmonics aren't.
    CHECK(noiseFp[feature::Dissonance] > toneFp[feature::Dissonance] + 0.1f);
    CHECK(s[feature::Dissonance] > toneFp[feature::Dissonance] + 0.1f);
    // Harmonics repeat in the spectrum; a kick's lone low partial doesn't.
    CHECK(toneFp[feature::Salience] > k[feature::Salience] + 0.2f);
    // A hat crosses zero far more often than a kick; noise's spectrum changes
    // from frame to frame, a steady tone's doesn't.
    CHECK(h[feature::ZeroCrossings] > k[feature::ZeroCrossings] + 4.f);
    CHECK(noiseFp[feature::Flux] > 10.f * steady[feature::Flux]);
    // (A sound's first frame has nothing before it to change from: a click
    // shorter than a frame has no flux.)
    CHECK_EQ(fingerprint(noise(0.005))[feature::Flux], 0.f);
    // Peaks are counted across the band: partials all above 5 kHz (a cymbal's) are many.
    Samples high(frames(0.5), 0.f);
    for (int p = 0; p < 40; ++p)
        for (size_t i = 0; i < high.size(); ++i)
            high[i] += static_cast<float>(0.02 * std::sin(2.0 * std::numbers::pi * (6000.0 + 223.0 * p) * static_cast<double>(i) / kRate + p));
    CHECK(fingerprint(high)[feature::Complexity] > toneFp[feature::Complexity] + 2.f);
    // A loop's timbre moves from hit to hit; a steady tone's stays.
    CHECK(loop[feature::MfccSpread] > 5.f * steady[feature::MfccSpread]);
}

TEST_CASE("onsets: a loop has them, a one-shot doesn't") {
    const Fingerprint shot = fingerprint(kick());
    const Fingerprint loop = fingerprint(sequence({kick(), hat(), snare(), hat()}, 0.25, 4.0));
    CHECK_NEAR(shot[feature::OnsetRate], 0.0, 1e-6);
    CHECK_NEAR(fingerprint(stab())[feature::OnsetRate], 0.0, 1e-6);
    CHECK(loop[feature::OnsetRate] > 1.5f);  // about 4 a second: log2(1 + 4) = 2.3
}

TEST_CASE("kicks, snares, hats, claps, synth one-shots and loops each find their own kind nearest") {
    // Five of each kind, their parameters varied.
    std::vector<std::pair<std::string, Fingerprint>> sounds;
    Random random(42);
    for (uint64_t i = 0; i < 5; ++i) {
        sounds.push_back({"kick", fingerprint(kick(random.range(110, 180), random.range(42, 60), random.range(0.15, 0.35)))});
        sounds.push_back({"snare", fingerprint(snare(random.range(160, 230), random.range(0.08, 0.16), random.range(0.6, 0.8),
                                                     0.5, 10 + i))});
        sounds.push_back({"hat", fingerprint(hat(random.range(0.02, 0.05), 0.4, 20 + i))});
        sounds.push_back({"clap", fingerprint(clap(random.range(0.009, 0.014), random.range(0.06, 0.1), 0.5, 30 + i))});
        sounds.push_back({"stab", fingerprint(stab(random.range(150, 300), random.range(4000, 8000), random.range(0.08, 0.16),
                                                   random.range(0.25, 0.45)))});
        sounds.push_back({"pluck", fingerprint(pluck(random.range(200, 500), random.range(0.4, 0.6), 1.0, 40 + i))});
        const double step = random.range(0.2, 0.3);  // (110 to 150 BPM, in eighths)
        sounds.push_back({"drum loop", fingerprint(sequence({kick(150, 50, 0.2, 0.3, 50 + i), hat(0.03, 0.2, 60 + i),
                                                             snare(190, 0.1, 0.7, 0.3, 70 + i), hat(0.03, 0.2, 80 + i)},
                                                            step, 4.0))});
    }
    const Ranking r = rank(sounds, 4);
    INFO(describe(r));
    CHECK(r.right >= r.total * 95 / 100);
    for (const auto& [kind, counts] : r.byKind) {
        INFO(kind);
        CHECK(counts.first >= counts.second * 85 / 100);
    }
}

TEST_CASE("level and length don't outweigh how a sound sounds") {
    // Kicks of every length and level are nearer each other than a kick is to
    // a snare of its own length and level.
    const Fingerprint shortKick = fingerprint(scaled(kick(150, 50, 0.25, 0.25), 0.05f));
    const Fingerprint longKick = fingerprint(kick(140, 52, 0.3, 2.0));
    const Fingerprint shortSnare = fingerprint(scaled(snare(190, 0.12, 0.7, 0.25), 0.05f));
    const Fingerprint longSnare = fingerprint(snare(190, 0.12, 0.7, 2.0));
    std::vector<float> matrix;
    for (const auto* f : {&shortKick, &longKick, &shortSnare, &longSnare}) matrix.insert(matrix.end(), f->begin(), f->end());
    const Comparison c = Comparison::fit(schema(), matrix.data(), 4);
    CHECK(c.distance(shortKick.data(), longKick.data()) < c.distance(shortKick.data(), shortSnare.data()));
    CHECK(c.distance(longKick.data(), shortKick.data()) < c.distance(longKick.data(), longSnare.data()));
    CHECK(c.distance(shortSnare.data(), longSnare.data()) < c.distance(shortSnare.data(), shortKick.data()));
}

TEST_CASE("extraction can be cancelled") {
    EssentiaExtractor extractor;
    std::vector<float> fp(kDims);
    const Samples loop = sequence({kick(), hat(), snare(), hat()}, 0.25, 6.0);
    CancelFlag cancel{true};
    CHECK(extractor.extract(SoundBuffer{loop.data(), loop.size(), kRate}, fp.data(), &cancel) == Extraction::Cancelled);
    CHECK(extractor.extractFile(wav("loop.wav", loop), 0.0, -1.0, fp.data(), &cancel) == Extraction::Cancelled);
    cancel = false;
    CHECK(extractor.extract(SoundBuffer{loop.data(), loop.size(), kRate}, fp.data(), &cancel) == Extraction::Done);
}

TEST_CASE("comparing: the same sound is 1, distance grows with difference, weights pick aspects") {
    const Fingerprint a = fingerprint(kick(150, 50));
    const Fingerprint b = fingerprint(kick(140, 55));
    const Fingerprint c = fingerprint(hat());
    std::vector<float> matrix;
    for (const auto* f : {&a, &b, &c}) matrix.insert(matrix.end(), f->begin(), f->end());
    const Comparison comparison = Comparison::fit(schema(), matrix.data(), 3);
    CHECK_EQ(comparison.distance(a.data(), a.data()), 0.f);
    CHECK_EQ(Comparison::similarity(0.f), 1.f);
    CHECK(comparison.distance(a.data(), b.data()) < comparison.distance(a.data(), c.data()));
    CHECK(Comparison::similarity(comparison.distance(a.data(), b.data())) >
          Comparison::similarity(comparison.distance(a.data(), c.data())));
    // A feature's difference counts at most kClip.
    Fingerprint far = a;
    far[feature::Pitch] += 1000.f;
    const Comparison onlyPitch = Comparison::fit(schema(), matrix.data(), 3, nullptr, AspectWeights::only(Aspect::Pitch));
    CHECK_NEAR(onlyPitch.distance(a.data(), far.data()), Comparison::kClip / 2.0, 1e-4);  // one of the aspect's two features
    // With no weight, an aspect doesn't count.
    AspectWeights noPitch = schema().weights;
    noPitch[Aspect::Pitch] = 0.f;
    const Comparison withoutPitch = Comparison::fit(schema(), matrix.data(), 3, nullptr, noPitch);
    CHECK_NEAR(withoutPitch.distance(a.data(), far.data()), 0.0, 1e-6);
}

TEST_CASE("the library's statistics: robust to a few wild files") {
    // One feature: 1000 values spread evenly over [0, 1), and the same with
    // five wild ones. Held at the 1st and 99th percentiles, the wild ones
    // hardly move the spread (plain, it would grow tenfold).
    FeatureSchema one;
    one.features = {{"x", Aspect::Timbre, 1e-3f}};
    std::vector<float> values(1000);
    for (size_t i = 0; i < values.size(); ++i) values[i] = static_cast<float>(i) / 1000.f;
    const FeatureStatistics tame = FeatureStatistics::measure(one, values.data(), values.size());
    for (size_t i = 0; i < 5; ++i) values[i * 100] = 1000.f;
    const FeatureStatistics wild = FeatureStatistics::measure(one, values.data(), values.size());
    CHECK_EQ(tame.count, 1000u);
    CHECK_NEAR(tame.spread[0], 0.2887, 0.01);  // (uniform: 1 / sqrt(12))
    CHECK_NEAR(wild.spread[0], tame.spread[0], 0.02);
    // Fewer than two: no spread but the feature's least.
    const FeatureStatistics lone = FeatureStatistics::measure(one, values.data(), 1);
    CHECK_EQ(lone.spread[0], 1e-3f);
    // Rows pick fingerprints.
    const std::vector<uint32_t> rows = {1, 2, 3};
    CHECK_EQ(FeatureStatistics::measure(one, values.data(), values.size(), &rows).count, 3u);
}

TEST_CASE("every feature has a name, an aspect and a spread; the schema says what made them") {
    std::map<std::string, int> names;
    for (const FeatureInfo& f : featureInfo()) {
        CHECK(!f.name.empty());
        CHECK(f.minSpread > 0.f);
        CHECK(f.aspect != Aspect::Embedding);
        ++names[f.name];
    }
    CHECK_EQ(names.size(), kDims);  // all different
    CHECK_EQ(schema().dims(), kDims);
    CHECK_EQ(schema().extractor, std::string("essentia"));
    CHECK(schema().settings.find("essentia=2.1-beta5") != std::string::npos);
    // The key changes with anything that changes what fingerprints hold.
    FeatureSchema other = schema();
    CHECK_EQ(other.key(), schema().key());
    other.version += 1;
    CHECK(other.key() != schema().key());
    other = schema();
    other.settings += " frame=2048";
    CHECK(other.key() != schema().key());
    other = schema();
    other.features[3].name = "renamed";
    CHECK(other.key() != schema().key());
}

TEST_CASE("the store: saved and read back; another extractor's, or anything unexpected, is ignored") {
    std::vector<StoredSound> sounds(3);
    sounds[0].path = "C:\\Samples\\Kick 01.wav";
    sounds[0].stamp = {12345, 678};
    sounds[0].seen = 1760000000;
    sounds[0].analysed = true;
    sounds[0].fingerprint.resize(kDims);
    for (size_t d = 0; d < kDims; ++d) sounds[0].fingerprint[d] = static_cast<float>(d) * 0.5f - 3.f;
    sounds[1].path = "/samples/broken.wav";
    sounds[1].stamp = {1, 2};
    sounds[2].path = "C:\\Project\\Recorded \xe2\x99\xab.wav";
    sounds[2].analysed = true;
    sounds[2].reference = true;
    sounds[2].used = 99;
    sounds[2].fingerprint.assign(kDims, 0.25f);
    FeatureStatistics statistics;
    statistics.count = 2;
    statistics.spread.assign(kDims, 2.f);
    const std::string file = utf8(tempDir() / "sound-index.bin");
    StoreWriter writer(schema(), 3);
    for (const auto& s : sounds) writer.add(s);
    const std::string bytes = writer.finish(&statistics);
    REQUIRE(writeStore(file, bytes));
    const auto read = readStore(file, schema());
    REQUIRE(read.has_value());
    REQUIRE(read->sounds.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        const StoredSound& s = read->sounds[i];
        CHECK_EQ(s.path, sounds[i].path);
        CHECK(s.stamp == sounds[i].stamp);
        CHECK_EQ(s.analysed, sounds[i].analysed);
        CHECK_EQ(s.reference, sounds[i].reference);
        CHECK_EQ(s.used, sounds[i].used);
        CHECK_EQ(s.seen, sounds[i].seen);
        CHECK(s.fingerprint == sounds[i].fingerprint);
    }
    REQUIRE(read->statistics.has_value());
    CHECK_EQ(read->statistics->count, 2u);
    CHECK(read->statistics->spread == statistics.spread);

    // Without statistics.
    StoreWriter plain(schema(), 0);
    REQUIRE(writeStore(file, plain.finish()));
    REQUIRE(readStore(file, schema()).has_value());
    CHECK(!readStore(file, schema())->statistics.has_value());

    // Another extractor, version or setting: ignored.
    REQUIRE(writeStore(file, bytes));
    for (int change = 0; change < 3; ++change) {
        FeatureSchema other = schema();
        if (change == 0) other.extractor = "embedding";
        if (change == 1) other.version += 1;
        if (change == 2) other.settings = "rate=48000";
        CHECK(!readStore(file, other).has_value());
    }
    // A changed byte, a truncated file, another format: ignored.
    auto writeBytes = [&](const std::string& b) { std::ofstream(std::filesystem::path(file), std::ios::binary) << b; };
    std::string damaged = bytes;
    damaged[40] ^= 1;
    writeBytes(damaged);
    CHECK(!readStore(file, schema()).has_value());
    writeBytes(bytes.substr(0, bytes.size() - 3));
    CHECK(!readStore(file, schema()).has_value());
    auto withChecksum = [](std::string body) {
        const uint64_t h = fnv1a(body);  // (as the store sums)
        for (int i = 0; i < 8; ++i) body.push_back(static_cast<char>(h >> (8 * i)));
        return body;
    };
    std::string body = bytes.substr(0, bytes.size() - 8);
    writeBytes(withChecksum(body));
    CHECK(readStore(file, schema()).has_value());  // (the checksum is made right)
    body[8] = 2;                                   // the format, after the magic: the previous one
    writeBytes(withChecksum(body));
    CHECK(!readStore(file, schema()).has_value());
    CHECK(!readStore(utf8(tempDir() / "missing.bin"), schema()).has_value());

    // An analysed sound's fingerprint must be the schema's size.
    StoreWriter strict(schema(), 1);
    StoredSound wrong = sounds[0];
    wrong.fingerprint.resize(kDims - 1);
    CHECK_THROWS_AS(strict.add(wrong), std::invalid_argument);
    wrong.fingerprint.clear();
    CHECK_THROWS_AS(strict.add(wrong), std::invalid_argument);
    CHECK_THROWS_AS(strict.add(wrong, nullptr), std::invalid_argument);
    wrong.analysed = false;  // (one that couldn't be analysed has none)
    strict.add(wrong);
}
