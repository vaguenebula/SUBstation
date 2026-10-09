#include "Text.h"

#include <algorithm>
#include <array>

namespace sub::browser {

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

using sub::platform::appendUtf8;
using sub::platform::decodeUtf8;

std::vector<uint32_t> codePoints(std::string_view s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    auto* p = reinterpret_cast<const unsigned char*>(s.data());
    auto* end = p + s.size();
    while (p < end) cps.push_back(decodeUtf8(p, end));
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

std::string pyLower(std::string_view s) {
    if (isAscii(s)) return asciiLower(s);
    const auto cps = codePoints(s);
    std::string out;
    out.reserve(s.size() + 4);
    for (size_t i = 0; i < cps.size(); ++i) {
        const uint32_t c = cps[i];
        if (c < 0x80) {
            appendUtf8(out, c >= 'A' && c <= 'Z' ? c + 32 : c);
        } else if (c == 0x3A3) {
            appendUtf8(out, isFinalSigma(cps, i) ? 0x3C2 : 0x3C3);
        } else if (const CaseMapping* m = findMapping(kLower, c)) {
            for (int k = 0; k < m->n; ++k) appendUtf8(out, m->to[k]);
        } else {
            appendUtf8(out, c);
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
        const uint32_t c = decodeUtf8(p, end);
        if (c == 0x3A3) {
            appendUtf8(out, 0x3C3);  // casefold has no context
        } else if (const CaseMapping* m = c < 0x80 ? nullptr : findMapping(kFold, c)) {
            for (int k = 0; k < m->n; ++k) appendUtf8(out, m->to[k]);
        } else {
            appendUtf8(out, c >= 'A' && c <= 'Z' ? c + 32 : c);
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
        const uint32_t c = decodeUtf8(p, end);
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
        const uint32_t c = decodeUtf8(p, end);
        if (atStart && isWord(c)) {  // ^ then \w
            out.push_back(0);
            continue;
        }
        if (isSeparator(c) && p < end) {
            const unsigned char* next = p;
            if (isWord(decodeUtf8(next, end))) {
                out.push_back(static_cast<uint32_t>(p - begin));
                p = next;
            }
        }
    }
}

const char* unicodeVersion() { return kUnicodeVersion; }

}  // namespace sub::browser
