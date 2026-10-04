// The browser's platform layer elsewhere than Windows (see Platform.h): POSIX
// calls, and on Linux inotify for watching and per-thread priorities.

#include "Platform.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#ifdef __linux__
#include <sys/inotify.h>
#include <sys/syscall.h>
#endif

namespace sub::browser::platform {

namespace {

bool hiddenName(std::string_view name) { return !name.empty() && (name[0] == '.' || name[0] == '$'); }

bool dotOrDotDot(const char* name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

// Whether a directory entry is a real directory (not a symbolic link to one).
bool isDirectory(DIR* dir, const dirent* entry) {
#ifdef DT_UNKNOWN
    if (entry->d_type != DT_UNKNOWN) return entry->d_type == DT_DIR;
#endif
    struct stat st;
    return fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

std::string toUtf8(const NativeString& s) { return s; }

NativeString fromUtf8(std::string_view s) { return NativeString(s); }

bool isSeparator(char c) { return c == '/'; }

std::string nameKey(std::string_view name) { return std::string(name); }

std::string pathKey(std::string_view path) { return std::string(path); }

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

void enterBackgroundMode() {
#ifdef __linux__
    // On Linux the nice value and the I/O priority are per thread.
    const auto thread = static_cast<int>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(thread), 19) != 0) {
        // Not allowed: it stays at its priority.
    }
    // The idle I/O class: the disk is read for the index only when nothing else wants it.
    constexpr int kWhoProcess = 1, kClassIdle = 3, kClassShift = 13;
    if (syscall(SYS_ioprio_set, kWhoProcess, thread, kClassIdle << kClassShift) != 0) {
        // Not supported: best effort as before.
    }
#endif
}

bool replaceFile(const NativeString& from, const NativeString& to) { return std::rename(from.c_str(), to.c_str()) == 0; }

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

// --- FolderWatcher -------------------------------------------------------------------

#ifdef __linux__

namespace {
constexpr uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF |
                                IN_ONLYDIR;
}

struct FolderWatcher::State {
    int fd = -1;
    std::string root;
    int rootWatch = -1;
    bool failed = false;
    // Watched folders, relative to the root ('' for the root itself).
    std::unordered_map<int, std::string> folderOf;
    std::unordered_map<std::string, int> watchOf;
    std::vector<char> buffer = std::vector<char>(64 * 1024);

    std::string full(const std::string& relative) const {
        if (relative.empty()) return root;
        return root.ends_with('/') ? root + relative : root + '/' + relative;
    }

    // Watches a folder and every folder below it, except hidden ones (never
    // listed) and links (never walked into). A folder the system won't watch
    // (too many watches) is left out.
    void watchTree(const std::string& relative) {
        std::vector<std::string> stack{relative};
        while (!stack.empty()) {
            const std::string folder = std::move(stack.back());
            stack.pop_back();
            const std::string path = full(folder);
            // The root is followed if it is a link (as listing it does); the folders below are real ones.
            const int watch = inotify_add_watch(fd, path.c_str(), kWatchMask | (folder.empty() ? 0 : IN_DONT_FOLLOW));
            if (watch < 0) {
                if (folder.empty()) failed = true;
                continue;
            }
            // A folder moved within the tree keeps its watch: forget its old name.
            if (const auto old = folderOf.find(watch); old != folderOf.end() && old->second != folder) {
                const auto named = watchOf.find(old->second);
                if (named != watchOf.end() && named->second == watch) watchOf.erase(named);
            }
            folderOf[watch] = folder;
            watchOf[folder] = watch;
            if (folder.empty()) rootWatch = watch;
            DIR* dir = opendir(path.c_str());
            if (!dir) continue;
            while (const dirent* entry = readdir(dir)) {
                if (dotOrDotDot(entry->d_name) || hiddenName(entry->d_name) || !isDirectory(dir, entry)) continue;
                stack.push_back(folder.empty() ? std::string(entry->d_name) : folder + '/' + entry->d_name);
            }
            closedir(dir);
        }
    }

