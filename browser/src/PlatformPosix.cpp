// The browser's platform layer elsewhere than Windows (see Platform.h): POSIX
// calls. Watching is a file of its own: FolderWatcherInotify.cpp (Linux),
// FolderWatcherNone.cpp (other systems).

#include "Platform.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>

#include "PlatformPosix.h"

namespace sub::browser::platform {

bool listFolder(const NativeString& path, std::vector<Entry>& out) {
    out.clear();
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    while (const dirent* entry = readdir(dir)) {
        if (dotOrDotDot(entry->d_name)) continue;
        out.push_back({entry->d_name, isDirectory(dir, entry)});
    }
    closedir(dir);
    return true;
}

std::optional<uint64_t> folderTime(const NativeString& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG || errno == ELOOP) return std::nullopt;
        return 0;  // there, but unreadable
    }
    if (!S_ISDIR(st.st_mode)) return std::nullopt;
#ifdef __APPLE__
    const timespec& time = st.st_mtimespec;
#else
    const timespec& time = st.st_mtim;
#endif
    return static_cast<uint64_t>(time.tv_sec) * 1'000'000'000u + static_cast<uint64_t>(time.tv_nsec);
}

// --- Event ----------------------------------------------------------------------------

Event::Event() {
    int ends[2];
#ifdef __linux__
    if (pipe2(ends, O_CLOEXEC | O_NONBLOCK) != 0) return;
#else
    if (pipe(ends) != 0) return;
    for (const int fd : ends) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    handle_ = ends[0];
    writeEnd_ = ends[1];
}

Event::~Event() {
    if (handle_ >= 0) close(handle_);
    if (writeEnd_ >= 0) close(writeEnd_);
}

void Event::set() {
    const char byte = 1;
    if (writeEnd_ >= 0 && write(writeEnd_, &byte, 1) < 0) {
        // The pipe is full: it is set already.
    }
}

void Event::reset() {
    char bytes[64];
    while (handle_ >= 0 && read(handle_, bytes, sizeof bytes) > 0) {
    }
}

// --- Waiter --------------------------------------------------------------------------

bool Waiter::add(WaitHandle handle) {
    if (handles_.size() >= kMaxHandles) return false;
    handles_.push_back(handle);
    return true;
}

int Waiter::wait(std::optional<std::chrono::milliseconds> timeout) {
    std::vector<pollfd> fds(handles_.size());
    for (size_t i = 0; i < handles_.size(); ++i) fds[i] = {handles_[i], POLLIN, 0};
    int ms = -1;
    if (timeout) ms = static_cast<int>(std::clamp<long long>(timeout->count(), 0, INT_MAX));
    const int n = poll(fds.data(), static_cast<nfds_t>(fds.size()), ms);
    if (n < 0) return errno == EINTR ? kTimeout : kFailed;  // a signal: the caller looks at its deadlines again
    for (size_t i = 0; i < fds.size(); ++i)
        if (fds[i].revents & (POLLIN | POLLERR | POLLHUP)) return static_cast<int>(i);
    return kTimeout;
}

}  // namespace sub::browser::platform
