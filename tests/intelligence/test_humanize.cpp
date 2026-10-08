// Humanizing velocities with the model the application ships (models/velocity.hbm):
// loading it (and refusing what isn't one), what it predicts depending only on
// the notes (not their velocities, nor how many whole bars into the song they
// are), the targets keeping their level while the context stays as it is, the
// amount, a melody over chords, and parts too small to say much about.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "harness/Test.h"
#include "humanize/VelocityModel.h"

using namespace sub::intelligence::humanize;

namespace {

const VelocityModel& model() {
    static const VelocityModel loaded(SUBSTATION_VELOCITY_MODEL);
    return loaded;
}

// Four bars of 4/4: a C, F, G, C chord (an octave below middle C) on every
// beat, and a melody of eighths above it, every note at `velocity`.
std::vector<Note> pianoPiece(int velocity = 100, double from = 0.0) {
    std::vector<Note> notes;
    const int chords[4][3] = {{48, 52, 55}, {48, 53, 57}, {47, 50, 55}, {48, 52, 55}};
    const int melody[8] = {72, 74, 76, 79, 77, 76, 74, 72};
    for (int bar = 0; bar < 4; ++bar) {
        for (int beat = 0; beat < 4; ++beat) {
            for (const int pitch : chords[bar]) notes.push_back({pitch, from + bar * 4 + beat, 0.9, velocity});
        }
        for (int eighth = 0; eighth < 8; ++eighth)
            notes.push_back({melody[(eighth + bar) % 8], from + bar * 4 + eighth * 0.5, 0.45, velocity});
    }
    return notes;
}

std::vector<Note> allTargets(std::vector<Note> notes) {
    for (Note& note : notes) note.target = true;
    return notes;
}

double mean(const std::vector<int>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

}  // namespace

TEST_CASE("the velocity model loads, and a file that isn't one is refused") {
    CHECK(model().predict(pianoPiece(), Meter{}).size() == pianoPiece().size());
    CHECK_THROWS_MATCHING(VelocityModel((subtest::tempDir() / "missing.hbm").string()), ModelError, "open");
    const auto junk = subtest::tempDir() / "junk.hbm";
    std::ofstream(junk, std::ios::binary) << "not a model at all";
    CHECK_THROWS_MATCHING(VelocityModel(junk.string()), ModelError, "HUMANBRO");
    const auto truncated = subtest::tempDir() / "truncated.hbm";
    {
        std::ifstream in(SUBSTATION_VELOCITY_MODEL, std::ios::binary);
        std::vector<char> head(4096);
        in.read(head.data(), static_cast<std::streamsize>(head.size()));
        std::ofstream(truncated, std::ios::binary).write(head.data(), static_cast<std::streamsize>(head.size()));
    }
    CHECK_THROWS_MATCHING(VelocityModel(truncated.string()), ModelError, "truncated");
}

TEST_CASE("what the model predicts depends on the notes, not on their velocities") {
    std::vector<Note> flat = pianoPiece(64), random = flat;
    std::mt19937 rng(7);
    for (Note& note : random) note.velocity = std::uniform_int_distribution<int>(1, 127)(rng);
    const auto a = model().predict(flat, Meter{}), b = model().predict(random, Meter{});
    CHECK(a == b);
    for (const double v : a) CHECK(v > 1.0 && v < 127.0);
    // Not one velocity for all: it shapes them.
    CHECK(*std::max_element(a.begin(), a.end()) - *std::min_element(a.begin(), a.end()) > 10.0);
}

TEST_CASE("a part whole bars later in the song gets the same velocities; off the bar line, others") {
    const auto atStart = model().predict(pianoPiece(100, 0.0), Meter{});
    CHECK(model().predict(pianoPiece(100, 32.0), Meter{}) == atStart);  // (from bar 9: the part's first bar)
    CHECK(model().predict(pianoPiece(100, 1.0), Meter{}) != atStart);   // (every chord on another beat of the bar)
    // In 3/4 the same notes fall elsewhere in their bars.
    CHECK(model().predict(pianoPiece(100, 0.0), Meter{120.0, 3, 4}) != atStart);
}

TEST_CASE("humanizing keeps the targets' level, and leaves the other notes as they are") {
    std::vector<Note> part = pianoPiece(90);
    for (size_t i = 0; i < part.size(); i += 2) part[i].target = true;
    const std::vector<int> out = model().humanize(part, Meter{}, 1.0);
    REQUIRE(out.size() == part.size());
    std::vector<int> targets;
    for (size_t i = 0; i < part.size(); ++i) {
        if (part[i].target)
            targets.push_back(out[i]);
        else
            CHECK_EQ(out[i], 90);
    }
    CHECK_NEAR(mean(targets), 90.0, 0.5);
    CHECK(*std::max_element(targets.begin(), targets.end()) - *std::min_element(targets.begin(), targets.end()) > 10);
    for (const int v : out) CHECK(v >= 1 && v <= 127);
    // The same notes always come out the same.
    CHECK(model().humanize(part, Meter{}, 1.0) == out);
}

TEST_CASE("the amount: none leaves the velocities, half goes half the way") {
    const std::vector<Note> part = allTargets(pianoPiece(80));
    const std::vector<int> none = model().humanize(part, Meter{}, 0.0);
    for (const int v : none) CHECK_EQ(v, 80);
    const std::vector<int> full = model().humanize(part, Meter{}, 1.0);
    const std::vector<int> half = model().humanize(part, Meter{}, 0.5);
    for (size_t i = 0; i < part.size(); ++i) {
        INFO("note " + std::to_string(i));
        CHECK(std::abs((half[i] - 80) - (full[i] - 80) / 2.0) <= 1.0);
    }
    // No targets: nothing to do.
    CHECK(model().humanize(pianoPiece(80), Meter{}, 1.0) == none);
}

TEST_CASE("a melody over chords comes out louder than the chords' inner voices") {
    const std::vector<Note> part = allTargets(pianoPiece(100));
    const std::vector<int> out = model().humanize(part, Meter{}, 1.0);
    std::vector<int> melody, inner;
    for (size_t i = 0; i < part.size(); ++i) {
        if (part[i].pitch >= 72) melody.push_back(out[i]);
        if (part[i].pitch >= 50 && part[i].pitch <= 53) inner.push_back(out[i]);  // (each chord's middle note)
    }
    CHECK(mean(melody) > mean(inner) + 3.0);
}

TEST_CASE("one note, one chord, and notes at one time are humanized without trouble") {
    const std::vector<Note> one{{60, 0.0, 1.0, 100, true}};
    CHECK_EQ(model().humanize(one, Meter{}, 1.0), std::vector<int>{100});  // (its own level)
    const std::vector<Note> chord{{60, 2.0, 1.0, 100, true}, {64, 2.0, 1.0, 100, true}, {67, 2.0, 1.0, 100, true}};
    const auto out = model().humanize(chord, Meter{}, 1.0);
    CHECK_NEAR(mean(out), 100.0, 0.5);
    const std::vector<Note> silent{{60, 0.0, 0.0, 100, true}, {62, 0.0, 0.0, 100, true}};
    CHECK(model().humanize(silent, Meter{}, 1.0).size() == 2);
    CHECK(model().humanize({}, Meter{}, 1.0).empty());
}

TEST_CASE("a long part is humanized in well under a second") {
    std::vector<Note> part;
    for (int i = 0; i < 40; ++i) {
        for (const Note& note : pianoPiece(100, i * 16.0)) part.push_back(note);
    }
    part = allTargets(part);  // (2 560 notes)
    const auto start = std::chrono::steady_clock::now();
    const auto out = model().humanize(part, Meter{}, 1.0);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(out.size() == part.size());
    CHECK(seconds < 1.0);
}