    // Stops watching a folder and the folders below it (it went, or moved).
    void unwatchTree(const std::string& relative) {
        for (auto it = watchOf.begin(); it != watchOf.end();) {
            const std::string& folder = it->first;
            const bool inside = folder == relative || (folder.size() > relative.size() && folder.starts_with(relative) &&
                                                       folder[relative.size()] == '/');
            if (inside && it->second != rootWatch) {
                inotify_rm_watch(fd, it->second);
                folderOf.erase(it->second);
                it = watchOf.erase(it);
            } else {
                ++it;
            }
        }
    }

    void handle(const inotify_event& event, const std::string& name, std::vector<NativeString>& paths,
                bool& overflow) {
        if (event.mask & IN_Q_OVERFLOW) {
            overflow = true;
            return;
        }
        const auto it = folderOf.find(event.wd);
        if (event.mask & IN_IGNORED) {  // the watch went (its folder did, or it was removed)
            if (it != folderOf.end()) {
                const auto named = watchOf.find(it->second);
                if (named != watchOf.end() && named->second == event.wd) watchOf.erase(named);
                folderOf.erase(it);
            }
            if (event.wd == rootWatch) failed = true;
            return;
        }
        if (it == folderOf.end()) return;
        if (event.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) {
            // A folder below: its parent says so. The root itself: stop watching.
            if (event.wd == rootWatch) failed = true;
            return;
        }
        if (name.empty()) return;
        const std::string path = it->second.empty() ? name : it->second + '/' + name;
        paths.push_back(path);
        if (event.mask & IN_ISDIR) {
            if (event.mask & (IN_MOVED_FROM | IN_DELETE)) unwatchTree(path);
            if ((event.mask & (IN_CREATE | IN_MOVED_TO)) && !hiddenName(name)) watchTree(path);
        }
    }
};

FolderWatcher::FolderWatcher(const NativeString& root) : state_(std::make_unique<State>()) {
    state_->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (state_->fd < 0) return;
    state_->root = root;
    state_->watchTree("");
}

FolderWatcher::~FolderWatcher() {
    if (state_->fd >= 0) close(state_->fd);  // removes its watches
}

bool FolderWatcher::ok() const { return state_->fd >= 0 && state_->rootWatch >= 0 && !state_->failed; }

WaitHandle FolderWatcher::handle() const { return state_->fd; }

FolderWatcher::Changes FolderWatcher::take(std::vector<NativeString>& paths) {
    State& s = *state_;
    paths.clear();
    if (!ok()) return Changes::Failed;
    bool overflow = false;
    for (;;) {
        const ssize_t n = read(s.fd, s.buffer.data(), s.buffer.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) s.failed = true;
            break;
        }
        if (n == 0) break;
        for (size_t at = 0; at + sizeof(inotify_event) <= static_cast<size_t>(n);) {
            inotify_event event;
            std::memcpy(&event, s.buffer.data() + at, sizeof event);
            const char* name = s.buffer.data() + at + sizeof(inotify_event);
            const size_t length = std::min<size_t>(event.len, static_cast<size_t>(n) - at - sizeof(inotify_event));
            at += sizeof(inotify_event) + event.len;
            s.handle(event, std::string(name, strnlen(name, length)), paths, overflow);
        }
    }
    if (s.failed) {
        paths.clear();
        return Changes::Failed;
    }
    if (overflow) {
        s.watchTree("");  // folders made meanwhile may not be watched yet
        paths.clear();
        return Changes::Overflow;
    }
    return Changes::Paths;
}

#else  // no inotify: nothing is watched; changes show on a rescan or the next start

struct FolderWatcher::State {};

FolderWatcher::FolderWatcher(const NativeString&) : state_(std::make_unique<State>()) {}

FolderWatcher::~FolderWatcher() = default;

bool FolderWatcher::ok() const { return false; }

WaitHandle FolderWatcher::handle() const { return -1; }

FolderWatcher::Changes FolderWatcher::take(std::vector<NativeString>& paths) {
    paths.clear();
    return Changes::Failed;
}

#endif

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
