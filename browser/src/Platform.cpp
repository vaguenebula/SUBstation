// The browser's platform layer on Windows (see Platform.h).

#include "Platform.h"

#include <windows.h>

#include <algorithm>

namespace sub::browser::platform {

bool listFolder(const NativeString& path, std::vector<Entry>& out) {
    out.clear();
    // The pattern os.scandir uses: path, a backslash unless it ends in one, '*'.
    std::wstring pattern = path;
    appendName(pattern, std::wstring_view(L"*"));
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

std::optional<uint64_t> folderTime(const NativeString& path) {
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

// --- Event ----------------------------------------------------------------------------

Event::Event() : handle_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

Event::~Event() {
    if (handle_) CloseHandle(handle_);
}

void Event::set() { SetEvent(handle_); }

void Event::reset() { ResetEvent(handle_); }

// --- FolderWatcher -------------------------------------------------------------------

struct FolderWatcher::State {
    HANDLE dir = nullptr;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    std::vector<unsigned long> buffer = std::vector<unsigned long>(64 * 1024 / sizeof(unsigned long));
    bool armed = false;

    bool arm() {
        ResetEvent(event);
        return ReadDirectoryChangesW(dir, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(unsigned long)),
                                     TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, nullptr,
                                     &overlapped, nullptr) != 0;
    }
};

FolderWatcher::FolderWatcher(const NativeString& root) : state_(std::make_unique<State>()) {
    HANDLE dir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return;
    state_->dir = dir;
    state_->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state_->overlapped.hEvent = state_->event;
    state_->armed = state_->arm();
}

FolderWatcher::~FolderWatcher() {
    State& s = *state_;
    if (s.dir) {
        if (s.armed) {
            CancelIoEx(s.dir, &s.overlapped);
            DWORD bytes = 0;
            GetOverlappedResult(s.dir, &s.overlapped, &bytes, TRUE);  // the buffer stays until it's done with
        }
        CloseHandle(s.dir);
    }
    if (s.event) CloseHandle(s.event);
}

bool FolderWatcher::ok() const { return state_->armed; }

WaitHandle FolderWatcher::handle() const { return state_->event; }

FolderWatcher::Changes FolderWatcher::take(std::vector<NativeString>& paths) {
    State& s = *state_;
    paths.clear();
    if (!s.armed) return Changes::Failed;
    DWORD bytes = 0;
    if (!GetOverlappedResult(s.dir, &s.overlapped, &bytes, FALSE)) {
        if (GetLastError() == ERROR_IO_INCOMPLETE) return Changes::Paths;  // nothing yet
        s.armed = false;
        return Changes::Failed;
    }
    Changes result = Changes::Paths;
    if (bytes == 0) {
        result = Changes::Overflow;
    } else {
        const auto* at = reinterpret_cast<const unsigned char*>(s.buffer.data());
        for (;;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(at);
            paths.emplace_back(info->FileName, info->FileNameLength / sizeof(WCHAR));
            if (info->NextEntryOffset == 0) break;
            at += info->NextEntryOffset;
        }
    }
    s.armed = s.arm();
    return result;
}

// --- Waiter --------------------------------------------------------------------------

bool Waiter::add(WaitHandle handle) {
    if (handles_.size() >= kMaxHandles) return false;
    handles_.push_back(handle);
    return true;
}

int Waiter::wait(std::optional<std::chrono::milliseconds> timeout) {
    DWORD ms = INFINITE;
    if (timeout) ms = static_cast<DWORD>(std::clamp<long long>(timeout->count(), 0, 0x7FFFFFFF));
    const DWORD woke = WaitForMultipleObjects(static_cast<DWORD>(handles_.size()), handles_.data(), FALSE, ms);
    if (woke == WAIT_TIMEOUT) return kTimeout;
    const DWORD index = woke - WAIT_OBJECT_0;
    if (index < handles_.size()) return static_cast<int>(index);
    return kFailed;
}

}  // namespace sub::browser::platform
