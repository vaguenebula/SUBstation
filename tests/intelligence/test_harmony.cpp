// Harmony: naming chords and keys, estimating a key, inferring chords from
// notes (triads, sevenths, inversions, a bass on another track, melodies over
// chords, arpeggios, rests), and the parts made from chords (block chords that
// move little, a bass line, a progression to start from).

#include <chrono>
#include <initializer_list>
#include <string>
#include <vector>

#include "harmony/Accompaniment.h"
#include "harmony/ChordInference.h"
#include "harmony/Chords.h"
#include "harness/Test.h"

using namespace sub::intelligence::harmony;

namespace {

// Notes sounding together from `start` for `length` beats.
std::vector<Note> block(std::initializer_list<int> pitches, double start, double length, int velocity = 100) {
    std::vector<Note> notes;
    for (const int p : pitches) notes.push_back({p, start, start + length, velocity});
    return notes;
}

void add(std::vector<Note>& to, const std::vector<Note>& notes) { to.insert(to.end(), notes.begin(), notes.end()); }

// "C [0, 4) | G [4, 8)".
std::string spans(const Harmony& harmony) {
    std::string text;
    for (const ChordSpan& span : harmony.chords) {
        if (!text.empty()) text += " | ";
        text += span.chord.name() + " [" + subtest::show(span.start) + ", " + subtest::show(span.end) + ")";
    }
    return text;
}

// "C G Am F".
std::string names(const Harmony& harmony) {
    std::string text;
    for (const ChordSpan& span : harmony.chords) text += (text.empty() ? "" : " ") + span.chord.name();
    return text;
}

}  // namespace

TEST_CASE("chords and keys are named as the application spells keys") {
    const auto name = [](int root, Quality quality, int bass = -1) { return Chord{root, quality, bass}.name(); };
    CHECK_EQ(name(0, Quality::Major), std::string("C"));
    CHECK_EQ(name(9, Quality::Minor7), std::string("Am7"));
    CHECK_EQ(name(0, Quality::Major, 4), std::string("C/E"));
    CHECK_EQ(name(6, Quality::Diminished), std::string("F#dim"));
    CHECK_EQ(name(3, Quality::Major7), std::string("Ebmaj7"));
    CHECK_EQ(name(11, Quality::HalfDiminished7), std::string("Bm7b5"));
    CHECK_EQ(name(7, Quality::Sus4), std::string("Gsus4"));
    const Key aMinor{9, true}, bFlat{10, false};
    CHECK_EQ(aMinor.name(), std::string("A minor"));
    CHECK_EQ(bFlat.name(), std::string("Bb major"));

    const Chord g7{7, Quality::Dominant7}, g{7, Quality::Major}, cOverE{0, Quality::Major, 4}, c{0, Quality::Major};
    CHECK(g7.contains(5));  // G7 has F
    CHECK(!g.contains(5));
    CHECK_EQ(cOverE.bassClass(), 4);
    CHECK_EQ(c.bassClass(), 0);
}

TEST_CASE("a key's scale and its triads") {
    const Key c{0, false}, a{9, true};
    CHECK(c.contains(4));
    CHECK(!c.contains(6));  // F#
    CHECK(a.contains(7));   // G: natural minor
    CHECK(!a.contains(8));  // G#
    CHECK_EQ(c.scale(), a.scale());  // relative keys share their notes
    std::string triads;
    for (int d = 0; d < 7; ++d) triads += c.triad(d).name() + " ";
    CHECK_EQ(triads, std::string("C Dm Em F G Am Bdim "));
    triads.clear();
    for (int d = 0; d < 7; ++d) triads += a.triad(d).name() + " ";
    CHECK_EQ(triads, std::string("Am Bdim C Dm Em F G "));
    const Chord g7{7, Quality::Dominant7}, d{2, Quality::Major};
    CHECK(c.isDiatonic(g7));
    CHECK(!c.isDiatonic(d));  // D has F#
}

