#include "plugins/PresetName.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

namespace sub::vst3 {

namespace {

constexpr size_t kScanLimit = 64 * 1024;  // names sit in a header; the rest is binary or a big payload

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// `needle` in `text` at or after `from`, ignoring case; npos if none.
size_t find(std::string_view text, std::string_view needle, size_t from = 0) {
    if (needle.empty() || text.size() < needle.size()) return std::string_view::npos;
    for (size_t i = from; i + needle.size() <= text.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && lower(text[i + k]) == lower(needle[k])) ++k;
        if (k == needle.size()) return i;
    }
    return std::string_view::npos;
}

std::string trimmed(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// The text up to the closing `quote`, if it is printable and short (a name, not binary).
std::string quoted(std::string_view text, size_t start, char quote) {
    const size_t end = text.find(quote, start);
    if (end == std::string_view::npos || end - start > 200) return {};
    for (size_t i = start; i < end; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 && c != '\t') return {};
    }
    return trimmed(text.substr(start, end - start));
}

// `key` followed by ="value" (XML attribute) or ":"value" (JSON), between `from` and `to`.
std::string attribute(std::string_view text, std::string_view key, size_t from, size_t to) {
    text = text.substr(0, to);
    for (size_t at = find(text, key, from); at != std::string_view::npos; at = find(text, key, at + 1)) {
        size_t i = at + key.size();
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_')) continue;  // "xname"
        if (i < text.size() && text[i] == '"') ++i;  // JSON: the key is quoted
        while (i < text.size() && text[i] == ' ') ++i;
        if (i >= text.size() || (text[i] != '=' && text[i] != ':')) continue;
        ++i;
        while (i < text.size() && text[i] == ' ') ++i;
        if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) continue;
        std::string value = quoted(text, i + 1, text[i]);
        if (!value.empty()) return value;
    }
    return {};
}

// The `name` attribute of the first element `<tag ...>` (Spitfire's META, Nuro Audio's PRESET).
std::string elementName(std::string_view text, std::string_view tag) {
    const size_t at = find(text, tag);
    if (at == std::string_view::npos) return {};
    const size_t close = text.find('>', at);
    return close == std::string_view::npos ? std::string() : attribute(text, "name", at, close);
}

template <typename T>
T readLittle(const uint8_t* data) {
    T value{};
    for (size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(static_cast<T>(data[i]) << (8 * i));
    return value;
}

}  // namespace

std::string presetNameFromState(const uint8_t* data, size_t size) {
    if (!data) return {};
    const std::string_view text(reinterpret_cast<const char*>(data), std::min(size, kScanLimit));

    // Spitfire (BBC Symphony Orchestra, Originals, ...): <META family="Strings" name="Core - Violins 1" .../>
    if (std::string name = elementName(text, "<META "); !name.empty()) return name;

    // Serum 2 ("presetName":"..."), Valhalla (presetName="..."), and the like.
    for (std::string_view key : {"presetName", "preset_name", "programName", "patchName"}) {
        std::string name = attribute(text, key, 0, text.size());
        if (!name.empty()) return name;
    }

    // Nuro Audio (Xrider, Xvox): <PRESET group="Factory Presets" name="Default" .../>
    return elementName(text, "<PRESET ");
}

std::string presetNameFromPreset(const uint8_t* data, size_t size) {
    // A .vstpreset: "VST3", a version, the class id (32 characters), the chunk
    // list's offset (int64); the list: "List", a count, then each chunk's id,
    // offset and size (int64s). All little-endian.
    constexpr size_t kHeader = 48;
    if (!data || size < kHeader || std::memcmp(data, "VST3", 4) != 0) return presetNameFromState(data, size);
    const auto listAt = readLittle<uint64_t>(data + 40);
    if (listAt > size || size - listAt < 8 || std::memcmp(data + listAt, "List", 4) != 0) {
        return presetNameFromState(data, size);
    }
    const auto count = readLittle<uint32_t>(data + listAt + 4);
    for (const char* wanted : {"Comp", "Cont"}) {
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t entry = listAt + 8 + i * 20;
            if (entry + 20 > size) break;
            if (std::memcmp(data + entry, wanted, 4) != 0) continue;
            const auto offset = readLittle<uint64_t>(data + entry + 4);
            const auto length = readLittle<uint64_t>(data + entry + 12);
            if (offset > size || length > size - offset) continue;
            std::string name = presetNameFromState(data + offset, static_cast<size_t>(length));
            if (!name.empty()) return name;
        }
    }
    return {};
}

bool isPlaceholderPresetName(const std::string& name) {
    const std::string s = trimmed(name);
    if (s.empty()) return true;
    size_t i = 0;
    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
    std::string word;
    for (size_t k = 0; k < i; ++k) word += lower(s[k]);
    if (word != "prog" && word != "program" && word != "preset" && word != "patch" && word != "slot") return false;
    while (i < s.size() && (s[i] == ' ' || s[i] == '_' || s[i] == '-' || s[i] == '#')) ++i;
    if (i == s.size()) return true;  // just the word
    return std::all_of(s.begin() + static_cast<std::ptrdiff_t>(i), s.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

}  // namespace sub::vst3
