// Files, by UTF-8 path (see Paths.h).

#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sub::platform {

// What tells a file's contents changed: its size and last-write time (in the
// system's units).
struct FileStamp {
    uint64_t size = 0;
    uint64_t modified = 0;

    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

// The file's stamp; none if it isn't there (or is a folder).
std::optional<FileStamp> stamp(const std::string& path);

// Moves `from` over `to`, replacing it at once where the system allows (a file
// is written to a temporary one first and then put in place, so a crash never
// leaves half of one). False on failure.
bool replaceFile(const std::string& from, const std::string& to);

// Opens a file for binary reading or writing; null on failure.
std::FILE* openFile(const std::string& path, bool write);

// A whole file's bytes, as a std::string or a std::vector<char> (whichever the
// caller keeps it in: no copy); none if it can't be read.
template <typename Bytes = std::string>
std::optional<Bytes> readFile(const std::string& path);
extern template std::optional<std::string> readFile(const std::string&);
extern template std::optional<std::vector<char>> readFile(const std::string&);

// Writes a whole file as one step: to `path` + ".tmp" first (making the folder
// if needed), then over `path` (replaceFile), so a crash or a full disk never
// leaves half of one. False on failure; `path` is as it was then.
bool writeFileAtomically(const std::string& path, std::string_view bytes);

}  // namespace sub::platform