TEST_CASE("the key: from how long each pitch class sounds, and the bass; none from too little") {
    std::vector<Note> cadence;  // I IV V I in C
    add(cadence, block({48, 52, 55, 60}, 0, 4));
    add(cadence, block({53, 57, 60, 65}, 4, 4));
    add(cadence, block({55, 59, 62, 67}, 8, 4));
    add(cadence, block({48, 52, 55, 60}, 12, 4));
    CHECK(estimateKey(cadence) == (Key{0, false}));

    std::vector<Note> minor;  // i iv V i in A minor, with its leading note
    add(minor, block({45, 57, 60, 64}, 0, 4));
    add(minor, block({50, 57, 62, 65}, 4, 4));
    add(minor, block({52, 56, 59, 64}, 8, 4));
    add(minor, block({45, 57, 60, 64}, 12, 4));
    CHECK(estimateKey(minor) == (Key{9, true}));

    // C then G: their notes are G major's (and E minor's) too; the bass says C major.
    std::vector<Note> twoChords;
    add(twoChords, block({48, 52, 55}, 0, 6));
    add(twoChords, block({43, 47, 50}, 6, 2));
    CHECK(estimateKey(twoChords) == (Key{0, false}));

    CHECK(!estimateKey({}).has_value());
    CHECK(!estimateKey(block({60, 64}, 0, 8)).has_value());      // two pitch classes
    CHECK(!estimateKey(block({60, 64, 67}, 0, 0.5)).has_value());  // a short chord
}

TEST_CASE("chords: triads and sevenths in root position, where they sound") {
    struct Case {
        std::initializer_list<int> pitches;
        const char* name;
    };
    for (const Case& c : {Case{{48, 52, 55}, "C"}, Case{{45, 57, 60, 64}, "Am"}, Case{{50, 62, 65, 69}, "Dm"},
                          Case{{47, 59, 62, 65}, "Bdim"}, Case{{43, 55, 59, 62, 65}, "G7"},
                          Case{{48, 55, 59, 64}, "Cmaj7"}, Case{{50, 57, 60, 65}, "Dm7"},
                          Case{{47, 57, 62, 65}, "Bm7b5"}, Case{{43, 55, 60, 62}, "Gsus4"},
                          Case{{48, 56, 60, 64}, "Caug"}, Case{{39, 55, 58, 62, 67}, "Ebmaj7"}}) {
        INFO(c.name);
        const Harmony h = inferHarmony(block(c.pitches, 4, 4));
        CHECK_EQ(spans(h), std::string(c.name) + " [4, 8)");
    }
}

TEST_CASE("chords: an inversion's bass names it, as a slash chord") {
    CHECK_EQ(names(inferHarmony(block({40, 55, 60, 64}, 0, 4))), std::string("C/E"));
    CHECK_EQ(names(inferHarmony(block({43, 60, 64, 67}, 0, 4))), std::string("C/G"));
    // Am7 over C: the sixth chord C6 is spelled as its relative minor seventh.
    CHECK_EQ(names(inferHarmony(block({36, 57, 60, 64, 67}, 0, 4))), std::string("Am7/C"));
}

TEST_CASE("chords: a progression with its bass on another track, a chord a bar") {
    std::vector<Note> song;
    const std::vector<std::pair<std::initializer_list<int>, int>> bars{
        {{60, 64, 67}, 36}, {{59, 62, 67}, 43}, {{60, 64, 69}, 45}, {{60, 65, 69}, 41}};
    for (size_t bar = 0; bar < bars.size(); ++bar) {
        add(song, block(bars[bar].first, 4.0 * bar, 4, 90));
        for (int beat = 0; beat < 4; ++beat)
            song.push_back({bars[bar].second, 4.0 * bar + beat, 4.0 * bar + beat + 0.5, 110});
    }
    const Harmony h = inferHarmony(song);
    CHECK_EQ(spans(h), std::string("C [0, 4) | G [4, 8) | Am [8, 12) | F [12, 16)"));
    CHECK(h.key == (Key{0, false}));
}

