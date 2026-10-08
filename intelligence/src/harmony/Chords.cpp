#include "harmony/Chords.h"

namespace sub::intelligence::harmony {

namespace {

constexpr std::array<const char*, 12> kNoteNames{"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

constexpr std::array<QualityInfo, kQualities> kQualityInfo{{
    {"", {0, 4, 7, 0}, 3},
    {"m", {0, 3, 7, 0}, 3},
    {"dim", {0, 3, 6, 0}, 3},
    {"aug", {0, 4, 8, 0}, 3},
    {"sus2", {0, 2, 7, 0}, 3},
    {"sus4", {0, 5, 7, 0}, 3},
    {"7", {0, 4, 7, 10}, 4},
    {"maj7", {0, 4, 7, 11}, 4},
    {"m7", {0, 3, 7, 10}, 4},
    {"m7b5", {0, 3, 6, 10}, 4},
    {"dim7", {0, 3, 6, 9}, 4},
}};

constexpr std::array<int, 7> kMajorScale{0, 2, 4, 5, 7, 9, 11};
constexpr std::array<int, 7> kMinorScale{0, 2, 3, 5, 7, 8, 10};  // natural minor

}  // namespace

const char* noteName(int pc) { return kNoteNames[static_cast<size_t>(pitchClass(pc))]; }

const QualityInfo& qualityInfo(Quality quality) { return kQualityInfo[static_cast<size_t>(quality)]; }

uint16_t Chord::pitchClasses() const {
    const QualityInfo& info = qualityInfo(quality);
    uint16_t bits = 0;
    for (int i = 0; i < info.size; ++i) bits |= static_cast<uint16_t>(1u << pitchClass(root + info.intervals[i]));
    return bits;
}

std::array<int, 4> Chord::tones() const {
    const QualityInfo& info = qualityInfo(quality);
    std::array<int, 4> tones{};
    for (int i = 0; i < info.size; ++i) tones[i] = pitchClass(root + info.intervals[i]);
    return tones;
}

std::string Chord::name() const {
    std::string text = std::string(noteName(root)) + qualityInfo(quality).suffix;
    if (bass >= 0 && bass != root) text += std::string("/") + noteName(bass);
    return text;
}

uint16_t Key::scale() const {
    uint16_t bits = 0;
    for (const int step : minor ? kMinorScale : kMajorScale) bits |= static_cast<uint16_t>(1u << pitchClass(tonic + step));
    return bits;
}

bool Key::contains(int pc) const { return (scale() >> pitchClass(pc)) & 1; }

Chord Key::triad(int degree) const {
    const auto& steps = minor ? kMinorScale : kMajorScale;
    const auto step = [&](int d) { return steps[static_cast<size_t>(((d % 7) + 7) % 7)] + 12 * (d >= 7 ? 1 : 0); };
    degree = ((degree % 7) + 7) % 7;
    const int third = step(degree + 2) - step(degree);
    const int fifth = step(degree + 4) - step(degree);
    Quality quality = Quality::Major;
    if (third == 3) quality = fifth == 6 ? Quality::Diminished : Quality::Minor;
    else if (fifth == 8) quality = Quality::Augmented;
    return {pitchClass(tonic + step(degree)), quality};
}

std::string Key::name() const { return std::string(noteName(tonic)) + (minor ? " minor" : " major"); }

}  // namespace sub::intelligence::harmony
