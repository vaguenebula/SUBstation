#include "Platform.h"

#include <windows.h>

namespace sub::browser::platform {

bool listFolder(const std::wstring& path, std::vector<Entry>& out) {
    out.clear();
    // The pattern os.scandir uses: path, a backslash unless it ends in one, '*'.
    std::wstring pattern = path;
    const wchar_t last = pattern.empty() ? L'\0' : pattern.back();
    if (last != L'\\' && last != L'/' && last != L':') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return false;
    do {
        const wchar_t* name = data.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) continue;
        // DirEntry.is_dir(follow_symlinks=False): directories, junctions too, but not symbolic links.
        const bool reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        const bool symlink = reparse && data.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
        out.push_back({name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !symlink});
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return true;
}

std::optional<uint64_t> folderTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME ||
            error == ERROR_BAD_NETPATH || error == ERROR_DIRECTORY)
            return std::nullopt;
        return 0;  // there, but unreadable
    }
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    // A junction: the time of the folder it leads to.
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    FILE_BASIC_INFO info;
    const BOOL ok = GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info));
    CloseHandle(handle);
    return ok ? static_cast<uint64_t>(info.LastWriteTime.QuadPart) : 0;
}

void enterBackgroundMode() { SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); }

Event::Event() : handle_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

Event::~Event() {
    if (handle_) CloseHandle(handle_);
}

void Event::set() { SetEvent(handle_); }

FolderWatcher::FolderWatcher(const std::wstring& root) : buffer_(64 * 1024 / sizeof(unsigned long)) {
    HANDLE dir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return;
    dir_ = dir;
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto* overlapped = new OVERLAPPED{};
    overlapped->hEvent = event_;
    overlapped_ = overlapped;
    armed_ = arm();
}

FolderWatcher::~FolderWatcher() {
    auto* overlapped = static_cast<OVERLAPPED*>(overlapped_);
    if (dir_) {
        if (armed_) {
            CancelIoEx(dir_, overlapped);
            DWORD bytes = 0;
            GetOverlappedResult(dir_, overlapped, &bytes, TRUE);  // the buffer stays until it's done with
        }
        CloseHandle(dir_);
    }
    if (event_) CloseHandle(event_);
    delete overlapped;
}

bool FolderWatcher::arm() {
    ResetEvent(event_);
    return ReadDirectoryChangesW(dir_, buffer_.data(), static_cast<DWORD>(buffer_.size() * sizeof(unsigned long)), TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, nullptr,
                                 static_cast<OVERLAPPED*>(overlapped_), nullptr) != 0;
}

FolderWatcher::Changes FolderWatcher::take(std::vector<std::wstring>& paths) {
    paths.clear();
    if (!armed_) return Changes::Failed;
    DWORD bytes = 0;
    if (!GetOverlappedResult(dir_, static_cast<OVERLAPPED*>(overlapped_), &bytes, FALSE)) {
        if (GetLastError() == ERROR_IO_INCOMPLETE) return Changes::Paths;  // nothing yet
        armed_ = false;
        return Changes::Failed;
    }
    Changes result = Changes::Paths;
    if (bytes == 0) {
        result = Changes::Overflow;
    } else {
        const auto* at = reinterpret_cast<const unsigned char*>(buffer_.data());
        for (;;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(at);
            paths.emplace_back(info->FileName, info->FileNameLength / sizeof(WCHAR));
            if (info->NextEntryOffset == 0) break;
            at += info->NextEntryOffset;
        }
    }
    armed_ = arm();
    return result;
}

}  // namespace sub::browser::platform
