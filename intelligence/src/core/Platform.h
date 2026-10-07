// What the intelligence module needs from the operating system, in one place:
// Platform.cpp on Windows, PlatformPosix.cpp elsewhere.
//
// Paths are UTF-8 strings in the system's form, as the browser's backend hands
// them over: on Windows WTF-8 (UTF-8 that may also hold the unpaired
// surrogates Windows file names can contain), converted to UTF-16 for the
// wide (W) file calls; never through the ANSI code page.

#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace sub::intelligence::platform {

#ifdef _WIN32
inline constexpr char kSeparator = '\\';
inline constexpr bool kCaseSensitivePaths = false;
// WTF-8 to UTF-16 and back (unpaired surrogates survive the trip).
std::wstring toWide(std::string_view path);
std::string toUtf8(std::wstring_view path);
#else
inline constexpr char kSeparator = '/';
inline constexpr bool kCaseSensitivePaths = true;
#endif

// What identifies a file, as the browser's keys do: on Windows its path with
// backslashes, in Windows' own lower case (LCMapStringEx, invariant locale), so
// "C:/Drums/Kick.wav" and "c:\drums\KICK.wav" are one file; elsewhere the path
// as it is, since names that differ in case are different files there.
std::string pathKey(std::string_view path);

// What tells a file's contents changed: its size and last-write time (in the
// system's units).
struct FileStamp {
    uint64_t size = 0;
    uint64_t modified = 0;

    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

// The file's stamp; none if it isn't there (or is a folder).
std::optional<FileStamp> stamp(const std::string& path);

// This thread at background priority for CPU, disk and memory (Windows:
// THREAD_MODE_BACKGROUND_BEGIN; Linux: the lowest nice value and the idle I/O
// class), so it yields to playback and to everything the user waits for.
void enterBackgroundMode();

// Moves `from` over `to` (replacing it), as one step where the system allows.
bool replaceFile(const std::string& from, const std::string& to);

// Opens a file for binary reading or writing (wide paths on Windows); null on failure.
std::FILE* openFile(const std::string& path, bool write);

}  // namespace sub::intelligence::platform
