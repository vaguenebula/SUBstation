// The intelligence module's platform layer on Windows (see Platform.h).

#include "core/Platform.h"

#include <windows.h>

namespace sub::intelligence::platform {

std::wstring toWide(std::string_view path) {
    std::wstring out;
    out.reserve(path.size());
    const auto* p = reinterpret_cast<const unsigned char*>(path.data());
    const auto* end = p + path.size();
    while (p < end) {
        // UTF-8 as WTF-8 writes it: surrogates may appear as three-byte sequences.
        uint32_t c = *p++;
        int more = 0;
        if (c >= 0xF0) {
            c &= 0x07;
            more = 3;
        } else if (c >= 0xE0) {
            c &= 0x0F;
            more = 2;
        } else if (c >= 0xC0) {
            c &= 0x1F;
            more = 1;
        }
        for (; more > 0 && p < end && (*p & 0xC0) == 0x80; --more) c = (c << 6) | (*p++ & 0x3F);
        if (c >= 0x10000) {
            out.push_back(static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(c));
        }
    }
    return out;
}

std::string toUtf8(std::wstring_view path) {
    std::string out;
    out.reserve(path.size());
    auto put = [&out](uint32_t c) {
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
    };
    for (size_t i = 0; i < path.size(); ++i) {
        const uint32_t c = path[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < path.size() && path[i + 1] >= 0xDC00 && path[i + 1] < 0xE000) {
            put(0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(path[i + 1]) - 0xDC00));
            ++i;
        } else {
            put(c);  // (an unpaired surrogate as three bytes: WTF-8)
        }
    }
    return out;
}

std::string pathKey(std::string_view path) {
    bool ascii = true;
    for (const char c : path) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
    if (ascii) {  // (Windows' lower case of ASCII is ASCII's)
        std::string key(path);
        for (char& c : key) {
            if (c == '/') c = '\\';
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        }
        return key;
    }
    std::wstring wide = toWide(path);
    for (wchar_t& c : wide)
        if (c == L'/') c = L'\\';
    if (wide.empty()) return {};
    std::wstring lowered(wide.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(), static_cast<int>(wide.size()),
                                lowered.data(), static_cast<int>(lowered.size()), nullptr, nullptr, 0);
    if (n <= 0) return toUtf8(wide);
    lowered.resize(static_cast<size_t>(n));
    return toUtf8(lowered);
}

std::optional<FileStamp> stamp(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(toWide(path).c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return std::nullopt;
    FileStamp s;
    s.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    s.modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return s;
}

void enterBackgroundMode() { SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); }

bool replaceFile(const std::string& from, const std::string& to) {
    return MoveFileExW(toWide(from).c_str(), toWide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::FILE* openFile(const std::string& path, bool write) {
    return _wfopen(toWide(path).c_str(), write ? L"wb" : L"rb");
}

}  // namespace sub::intelligence::platform
