// What the index needs from the operating system: listing folders and their
// times, background priority, waking its thread, watching folder trees for
// changes, and how file names compare. Platform.cpp is Windows' (the Win32
// calls the browser always used), PlatformPosix.cpp everyone else's (Linux:
// inotify; other POSIX systems list and check folder times but don't watch).
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

namespace sub::browser::platform {

#ifdef _WIN32
using NativeString = std::wstring;  // UTF-16, as the wide (W) calls take it
using WaitHandle = void*;           // a HANDLE
inline constexpr char kSeparator = '\\';
// Names that differ only in case are the same file.
inline constexpr bool kCaseSensitivePaths = false;
#else
using NativeString = std::string;  // bytes, as the file system has them (UTF-8 in practice)
using WaitHandle = int;            // a file descriptor
inline constexpr char kSeparator = '/';
// Names that differ only in case are different files (Linux file systems).
inline constexpr bool kCaseSensitivePaths = true;
#endif

std::string toUtf8(const NativeString& s);
NativeString fromUtf8(std::string_view s);

// Whether `c` separates folders in a path: '\' and '/' on Windows, '/' elsewhere.
bool isSeparator(char c);

// Item keys (os.path.normcase), for use counts.
//
// On Windows a name in a key is Windows' own lower case (LCMapStringEx with the
// invariant locale), which is what os.path.normcase() used: names that differ
// only in case are the same file there, so they have one key. Elsewhere a name
// is as it is: file systems there are case-sensitive, so "Kick.wav" and
// "kick.wav" are two files and must keep two keys (and two use counts). This
// is also what os.path.normcase() does on POSIX.
std::string nameKey(std::string_view name);
// A whole path (already normalised, as os.path.normpath does): on Windows '/'
// becomes '\' and every name goes to Windows' lower case; elsewhere it is as it is.
std::string pathKey(std::string_view path);

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

// The calling thread's CPU, I/O and memory priority to background: it yields to
// everything else, the audio thread and disk reads for playback first. (Linux:
// the lowest nice value and the idle I/O class, for this thread only.)
void enterBackgroundMode();

// Moves `from` over `to`, replacing it at once (the saved index is written to a
// temporary file first, so a crash never leaves half of one). False on failure.
bool replaceFile(const NativeString& from, const NativeString& to);

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
