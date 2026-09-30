#include "Text.h"

#include <algorithm>
#include <array>

#ifdef _WIN32
#include <windows.h>
#endif

namespace gil::browser {

namespace {

struct Range {
    uint32_t first, last;
};

struct CaseMapping {
    uint32_t cp;
    uint32_t to[3];
    uint8_t n;
};

#include "UnicodeTables.inc"

template <size_t N>
bool inRanges(const Range (&table)[N], uint32_t c) {
    const auto* it = std::upper_bound(std::begin(table), std::end(table), c,
                                      [](uint32_t v, const Range& r) { return v < r.first; });
    return it != std::begin(table) && c <= (it - 1)->last;
}

template <size_t N>
const CaseMapping* findMapping(const CaseMapping (&table)[N], uint32_t c) {
    const auto* it = std::lower_bound(std::begin(table), std::end(table), c,
                                      [](const CaseMapping& m, uint32_t v) { return m.cp < v; });
    return it != std::end(table) && it->cp == c ? it : nullptr;
}

// Classes of the ASCII characters, from the same tables.
enum : uint8_t { kIsSpace = 1, kIsWord = 2, kIsSeparator = 4 };

std::array<uint8_t, 128> makeAsciiClasses() {
    std::array<uint8_t, 128> classes{};
    for (uint32_t c = 0; c < 128; ++c) {
        uint8_t k = 0;
        if (inRanges(kSpace, c)) k |= kIsSpace | kIsSeparator;
        if (inRanges(kWord, c)) k |= kIsWord;
        if (c == '_' || c == '-' || c == '.' || c == '(' || c == ')' || c == '[' || c == ']') k |= kIsSeparator;
        classes[c] = k;
    }
    return classes;
}

const std::array<uint8_t, 128> kAscii = makeAsciiClasses();

bool isSpace(uint32_t c) { return c < 128 ? (kAscii[c] & kIsSpace) != 0 : inRanges(kSpace, c); }
bool isWord(uint32_t c) { return c < 128 ? (kAscii[c] & kIsWord) != 0 : inRanges(kWord, c); }
// The regex's [\s_\-.()\[\]]
bool isSeparator(uint32_t c) { return c < 128 ? (kAscii[c] & kIsSeparator) != 0 : inRanges(kSpace, c); }

// One code point from WTF-8 (surrogates allowed). Bad bytes read as U+FFFD.
uint32_t decode(const unsigned char*& p, const unsigned char* end) {
    const uint32_t b = *p++;
    if (b < 0x80) return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    if (extra < 0 || end - p < extra) return 0xFFFD;
    uint32_t c = b & (0x3F >> extra);
    for (int i = 0; i < extra; ++i) {
        if ((*p & 0xC0) != 0x80) return 0xFFFD;
        c = (c << 6) | (*p++ & 0x3F);
    }
    return c;
}

void append(std::string& out, uint32_t c) {
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

std::vector<uint32_t> codePoints(std::string_view s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    auto* p = reinterpret_cast<const unsigned char*>(s.data());
    auto* end = p + s.size();
    while (p < end) cps.push_back(decode(p, end));
    return cps;
}

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

// str.lower()'s Final_Sigma rule for the capital sigma at `i`.
bool isFinalSigma(const std::vector<uint32_t>& cps, size_t i) {
    size_t j = i;
    for (;;) {
        if (j == 0) return false;
        const uint32_t c = cps[--j];
        if (inRanges(kCaseIgnorable, c)) continue;
        if (!inRanges(kCased, c)) return false;
        break;
    }
    for (size_t k = i + 1; k < cps.size(); ++k) {
        const uint32_t c = cps[k];
        if (!inRanges(kCaseIgnorable, c)) return !inRanges(kCased, c);
    }
    return true;
}

}  // namespace

std::string toUtf8(std::wstring_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = static_cast<uint16_t>(s[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) {
            const uint32_t low = static_cast<uint16_t>(s[i + 1]);
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        append(out, c);
    }
    return out;
}

std::wstring toWide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    auto* p = reinterpret_cast<const unsigned char*>(s.data());
    auto* end = p + s.size();
    while (p < end) {
        const uint32_t c = decode(p, end);
        if (c >= 0x10000) {
            out.push_back(static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(c));
        }
    }
    return out;
}

bool isAscii(std::string_view s) {
    for (char c : s)
        if (static_cast<unsigned char>(c) >= 0x80) return false;
    return true;
}

std::string pyLower(std::string_view s) {
    if (isAscii(s)) return asciiLower(s);
    const auto cps = codePoints(s);
    std::string out;
    out.reserve(s.size() + 4);
    for (size_t i = 0; i < cps.size(); ++i) {
        const uint32_t c = cps[i];
        if (c < 0x80) {
            append(out, c >= 'A' && c <= 'Z' ? c + 32 : c);
        } else if (c == 0x3A3) {
            append(out, isFinalSigma(cps, i) ? 0x3C2 : 0x3C3);
        } else if (const CaseMapping* m = findMapping(kLower, c)) {
            for (int k = 0; k < m->n; ++k) append(out, m->to[k]);
        } else {
            append(out, c);
        }
    }
    return out;
}

std::string pyCasefold(std::string_view s) {
    if (isAscii(s)) return asciiLower(s);
    std::string out;
    out.reserve(s.size() + 4);
    auto* p = reinterpret_cast<const unsigned char*>(s.data());
    auto* end = p + s.size();
    while (p < end) {
        const uint32_t c = decode(p, end);
        if (c == 0x3A3) {
            append(out, 0x3C3);  // casefold has no context
        } else if (const CaseMapping* m = c < 0x80 ? nullptr : findMapping(kFold, c)) {
            for (int k = 0; k < m->n; ++k) append(out, m->to[k]);
        } else {
            append(out, c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
    }
    return out;
}

std::vector<std::string> pySplit(std::string_view s) {
    std::vector<std::string> parts;
    auto* begin = reinterpret_cast<const unsigned char*>(s.data());
    auto* p = begin;
    auto* end = p + s.size();
    const unsigned char* word = nullptr;
    while (p < end) {
        const unsigned char* at = p;
        const uint32_t c = decode(p, end);
        if (isSpace(c)) {
            if (word) parts.emplace_back(reinterpret_cast<const char*>(word), at - word);
            word = nullptr;
        } else if (!word) {
            word = at;
        }
    }
    if (word) parts.emplace_back(reinterpret_cast<const char*>(word), end - word);
    return parts;
}

void wordStarts(std::string_view s, std::vector<uint32_t>& out) {
    out.clear();
    auto* begin = reinterpret_cast<const unsigned char*>(s.data());
    auto* end = begin + s.size();
    auto* p = begin;
    // Walk the string as finditer does: a match is `^` or a separator, then a
    // word character, and the next search starts after it.
    while (p < end) {
        const bool atStart = p == begin;
        const uint32_t c = decode(p, end);
        if (atStart && isWord(c)) {  // ^ then \w
            out.push_back(0);
            continue;
        }
        if (isSeparator(c) && p < end) {
            const unsigned char* next = p;
            if (isWord(decode(next, end))) {
                out.push_back(static_cast<uint32_t>(p - begin));
                p = next;
            }
        }
    }
}

const char* unicodeVersion() { return kUnicodeVersion; }

#ifdef _WIN32
std::string ntLower(std::wstring_view s) {
    if (s.empty()) return {};
    std::wstring lowered(s.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), static_cast<int>(s.size()),
                                lowered.data(), static_cast<int>(lowered.size()), nullptr, nullptr, 0);
    if (n <= 0) return toUtf8(s);
    lowered.resize(static_cast<size_t>(n));
    return toUtf8(lowered);
}
#else
std::string ntLower(std::wstring_view s) { return pyLower(toUtf8(s)); }
#endif

}  // namespace gil::browser
