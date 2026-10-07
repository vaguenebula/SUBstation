// Fingerprints: the FFT under them, decoding, what each aspect tells apart,
// comparing them, and the store they are saved in.

#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <map>
#include <numbers>

#include "Sounds.h"
#include "core/AudioReader.h"
#include "similarity/Similarity.h"
#include "similarity/SoundFeatures.h"
#include "similarity/SoundStore.h"

using namespace subtest;
using namespace sub::intelligence;

namespace {

Fingerprint fingerprint(const Samples& s, int rate = kRate) {
    SoundAnalyzer analyzer;
    const auto fp = analyzer.analyze(s.data(), s.size(), static_cast<uint32_t>(rate));
    REQUIRE(fp.has_value());
    return *fp;
}

// The distance with the features' own minimum spreads (no library to measure them by).
float distance(const Fingerprint& a, const Fingerprint& b) {
    const Comparison c = Comparison::fit(a.data(), 1);
    return c.distance(a.data(), b.data());
}

}  // namespace

TEST_CASE("the spectrum (Signalsmith Linear's FFT): a sine's brightness is its frequency, at any rate") {
    for (const int rate : {16000, 22050, 44100, 48000}) {
        for (const double hz : {250.0, 1000.0, 4000.0}) {
            INFO(std::to_string(hz) + " Hz at " + std::to_string(rate));
            Samples s(static_cast<size_t>(rate));
            for (size_t i = 0; i < s.size(); ++i)
                s[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
            const Fingerprint fp = fingerprint(s, rate);
            CHECK_NEAR(fp[feature::Centroid], std::log2(hz / 1000.0), 0.03);  // octaves
            CHECK(fp[feature::Flatness] < -20.f);
        }
    }
    // The Nyquist frequency's bin (which the real FFT hands over in bin 0's
    // imaginary part) counts: all the energy is at 8 kHz, so is the roll-off.
    Samples nyquist(16000);
    for (size_t i = 0; i < nyquist.size(); ++i) nyquist[i] = i % 2 ? -0.5f : 0.5f;
    CHECK(fingerprint(nyquist, 16000)[feature::Rolloff] > 2.997f);  // log2(8 kHz / 1 kHz) = 3; 2.994 without it
}

TEST_CASE("audio files are read as mono, from where asked, at no more than 48 kHz") {
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

    // 96 kHz comes as 48 kHz.
    audio = readMono(wav("high.wav", Samples(96000, 0.25f), 96000), 0.0, 10.0);
    CHECK_EQ(audio.sampleRate, 48000u);
    CHECK_NEAR(static_cast<double>(audio.samples.size()), 48000.0, 64.0);

    // Not audio.
    const auto junk = tempDir() / "junk.wav";
    std::ofstream(junk, std::ios::binary) << "this is not a wave file at all";
    bool threw = false;
    try {
        readMono(utf8(junk), 0.0, 1.0);
    } catch (const AudioError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("a fingerprint ignores the level and leading silence") {
    const Fingerprint loud = fingerprint(kick());
    CHECK(distance(loud, fingerprint(scaled(kick(), 0.2f))) < 0.01f);
    // Leading silence is skipped; only the file's length differs.
    const Fingerprint late = fingerprint(delayed(kick(), 0.3));
    Fingerprint a = loud, b = late;
    a[feature::Length] = b[feature::Length] = 0.f;
    CHECK(distance(a, b) < 0.02f);
    CHECK(late[feature::Length] > loud[feature::Length]);
}

TEST_CASE("silence has no fingerprint, and a file that isn't audio can't be analysed") {
    SoundAnalyzer analyzer;
    const Samples silence(frames(0.5), 0.f);
    CHECK(!analyzer.analyze(silence.data(), silence.size(), kRate).has_value());
    CHECK(!analyzer.analyzeFile(wav("silent.wav", silence)).has_value());
    const auto junk = tempDir() / "junk.mp3";
    std::ofstream(junk, std::ios::binary) << "ID3 not really";
    bool threw = false;
    try {
        analyzer.analyzeFile(utf8(junk));
    } catch (const AudioError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("pitch: a tone's is found, noise has none") {
    for (const double hz : {55.0, 110.0, 440.0, 880.0}) {
        INFO(std::to_string(hz) + " Hz");
        const Fingerprint fp = fingerprint(tone(hz));
        const float confidence = fp[feature::PitchConfidence];
        CHECK(confidence > 0.8f);
        CHECK_NEAR(fp[feature::Pitch] / confidence, std::log2(hz / 220.0), 0.02);  // a quarter of a semitone
    }
    CHECK(fingerprint(noise(1.0))[feature::PitchConfidence] < 0.2f);
    CHECK(fingerprint(hat())[feature::PitchConfidence] < 0.2f);
}

TEST_CASE("the envelope: attack, how long it rings, the level after the peak") {
    const Fingerprint closed = fingerprint(hat(0.02));
    const Fingerprint open = fingerprint(hat(0.3, 1.5));
    CHECK(closed[feature::Duration] < open[feature::Duration]);
    CHECK(closed[feature::TemporalCentroid] < open[feature::TemporalCentroid]);
    for (int i = 2; i < 6; ++i) CHECK(closed[feature::Contour + i] < open[feature::Contour + i] - 10.f);
    // A slow swell has a long attack.
    Samples swell = tone(220.0, 1.0, 10.0);
    for (size_t i = 0; i < swell.size(); ++i) swell[i] *= std::min(1.f, static_cast<float>(i) / static_cast<float>(frames(0.4)));
    CHECK(fingerprint(swell)[feature::AttackTime] > fingerprint(tone(220.0))[feature::AttackTime] + 1.f);
    // Past the end of a short file is silence.
    CHECK_NEAR(fingerprint(hat(0.01, 0.1))[feature::Contour + 7], -60.0, 1e-4);
}

TEST_CASE("the spectrum: bright, dark, noisy, sub-bass") {
    const Fingerprint k = fingerprint(kick());
    const Fingerprint h = fingerprint(hat());
    CHECK(k[feature::Centroid] < h[feature::Centroid] - 2.f);  // octaves
    CHECK(k[feature::SubBass] > h[feature::SubBass] + 20.f);  // dB
    CHECK(h[feature::Air] > k[feature::Air] + 20.f);
    CHECK(h[feature::Flatness] > fingerprint(tone(440.0))[feature::Flatness] + 10.f);
}

TEST_CASE("onsets: a loop has them, a one-shot doesn't") {
    const Fingerprint shot = fingerprint(kick());
    const Fingerprint loop = fingerprint(sequence({kick(), hat(), snare(), hat()}, 0.25, 4.0));
    CHECK_NEAR(shot[feature::OnsetRate], 0.0, 1e-6);
    CHECK(loop[feature::OnsetRate] > 1.5f);  // about 4 a second: log2(1 + 4) = 2.3
}

TEST_CASE("each kind of drum finds its own kind nearest") {
    // Five of each, with their parameters varied.
    std::vector<std::pair<std::string, Fingerprint>> sounds;
    Random random(42);
    for (uint64_t i = 0; i < 5; ++i) {
        sounds.push_back({"kick", fingerprint(kick(random.range(110, 180), random.range(42, 60), random.range(0.15, 0.35)))});
        sounds.push_back({"snare", fingerprint(snare(random.range(160, 230), random.range(0.08, 0.16), random.range(0.6, 0.8),
                                                     0.5, 10 + i))});
        sounds.push_back({"hat", fingerprint(hat(random.range(0.02, 0.05), 0.4, 20 + i))});
        sounds.push_back({"clap", fingerprint(clap(random.range(0.009, 0.014), random.range(0.06, 0.1), 0.5, 30 + i))});
    }
    std::vector<float> matrix;
    for (const auto& s : sounds) matrix.insert(matrix.end(), s.second.begin(), s.second.end());
    const Comparison comparison = Comparison::fit(matrix.data(), sounds.size());
    int right = 0, total = 0;
    for (size_t q = 0; q < sounds.size(); ++q) {
        std::vector<std::pair<float, size_t>> nearest;
        for (size_t r = 0; r < sounds.size(); ++r)
            if (r != q) nearest.push_back({comparison.distance(sounds[q].second.data(), sounds[r].second.data()), r});
        std::sort(nearest.begin(), nearest.end());
        for (size_t i = 0; i < 4; ++i, ++total)
            if (sounds[nearest[i].second].first == sounds[q].first) ++right;
    }
    INFO(std::to_string(right) + " of " + std::to_string(total));
    CHECK(right >= total * 95 / 100);
}

TEST_CASE("comparing: the same sound is 1, distance grows with difference, weights pick aspects") {
    const Fingerprint a = fingerprint(kick(150, 50));
    const Fingerprint b = fingerprint(kick(140, 55));
    const Fingerprint c = fingerprint(hat());
    std::vector<float> matrix;
    for (const auto* f : {&a, &b, &c}) matrix.insert(matrix.end(), f->begin(), f->end());
    const Comparison comparison = Comparison::fit(matrix.data(), 3);
    CHECK_EQ(comparison.distance(a.data(), a.data()), 0.f);
    CHECK_EQ(Comparison::similarity(0.f), 1.f);
    CHECK(comparison.distance(a.data(), b.data()) < comparison.distance(a.data(), c.data()));
    CHECK(Comparison::similarity(comparison.distance(a.data(), b.data())) >
          Comparison::similarity(comparison.distance(a.data(), c.data())));
    // A feature's difference counts at most kClip.
    Fingerprint far = a;
    far[feature::Pitch] += 1000.f;
    AspectWeights pitchOnly;
    pitchOnly.weight.fill(0.f);
    pitchOnly[Aspect::Pitch] = 1.f;
    const Comparison onlyPitch = Comparison::fit(matrix.data(), 3, nullptr, pitchOnly);
    CHECK_NEAR(onlyPitch.distance(a.data(), far.data()), Comparison::kClip / 2.0, 1e-4);  // one of the aspect's two features
    // With no weight, an aspect doesn't count.
    AspectWeights noPitch;
    noPitch[Aspect::Pitch] = 0.f;
    const Comparison withoutPitch = Comparison::fit(matrix.data(), 3, nullptr, noPitch);
    CHECK_NEAR(withoutPitch.distance(a.data(), far.data()), 0.0, 1e-6);
}

TEST_CASE("every feature has a name, an aspect and a spread") {
    std::map<std::string, int> names;
    for (const FeatureInfo& f : featureInfo()) {
        REQUIRE(f.name != nullptr);
        CHECK(f.minSpread > 0.f);
        ++names[f.name];
    }
    CHECK_EQ(names.size(), kDims);  // all different
}

TEST_CASE("the store: saved and read back; anything unexpected is ignored") {
    std::vector<StoredSound> sounds(3);
    sounds[0].path = "C:\\Samples\\Kick 01.wav";
    sounds[0].stamp = {12345, 678};
    sounds[0].seen = 1760000000;
    sounds[0].analysed = true;
    for (size_t d = 0; d < kDims; ++d) sounds[0].fingerprint[d] = static_cast<float>(d) * 0.5f - 3.f;
    sounds[1].path = "/samples/broken.wav";
    sounds[1].stamp = {1, 2};
    sounds[2].path = "C:\\Project\\Recorded \xe2\x99\xab.wav";
    sounds[2].analysed = true;
    sounds[2].reference = true;
    sounds[2].used = 99;
    const std::string file = utf8(tempDir() / "sound-index.bin");
    StoreWriter writer(3);
    for (const auto& s : sounds) writer.add(s);
    const std::string bytes = writer.finish();
    REQUIRE(writeStore(file, bytes));
    const auto read = readStore(file);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        CHECK_EQ((*read)[i].path, sounds[i].path);
        CHECK((*read)[i].stamp == sounds[i].stamp);
        CHECK_EQ((*read)[i].analysed, sounds[i].analysed);
        CHECK_EQ((*read)[i].reference, sounds[i].reference);
        CHECK_EQ((*read)[i].used, sounds[i].used);
        CHECK_EQ((*read)[i].seen, sounds[i].seen);
        if (sounds[i].analysed) CHECK((*read)[i].fingerprint == sounds[i].fingerprint);
    }
    // A changed byte, a truncated file, another feature version: ignored.
    auto writeBytes = [&](const std::string& b) { std::ofstream(std::filesystem::path(file), std::ios::binary) << b; };
    std::string damaged = bytes;
    damaged[30] ^= 1;
    writeBytes(damaged);
    CHECK(!readStore(file).has_value());
    writeBytes(bytes.substr(0, bytes.size() - 3));
    CHECK(!readStore(file).has_value());
    auto withChecksum = [](std::string body) {
        uint64_t h = 1469598103934665603ull;  // FNV-1a, as the store sums
        for (const char c : body) {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ull;
        }
        for (int i = 0; i < 8; ++i) body.push_back(static_cast<char>(h >> (8 * i)));
        return body;
    };
    std::string body = bytes.substr(0, bytes.size() - 8);
    writeBytes(withChecksum(body));
    CHECK(readStore(file).has_value());  // (the checksum is made right)
    body[12] = static_cast<char>(kFeatureVersion + 1);  // after the magic (8) and the format (4)
    writeBytes(withChecksum(body));
    CHECK(!readStore(file).has_value());
    CHECK(!readStore(utf8(tempDir() / "missing.bin")).has_value());
}
