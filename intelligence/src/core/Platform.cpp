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
