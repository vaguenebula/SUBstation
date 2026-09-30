// What the index needs from the operating system (Windows).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gil::browser::platform {

struct Entry {
    std::wstring name;
    bool folder = false;  // a folder to walk into: directories and junctions, not symbolic links
};

// A folder's entries, in the order the file system lists them (as os.scandir
// does). False if it can't be read.
bool listFolder(const std::wstring& path, std::vector<Entry>& out);

// When a folder's entries last changed (its last-write time, following
// junctions); nothing if it's gone.
std::optional<uint64_t> folderTime(const std::wstring& path);

// The calling thread's CPU, I/O and memory priority to background: it yields to
// everything else, the audio thread and disk reads for playback first.
void enterBackgroundMode();

// An auto-reset event: set by any thread, waited for by the UI (QWinEventNotifier).
class Event {
public:
    Event();
    ~Event();
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    void set();
    void* handle() const { return handle_; }

private:
    void* handle_ = nullptr;
};

// Watches a folder and everything below it for files and folders being added,
// removed or renamed.
class FolderWatcher {
public:
    explicit FolderWatcher(const std::wstring& root);
    ~FolderWatcher();
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    bool ok() const { return armed_; }
    void* event() const { return event_; }  // signalled when changes are there to take

    enum class Changes { Paths, Overflow, Failed };
    // After event() was signalled: the changed paths (relative to the root), or
    // Overflow (too many to tell: look at everything), or Failed (stopped watching).
    Changes take(std::vector<std::wstring>& paths);

private:
    bool arm();
    void* dir_ = nullptr;
    void* event_ = nullptr;
    void* overlapped_ = nullptr;
    std::vector<unsigned long> buffer_;
    bool armed_ = false;
};

}  // namespace gil::browser::platform
