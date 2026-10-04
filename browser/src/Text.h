// Text handling for the browser, matching Python's exactly.
//
// The search used to be Python, and its matching and ordering are defined by
// Python's str.lower(), str.casefold(), str.split() and the regex classes \s and
// \w. These do the same, from tables generated out of Python itself
// (tools/gen_unicode_tables.py), so results don't change.
//
// Strings are WTF-8: UTF-8 that may also hold unpaired surrogates, which Windows
// file names (and so Python strings) can contain. Byte order is code point order,
// so comparing bytes compares strings as Python does. (How file names compare in
// item keys is the platform's: see nameKey() in Platform.h.)

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sub::browser {

// WTF-8 and UTF-16 (in wchar_t, as Windows' wide calls take it), both ways.
std::string toUtf8(std::wstring_view s);
std::wstring toWide(std::string_view s);

bool isAscii(std::string_view s);

// str.lower(), str.casefold() and str.split() (without arguments).
std::string pyLower(std::string_view s);
std::string pyCasefold(std::string_view s);
std::vector<std::string> pySplit(std::string_view s);

// Where `re.finditer(r"(?:^|[\s_\-.()\[\]])(\w)", s)` finds its group, as byte
// offsets: the starts of the words in a name.
void wordStarts(std::string_view s, std::vector<uint32_t>& out);

// The Unicode version of the tables (Python's unicodedata.unidata_version).
const char* unicodeVersion();

}  // namespace sub::browser
