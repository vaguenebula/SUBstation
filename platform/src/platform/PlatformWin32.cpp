// The platform layer on Windows: the wide (W) calls, with paths in UTF-16.

#include <windows.h>
#include <avrt.h>

#include "platform/Files.h"
#include "platform/Paths.h"
#include "platform/Threads.h"

namespace sub::platform {

namespace {

// Windows' own lower case (LCMapStringEx, invariant locale): what
// os.path.normcase() uses, so keys come out as Python made them.
std::wstring lowerCase(std::wstring_view s) {
    if (s.empty()) return {};
    std::wstring lowered(s.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), static_cast<int>(s.size()),
                                lowered.data(), static_cast<int>(lowered.size()), nullptr, nullptr, 0);
    if (n <= 0) return std::wstring(s);
    lowered.resize(static_cast<size_t>(n));
    return lowered;
}

// A key of a name or path: Windows' lower case, and with `separators` '/' as '\'.
std::string key(std::string_view text, bool separators) {
    if (isAscii(text)) {  // (Windows' lower case of ASCII is ASCII's)
        std::string key(text);
        for (char& c : key) {
            if (separators && c == '/') c = '\\';
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        }
        return key;
    }
    std::wstring wide = toWide(text);
    if (separators)
        for (wchar_t& c : wide)
            if (c == L'/') c = L'\\';
    return fromWide(lowerCase(wide));
}

}  // namespace

// --- Paths.h ---------------------------------------------------------------------------

std::string nameKey(std::string_view name) { return key(name, false); }

std::string pathKey(std::string_view path) { return key(path, true); }

// --- Files.h ---------------------------------------------------------------------------

std::optional<FileStamp> stamp(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(toWide(path).c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return std::nullopt;
    FileStamp s;
    s.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    s.modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return s;
}

bool replaceFile(const std::string& from, const std::string& to) {
    return MoveFileExW(toWide(from).c_str(), toWide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::FILE* openFile(const std::string& path, bool write) {
    return _wfopen(toWide(path).c_str(), write ? L"wb" : L"rb");
}

// --- Threads.h -------------------------------------------------------------------------

void enterBackgroundMode() { SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); }

ScopedRealtimePriority::ScopedRealtimePriority() noexcept {
    DWORD task = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
}

ScopedRealtimePriority::~ScopedRealtimePriority() {
    if (handle_) AvRevertMmThreadCharacteristics(handle_);
}

}  // namespace sub::platform
