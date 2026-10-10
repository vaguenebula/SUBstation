// What the index needs from the operating system besides what every layer
// shares (sub_platform: paths in the system's form and their keys, background
// priority, replacing files): listing folders and their times, waking its
// thread, and watching folder trees for changes. Platform.cpp is Windows' (the
// Win32 calls the browser always used), PlatformPosix.cpp everyone else's;
// watching is FolderWatcherInotify.cpp on Linux and FolderWatcherNone.cpp on
// other POSIX systems (which list and check folder times but don't watch).
//
// Paths and names cross the rest of the library as UTF-8 (WTF-8 on Windows:
// it may hold unpaired surrogates, which Windows file names can contain); here
// they are in the system's own form, `NativeString`.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/Files.h"
#include "platform/Paths.h"
#include "platform/Threads.h"

namespace sub::browser::platform {

// The shared layer's names are this one's too (platform::pathKey, NativeString...).
using namespace sub::platform;

#ifdef _WIN32
using WaitHandle = void*;  // a HANDLE
#else
using WaitHandle = int;  // a file descriptor
#endif

// Names the index never lists (and so never watches): starting with '.' or '$'.
inline bool hiddenName(NativeStringView name) {
    return !name.empty() && (name[0] == NativeChar('.') || name[0] == NativeChar('$'));
}

struct Entry {
    NativeString name;
    // A folder to walk into. On Windows directories and junctions, not symbolic
    // links (as DirEntry.is_dir(follow_symlinks=False)); elsewhere real
    // directories, not symbolic links to them.
    bool folder = false;
};

// A folder's entries, in the order the file system lists them (as os.scandir
// does). False if it can't be read.
bool listFolder(const NativeString& path, std::vector<Entry>& out);

// When a folder's entries last changed (its last-write time, following
// junctions and links); nothing if it's gone, 0 if it's there but can't be read.
std::optional<uint64_t> folderTime(const NativeString& path);

// An auto-reset event: set by any thread, waited for (with a Waiter) by one.
class Event {
public:
    Event();
    ~Event();
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    void set();
    void reset();  // unset (a wait that it ended may not have done so)
    WaitHandle handle() const { return handle_; }

private:
#ifdef _WIN32
    WaitHandle handle_ = nullptr;
#else
    WaitHandle handle_ = -1;  // a pipe's ends
    int writeEnd_ = -1;
#endif
};

// Watches a folder and everything below it for files and folders being added,
// removed or renamed. On Windows ReadDirectoryChangesW on the root (recursive);
// on Linux an inotify watch on each folder in the tree, except hidden ones
// (names starting with '.' or '$', which the index never lists), kept up as
// folders come and go. Folders the system won't watch any more of (too many
// watches) are left out: their changes show on a rescan or the next start.
class FolderWatcher {
public:
    explicit FolderWatcher(const NativeString& root);
    ~FolderWatcher();
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    bool ok() const;
    WaitHandle handle() const;  // signalled when changes are there to take

    enum class Changes { Paths, Overflow, Failed };
    // After handle() was signalled: the changed paths (relative to the root,
    // separated by kSeparator), or Overflow (too many to tell: look at
    // everything), or Failed (stopped watching).
    Changes take(std::vector<NativeString>& paths);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Waits for one of several handles (an Event's, FolderWatchers') to be
// signalled, or for a timeout.
class Waiter {
public:
    // How many handles one wait takes. Windows' WaitForMultipleObjects takes 64,
    // so only the wake event and the first 63 places' watchers are waited for.
#ifdef _WIN32
    static constexpr size_t kMaxHandles = 64;
#else
    static constexpr size_t kMaxHandles = 4096;
#endif
    static constexpr int kTimeout = -1;
    static constexpr int kFailed = -2;

    void clear() { handles_.clear(); }
    bool add(WaitHandle handle);  // false if it is full
    size_t size() const { return handles_.size(); }
    // The index of a handle that was signalled (the first one, if several were),
    // kTimeout, or kFailed. No timeout: wait until one is.
    int wait(std::optional<std::chrono::milliseconds> timeout);

private:
    std::vector<WaitHandle> handles_;
};

}  // namespace sub::browser::platform
