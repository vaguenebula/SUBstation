// Watching folder trees on Linux: an inotify watch on each folder (see
// FolderWatcher in Platform.h).

#include "Platform.h"

#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unordered_map>

#include "PlatformPosix.h"

namespace sub::browser::platform {

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

}  // namespace sub::browser::platform