TEST_CASE("chords: a melody's passing notes don't make chords of their own") {
    std::vector<Note> song;
    add(song, block({48, 55, 64}, 0, 4));
    add(song, block({53, 57, 60}, 4, 4));
    const int melody[] = {72, 74, 76, 77, 79, 77, 76, 74, 72, 71, 72, 74, 76, 74, 72, 71};
    for (int i = 0; i < 16; ++i) song.push_back({melody[i], 0.5 * i, 0.5 * i + 0.5, 100});
    CHECK_EQ(spans(inferHarmony(song)), std::string("C [0, 4) | F [4, 8)"));
}

TEST_CASE("chords: a melody alone is heard as the key's plain triads, without inversions") {
    // Twinkle, Twinkle in C, a quarter note each, the last of each bar held.
    const int melody[] = {60, 60, 67, 67, 69, 69, 67, -1, 65, 65, 64, 64, 62, 62, 60, -1};
    std::vector<Note> song;
    for (int i = 0; i < 16; ++i) {
        if (melody[i] < 0) continue;
        const double length = i + 1 < 16 && melody[i + 1] < 0 ? 2.0 : 1.0;
        song.push_back({melody[i] + 12, double(i), i + length, 100});
    }
    const Harmony h = inferHarmony(song);
    CHECK(h.key == (Key{0, false}));
    CHECK_EQ(names(h), std::string("C F G F C Dm C"));
}

TEST_CASE("chords: a chord played clearly changes where it is played, between beats too") {
    std::vector<Note> song;
    add(song, block({48, 52, 55}, 0, 3.5));
    add(song, block({43, 47, 50}, 3.5, 4.5));  // pushed an eighth ahead of the bar
    CHECK_EQ(spans(inferHarmony(song)), std::string("C [0, 3.5) | G [3.5, 8)"));
}

TEST_CASE("chords: arpeggios are heard as the chords they spell") {
    std::vector<Note> song;
    const std::vector<std::vector<int>> arpeggios{{48, 52, 55, 60}, {45, 48, 52, 57}, {41, 45, 48, 53}, {43, 47, 50, 55}};
    for (size_t bar = 0; bar < arpeggios.size(); ++bar) {
        for (int i = 0; i < 8; ++i) {
            const double at = 4.0 * bar + 0.5 * i;
            song.push_back({arpeggios[bar][static_cast<size_t>(i % 4)], at, at + 0.5, 100});
        }
    }
    CHECK_EQ(names(inferHarmony(song)), std::string("C Am F G"));
}

TEST_CASE("chords: a short rest keeps the chord, a long one has none") {
    std::vector<Note> song;
    add(song, block({48, 52, 55}, 0, 2));
    add(song, block({48, 52, 55}, 3, 1));  // a beat's rest
    add(song, block({43, 47, 50}, 16, 4));  // three bars after
    CHECK_EQ(spans(inferHarmony(song)), std::string("C [0, 4) | G [16, 20)"));
}

TEST_CASE("chords: nothing from no notes; the key given is the key used") {
    const Harmony none = inferHarmony({});
    CHECK(none.chords.empty());
    CHECK(!none.key.has_value());

    InferenceOptions options;
    options.key = Key{2, true};
    const Harmony given = inferHarmony(block({48, 52, 55}, 0, 4), options);
    CHECK(given.key == (Key{2, true}));
    CHECK_EQ(names(given), std::string("C"));
    // Notes of no length, or off the keyboard, are left out.
    const std::vector<Note> unplayable{{60, 2, 2, 100}, {200, 0, 4, 100}};
    CHECK(inferHarmony(unplayable).chords.empty());
}

