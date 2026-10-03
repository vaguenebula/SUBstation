#pragma once

#include <filesystem>
#include <string>

namespace sub {

// Python hands us UTF-8; Windows file APIs want UTF-16.
inline std::filesystem::path pathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

inline std::wstring widen(const std::string& utf8) { return pathFromUtf8(utf8).wstring(); }

}  // namespace sub
