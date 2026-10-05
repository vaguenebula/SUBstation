#include "LibraryGen.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "PyRandom.h"

namespace sub::bench {

namespace {

using Words = std::vector<std::string>;

const Words kInstruments{"Kick",  "Snare",      "Clap",  "Hat",      "Open Hat", "Closed Hat", "Tom",   "Perc",
                         "Shaker", "Ride",      "Crash", "Bass",     "808",      "Sub",        "Lead",  "Pad",
                         "Pluck", "Chord",      "Stab",  "FX",       "Riser",    "Impact",     "Vox",   "Vocal Chop",
                         "Loop",  "Fill",       "Break", "Top Loop", "Arp",      "Keys",       "Piano", "Guitar",
                         "Strings", "Brass",    "Synth", "Rim",      "Snap",     "Cowbell",    "Conga", "Bongo"};
const Words kCharacter{"Deep",   "Tight", "Punchy", "Dark",   "Bright",  "Warm",     "Dirty",    "Clean",
                       "Analog", "Vinyl", "Lofi",   "Hard",   "Soft",    "Wide",     "Short",    "Long",
                       "Big",    "Small", "Crispy", "Fat",    "Dusty",   "Metallic", "Airy",     "Gritty",
                       "Smooth", "Distorted", "Reversed", "Layered", "Processed", "Dry"};
const Words kKeys{"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const Words kKeyModes{"", "m", "min", "maj"};
const Words kBpmSuffixes{"", "bpm", "BPM"};
const Words kGenres{"House", "Techno", "Trap",    "Hip Hop",  "DnB",       "Lofi", "Ambient", "Pop",
                    "RnB",   "Dubstep", "Garage", "Afro",    "Latin",     "Cinematic", "Synthwave", "Jazz"};
const Words kCategories{"Drums", "One Shots", "Loops", "Bass", "Melodic", "FX", "Vocals", "Textures"};
const Words kSubfolders{"Kicks",      "Snares",     "Claps",  "Hats",   "Percussion", "Cymbals", "Toms",   "Top Loops",
                        "Full Loops", "Bass Loops", "Bass Shots", "Chords", "Leads",  "Pads",    "Plucks", "Risers",
                        "Impacts",    "Sweeps",     "Chops",  "Phrases", "Atmos",     "Foley"};
const Words kSeparators{"_", " ", "-", " - "};
const Words kAudio{".wav", ".mp3", ".flac", ".wave", ".WAV"};
const std::vector<int> kAudioWeights{80, 9, 8, 1, 2};
const Words kExtras{"Readme.txt", "Artwork.png", "License.pdf", "Cover.jpg"};

std::string twoDigits(int number) {
    char text[16];
    std::snprintf(text, sizeof text, "%02d", number);
    return text;
}

std::string sampleName(PyRandom& rng, int number) {
    const std::string& separator = rng.choice(kSeparators);
    Words words;
    if (rng.random() < 0.3) {
        std::string genre = rng.choice(kGenres);
        std::erase(genre, ' ');
        words.push_back(genre);
    }
    if (rng.random() < 0.6) words.push_back(rng.choice(kCharacter));
    words.push_back(rng.choice(kInstruments));
    if (rng.random() < 0.35) {
        std::string key = rng.choice(kKeys);
        key += rng.choice(kKeyModes);
        words.push_back(key);
    }
    if (rng.random() < 0.3) {
        std::string tempo = std::to_string(rng.randint(70, 175));
        tempo += rng.choice(kBpmSuffixes);
        words.push_back(tempo);
    }
    words.push_back(twoDigits(number));
    const std::string& extension = rng.weightedChoice(kAudio, kAudioWeights);
    std::string name;
    for (size_t i = 0; i < words.size(); ++i) name += (i ? separator : std::string()) + words[i];
    return name + extension;
}

// Makes an empty file (keeping one that is there), and its folder if need be.
void touch(const std::filesystem::path& path) {
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        if (file) return;
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) throw std::filesystem::filesystem_error("can't make the file", path, std::make_error_code(std::errc::io_error));
}

// The marker's text, with Windows' line ends read as Python reads them.
std::string readMarker(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::erase(text, '\r');
    return text;
}

}  // namespace

std::filesystem::path defaultLibraryRoot(int audioFiles, uint32_t seed) {
    return std::filesystem::temp_directory_path() / "sub-browser-bench" /
           ("lib-" + std::to_string(audioFiles) + "-" + std::to_string(seed));
}

std::filesystem::path makeLibrary(int audioFiles, uint32_t seed, const std::optional<std::filesystem::path>& root) {
    const std::filesystem::path dir = root ? *root : defaultLibraryRoot(audioFiles, seed);
    const std::filesystem::path marker = dir / "LIBRARY.txt";
    const std::string description = "SUBstation browser benchmark library: " + std::to_string(audioFiles) +
                                    " audio files, seed " + std::to_string(seed) + "\n";
    if (readMarker(marker) == description) return dir;
    PyRandom rng(seed);
    int made = 0;
    int pack = 0;
    while (made < audioFiles) {
        ++pack;
        char number[16];
        std::snprintf(number, sizeof number, "%03d", pack);
        std::string packName = "Pack " + std::string(number) + " - ";
        packName += rng.choice(kCharacter);
        packName += " ";
        packName += rng.choice(kGenres);
        const std::filesystem::path packDir = dir / packName;
        const int extras = rng.randint(1, static_cast<int>(kExtras.size()));
        for (const std::string& extra : rng.sample(kExtras, static_cast<size_t>(extras))) touch(packDir / extra);
        const int categories = rng.randint(2, 5);
        for (const std::string& category : rng.sample(kCategories, static_cast<size_t>(categories))) {
            const int subfolders = rng.randint(1, 4);
            for (const std::string& sub : rng.sample(kSubfolders, static_cast<size_t>(subfolders))) {
                const std::filesystem::path folder = packDir / category / sub;
                const int count = std::min(rng.randint(20, 150), audioFiles - made);
                for (int i = 0; i < count; ++i) {
                    const std::string name = sampleName(rng, i + 1);
                    touch(folder / name);
                    if (rng.random() < 0.3) touch(folder / (name + ".asd"));  // Ableton's analysis file next to it
                }
                made += count;
                if (made >= audioFiles) break;
            }
            if (made >= audioFiles) break;
        }
    }
    std::filesystem::create_directories(dir);
    std::ofstream(marker, std::ios::binary) << description;
    return dir;
}

}  // namespace sub::bench