TEST_CASE("chords: a long song takes next to no time") {
    std::vector<Note> song;
    const std::vector<std::initializer_list<int>> chords{{48, 52, 55}, {43, 47, 50}, {45, 48, 52}, {41, 45, 48}};
    constexpr int kScale[] = {72, 74, 76, 77, 79, 81, 83, 84};
    for (int bar = 0; bar < 400; ++bar) {
        add(song, block(chords[static_cast<size_t>(bar % 4)], 4.0 * bar, 4));
        for (int i = 0; i < 8; ++i) song.push_back({kScale[i], 4.0 * bar + 0.5 * i, 4.0 * bar + 0.5 * i + 0.5, 90});
    }
    const auto start = std::chrono::steady_clock::now();
    const Harmony h = inferHarmony(song);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    INFO("took " + std::to_string(ms) + " ms for " + std::to_string(song.size()) + " notes");
    CHECK(h.chords.size() >= 400);
    CHECK(ms < 250.0);  // (a few ms in a release build)
}

TEST_CASE("the chord part: close voicings in range that move as little as they can") {
    const std::vector<ChordSpan> chords{{0, 4, {0, Quality::Major}},
                                        {4, 8, {7, Quality::Major}},
                                        {8, 12, {9, Quality::Minor}},
                                        {12, 16, {5, Quality::Major}}};
    const std::vector<GeneratedNote> notes = chordPart(chords);
    REQUIRE(notes.size() == 12);
    std::vector<std::vector<int>> voicings(4);
    for (const GeneratedNote& n : notes) {
        CHECK(n.pitch >= kChordLowest && n.pitch <= kChordHighest);
        CHECK_EQ(n.velocity, kChordVelocity);
        CHECK_EQ(n.length, 4.0);
        voicings[static_cast<size_t>(n.start / 4)].push_back(n.pitch);
    }
    CHECK(voicings[0] == (std::vector<int>{60, 64, 67}));  // C3 E3 G3: the first in root position
    for (size_t i = 0; i < 4; ++i) {
        INFO("chord " + std::to_string(i));
        for (const int p : voicings[i]) CHECK(chords[i].chord.contains(pitchClass(p)));
    }
    CHECK(voicings[1] == (std::vector<int>{59, 62, 67}));  // G/B: the common G stays, the rest step down
    CHECK(voicings[2] == (std::vector<int>{60, 64, 69}));
    CHECK(voicings[3] == (std::vector<int>{60, 65, 69}));
}

TEST_CASE("the chord part and the bass: struck again at each bar line; the bass plays an inversion's bass") {
    const std::vector<ChordSpan> chords{{2, 9, {0, Quality::Major, 4}}, {9, 10, {7, Quality::Dominant7}}};
    const std::vector<GeneratedNote> bass = bassPart(chords);
    REQUIRE(bass.size() == 4);
    CHECK(bass[0] == (GeneratedNote{40, 2, 2, kBassVelocity}));  // E1, to the bar line
    CHECK(bass[1] == (GeneratedNote{40, 4, 4, kBassVelocity}));
    CHECK(bass[2] == (GeneratedNote{40, 8, 1, kBassVelocity}));
    CHECK(bass[3] == (GeneratedNote{43, 9, 1, kBassVelocity}));  // G1
    for (const GeneratedNote& n : bass) CHECK(n.pitch >= kBassLowest && n.pitch < kBassLowest + 12);

    const std::vector<GeneratedNote> part = chordPart(chords);
    CHECK_EQ(part.size(), size_t{3 * 3 + 4});  // C three times (2-4, 4-8, 8-9), G7 once
    CHECK_EQ(chordPart({}).size(), size_t{0});
}

TEST_CASE("a progression to start from: a chord a bar, cut at its ends") {
    Harmony major;
    major.chords = starterProgression(Key{0, false}, 2, 14);
    CHECK_EQ(spans(major), std::string("C [2, 4) | G [4, 8) | Am [8, 12) | F [12, 14)"));

    std::string text;

    for (const ChordSpan& s : starterProgression(Key{9, true}, 0, 20)) text += s.chord.name() + " ";
    CHECK_EQ(text, std::string("Am F C G Am "));
    CHECK(starterProgression(Key{}, 4, 4).empty());
}
