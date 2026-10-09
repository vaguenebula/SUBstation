// The index of the audio files under the browser's places.
//
// One thread, at background priority (CPU and I/O), keeps a tree of the folders
// under the places, each with the audio files it holds. The tree is saved to a
// file, so the next start shows it at once; then only folders whose last-write
// time changed are listed again. While running, the places are watched, and
// folders where something was added, removed or renamed are listed again.
//
// Searches never read the tree: the thread publishes immutable snapshots of it
// (also while a long scan is still running, so results show early).
//
// What is listed follows the Python walk it replaces: from each place, depth
// first, the last folder first; at most `maxDepth` folders deep, and no further
// folders once `maxFiles` files were found under a place; names starting with
// '.' or '$' are skipped; junctions are walked into, symbolic links to folders
// are not.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Model.h"
#include "Platform.h"

namespace sub::browser {

struct Limits {
    uint32_t maxFiles = 300000;  // per place
    uint32_t maxDepth = 16;
    std::vector<std::string> extensions;  // lower case, with the dot
};

struct PlaceSpec {
    std::string root;    // as given (and as its files' paths start)
    std::string key;     // os.path.normcase(os.path.normpath(root))
    std::string detail;  // os.path.basename(root): its files' detail
};

struct IndexStatus {
    bool busy = false;      // places being set up, scanned or checked after a request
    uint64_t version = 0;   // of the snapshot
    uint32_t files = 0;
    uint32_t folders = 0;
    // How long things took, for benchmarks.
    double loadMs = 0.0;    // reading the saved index
    double buildMs = 0.0;   // making the last snapshot
    double passMs = 0.0;    // the last walk over the places
    uint32_t listed = 0;    // folders it listed
    uint32_t checked = 0;   // folders whose time it compared
};

class Indexer {
public:
    // `store`: where the index is saved (UTF-8; '' for nowhere). `changed` is
    // called (from the indexer's thread) when a snapshot or the status changed.
    Indexer(std::string store, Limits limits, std::function<void()> changed);
    ~Indexer();
    Indexer(const Indexer&) = delete;
    Indexer& operator=(const Indexer&) = delete;

    void setPlaces(std::vector<PlaceSpec> places);
    void rescan();  // list every folder again
    void close();   // stop (saving the index); idempotent

    std::shared_ptr<const Snapshot> snapshot() const;
    IndexStatus status() const;
    // Until nothing is asked for, scheduled or running (tests, benchmarks).
    bool waitIdle(double seconds);

private:
    using Clock = std::chrono::steady_clock;

    struct Node {
        platform::NativeString path;  // as shown
        platform::NativeString name;  // in its parent ('' for a place's root)
        std::string key;    // normcase(path)
        std::string pathUtf8, pathLower, detail, detailLower;
        int32_t parent = -1;
        std::vector<uint32_t> children;  // its folders, in listing order
        std::shared_ptr<const FolderFiles> files;
        uint64_t time = 0;  // last-write time when it was listed
        bool listed = false;
        bool dirty = false;  // list again
        uint32_t checked = 0;  // the check round in which its time was last compared
        uint32_t mark = 0;
    };

    struct Place {
        PlaceSpec spec;
        uint32_t node = 0;
        std::unique_ptr<platform::FolderWatcher> watcher;
    };

    void run();
    bool pendingCommands() const;

    // The tree
    uint32_t addNode(platform::NativeString path, platform::NativeString name, std::string key, std::string detail,
                     int32_t parent);
    void setPath(Node& node, platform::NativeString path);
    bool removeUnreachable();  // true if any folder went
    Node& node(uint32_t id) { return *nodes_[id]; }

    void applyPlaces(std::vector<PlaceSpec> specs);
    // Walks the places as the Python walk did, listing folders that are new,
    // marked dirty or changed on disk. False if it stopped for a command.
    bool updatePass(bool& changed);
    // Lists a folder. False if the folder is gone.
    bool list(uint32_t id);
    std::shared_ptr<Snapshot> buildSnapshot();
    void publish(std::shared_ptr<Snapshot> snapshot);
    void publishNow();  // builds and publishes
    void publishWhileScanning();

    // Watching
    void takeWatcherChanges(Place& place);
    void markChanged(const Place& place, const platform::NativeString& relative);
    // Starts watching places that aren't watched (new ones, or whose watcher
    // failed), once their root is there.
    void rewatch();

    // The saved index
    bool load();
    void save();

    const std::string store_;  // the saved index (UTF-8)
    const Limits limits_;
    const std::function<void()> changed_;

    // Shared with other threads
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::optional<std::vector<PlaceSpec>> newPlaces_;
    bool rescanRequested_ = false;
    bool stop_ = false;
    uint64_t requested_ = 1;  // commands asked for (the first: loading the saved index)
    uint64_t done_ = 0;       // commands finished; busy while behind
    bool scheduled_ = false;  // work from watchers is waiting or running
    std::shared_ptr<const Snapshot> snapshot_;
    IndexStatus status_;
    platform::Event wake_;  // a command came in (the thread waits for it and the watchers)
    std::thread thread_;
    std::atomic<bool> interrupt_{false};  // commands are waiting: a pass should stop and let them in

    // The indexer's own
    std::vector<std::unique_ptr<Node>> nodes_;  // null: free
    std::vector<uint32_t> free_;
    std::unordered_map<std::string, uint32_t> byKey_;
    std::vector<Place> places_;
    uint32_t round_ = 1;  // folders whose `checked` differs get their time compared
    uint32_t pass_ = 0;
    uint64_t version_ = 0;
    bool placesChanged_ = false;
    bool unsaved_ = false;
    Clock::time_point lastPublish_{};
    double buildMs_ = 0.0;
    uint32_t listed_ = 0, checked_ = 0;  // in the pass running
    uint64_t foundFiles_ = 0;      // in all folders listed
    uint64_t publishedFiles_ = 0;  // in the last snapshot
    Clock::time_point firstChange_{}, lastChange_{};
    bool changesWaiting_ = false;
    Clock::time_point saveAt_ = Clock::time_point::max();
};

}  // namespace sub::browser
