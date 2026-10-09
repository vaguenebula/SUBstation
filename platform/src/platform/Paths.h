// Paths: their form on this system, and how they compare.
//
// Paths cross every layer as UTF-8 strings (WTF-8 on Windows: see Unicode.h).
// Where they meet the system they are in its own form, `NativeString`: UTF-16
// for Windows' wide calls, bytes as the file system has them elsewhere.
// std::filesystem takes them through toPath(), never through its char
// constructor (which on Windows reads the ANSI code page).

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "platform/Unicode.h"

namespace sub::platform {

#ifdef _WIN32
using NativeString = std::wstring;  // UTF-16, as the wide (W) calls take it
using NativeChar = wchar_t;
inline constexpr char kSeparator = '\\';
// Names that differ only in case are the same file.
inline constexpr bool kCaseSensitivePaths = false;
#else
using NativeString = std::string;  // bytes, as the file system has them (UTF-8 in practice)
using NativeChar = char;
inline constexpr char kSeparator = '/';
// Names that differ only in case are different files.
inline constexpr bool kCaseSensitivePaths = true;
#endif
using NativeStringView = std::basic_string_view<NativeChar>;

// Whether `c` separates folders in a path: '\' and '/' on Windows, '/' elsewhere.
constexpr bool isSeparator(char c) { return c == '/' || (kSeparator == '\\' && c == '\\'); }

// Appends a name to a folder's path (UTF-8 or the system's form), with a
// separator between unless the path ends in one already, or on Windows is a
// drive alone ("C:" is that drive's own folder, as os.path.join has it).
template <typename Char>
void appendName(std::basic_string<Char>& path, std::basic_string_view<Char> name) {
    const Char last = path.empty() ? Char() : path.back();
    const bool separated =
        last == Char('/') || (kSeparator == '\\' && (last == Char('\\') || last == Char(':')));
    if (!separated) path.push_back(static_cast<Char>(kSeparator));
    path.append(name);
}

// A UTF-8 path in the system's form, and back.
#ifdef _WIN32
inline NativeString toNative(std::string_view utf8) { return toWide(utf8); }
inline std::string fromNative(NativeStringView native) { return fromWide(native); }
#else
inline NativeString toNative(std::string_view utf8) { return NativeString(utf8); }
inline std::string fromNative(NativeStringView native) { return std::string(native); }
#endif

// A UTF-8 path as std::filesystem takes it, and back.
inline std::filesystem::path toPath(std::string_view utf8) { return std::filesystem::path(toNative(utf8)); }
inline std::string fromPath(const std::filesystem::path& path) { return fromNative(path.native()); }

// What identifies a file by its name or path, as os.path.normcase() made it.
//
// On Windows a name in a key is Windows' own lower case (LCMapStringEx with the
// invariant locale), and '/' in a path becomes '\': names that differ only in
// case are the same file there, so "C:/Drums/Kick.wav" and "c:\drums\KICK.wav"
// have one key. Elsewhere a name or path is as it is: file systems there are
// case-sensitive, so "Kick.wav" and "kick.wav" are two files and keep two keys.
// Nothing is normalised ("." and ".."): callers that need it do it first.
std::string nameKey(std::string_view name);
std::string pathKey(std::string_view path);

// The path without "." and ".." where they can go and without doubled
// separators, in the system's separators (lexically: links aren't followed).
inline std::string normalPath(std::string_view path) {
    return fromPath(toPath(path).lexically_normal().make_preferred());
}

// What identifies a file however its path is written: pathKey(normalPath()).
inline std::string fileKey(std::string_view path) { return pathKey(normalPath(path)); }

}  // namespace sub::platform
