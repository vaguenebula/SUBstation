#pragma once
// Synthetic sample libraries for the browser benchmarks.
//
// Shaped like real sample collections: packs, categories and sub-folders, 20-150
// files a folder, names made of instrument, character, key and tempo words with
// the separators people use, mostly WAV with some FLAC and MP3, and the files that
// sit next to samples (Ableton .asd analysis files, artwork, readmes). The files
// are empty: the browser reads names, never contents.
//
// A library is made once per (size, seed) and reused; LIBRARY.txt in its root
// says what it holds. The names come from Python's random numbers (PyRandom.h),
// so a library is the same, file for file, as the Python benchmarks' generator
// made it before, and either can reuse the other's.

#include <cstdint>
#include <filesystem>
#include <optional>

namespace sub::bench {

// <temp>/sub-browser-bench/lib-<audioFiles>-<seed>
std::filesystem::path defaultLibraryRoot(int audioFiles, uint32_t seed);

// A library of about `audioFiles` audio files (and ~40 % as many other files),
// in `root` (by default defaultLibraryRoot()). Returns its root.
std::filesystem::path makeLibrary(int audioFiles, uint32_t seed = 1,
                                  const std::optional<std::filesystem::path>& root = std::nullopt);

}  // namespace sub::bench
