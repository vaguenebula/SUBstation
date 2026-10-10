// Text in the encodings the systems' calls take, one way and the other.
//
// Strings cross every layer as UTF-8. On Windows they are WTF-8: UTF-8 that may
// also hold unpaired surrogates, which Windows file names can contain, so a
// name read from the system and handed back to it is the same name. Windows'
// wide (W) calls take UTF-16, in wchar_t. (The conversions build anywhere, so
// they are tested anywhere; only Windows' code needs them.)

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sub::platform {

// One code point from WTF-8 at `p` (surrogates allowed), moving `p` past it.
// Bytes that don't make one read as U+FFFD, one at a time.
inline uint32_t decodeUtf8(const unsigned char*& p, const unsigned char* end) {
    const uint32_t b = *p++;
    if (b < 0x80) return b;
    const int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    if (extra < 0 || end - p < extra) return 0xFFFD;
    uint32_t c = b & (0x3F >> extra);
    for (int i = 0; i < extra; ++i) {
        if ((*p & 0xC0) != 0x80) return 0xFFFD;
        c = (c << 6) | (*p++ & 0x3F);
    }
    return c;
}

// A code point as WTF-8 (a surrogate as three bytes).
inline void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

inline bool isAscii(std::string_view s) {
    for (const char c : s)
        if (static_cast<unsigned char>(c) >= 0x80) return false;
    return true;
}

// WTF-8 to wide text and back. Unpaired surrogates survive the trip; a pair
// becomes the code point it stands for.
std::wstring toWide(std::string_view s);
std::string fromWide(std::wstring_view s);

}  // namespace sub::platform
