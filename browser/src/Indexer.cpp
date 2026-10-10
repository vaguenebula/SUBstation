#include "Indexer.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <numeric>
#include <utility>

#include "Text.h"
#include "platform/Bytes.h"

namespace sub::browser {

using namespace std::chrono_literals;

namespace {

using NativeString = platform::NativeString;
using NativeChar = platform::NativeChar;
using NativeView = platform::NativeStringView;

constexpr auto kSettle = 250ms;         // after the last change seen, before looking
constexpr auto kSettleAtMost = 1000ms;  // after the first
constexpr auto kSaveAfter = 5s;
constexpr double kFirstPublishMs = 30.0;   // while scanning, with no files shown yet
constexpr double kPublishEveryMs = 150.0;  // while scanning; more if building takes long
constexpr std::chrono::milliseconds kWaitAtMost = 60s;  // it looks at its deadlines again at least this often
constexpr NativeChar kSeparator = static_cast<NativeChar>(platform::kSeparator);

NativeString joinPath(const NativeString& folder, NativeView name) {
    NativeString path = folder;
    platform::appendName(path, name);
    return path;
}

// A folder's key and a name (or names, joined by the separator) below it.
std::string joinKey(const std::string& folderKey, NativeView name) {
    std::string key = folderKey;
    if (key.empty() || key.back() != platform::kSeparator) key += platform::kSeparator;
    key += platform::pathKey(platform::fromNative(name));
    return key;
}

const std::shared_ptr<const FolderFiles>& noFiles() {
    static const auto empty = FolderFiles::make({});
    return empty;
}

// --- The saved index -------------------------------------------------------------
// "GILBIDX1", format, the extensions it was made with, the folders, and a
// checksum. Anything unexpected and it's ignored: the folders are listed again.

constexpr char kMagic[8] = {'G', 'I', 'L', 'B', 'I', 'D', 'X', '1'};
constexpr uint32_t kFormat = 1;

using platform::ByteReader;
using platform::ByteWriter;
using platform::fnv1a;

}  // namespace

Indexer::Indexer(std::string store, Limits limits, std::function<void()> changed)
    : store_(std::move(store)), limits_(std::move(limits)), changed_(std::move(changed)) {
    status_.busy = true;
    thread_ = std::thread([this] { run(); });
}

Indexer::~Indexer() { close(); }

void Indexer::setPlaces(std::vector<PlaceSpec> places) {
    {
        std::lock_guard lock(mutex_);
        newPlaces_ = std::move(places);
        ++requested_;
        status_.busy = true;
    }
    interrupt_ = true;
    wake_.set();
}

void Indexer::rescan() {
    {
        std::lock_guard lock(mutex_);
        rescanRequested_ = true;
        ++requested_;
        status_.busy = true;
    }
    interrupt_ = true;
    wake_.set();
}

void Indexer::close() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    interrupt_ = true;
    wake_.set();
    if (thread_.joinable()) thread_.join();
    idle_.notify_all();
}

std::shared_ptr<const Snapshot> Indexer::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

IndexStatus Indexer::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

bool Indexer::waitIdle(double seconds) {
    std::unique_lock lock(mutex_);
    return idle_.wait_for(lock, std::chrono::duration<double>(seconds),
                          [this] { return stop_ || (done_ >= requested_ && !scheduled_); });
}

bool Indexer::pendingCommands() const { return interrupt_.load(std::memory_order_relaxed); }

// --- The thread ---------------------------------------------------------------------

void Indexer::run() {
    platform::enterBackgroundMode();
    const auto loadStart = Clock::now();
    if (load()) {
        std::lock_guard lock(mutex_);
        status_.loadMs = std::chrono::duration<double, std::milli>(Clock::now() - loadStart).count();
    }
    bool needPass = true;
    platform::Waiter waiter;
    for (;;) {
        std::optional<std::vector<PlaceSpec>> places;
        bool rescan = false;
        uint64_t target = 0;
        // Unset before the commands are taken: one asked for after this sets it
        // again, so the wait below can't miss it.
        wake_.reset();
        {
            std::lock_guard lock(mutex_);
            if (stop_) break;
            places.swap(newPlaces_);
            rescan = std::exchange(rescanRequested_, false);
            target = requested_;
            interrupt_ = false;
        }
        if (places) {
            applyPlaces(std::move(*places));
            needPass = true;
        }
        if (rescan) {
            for (auto& n : nodes_)
                if (n) n->dirty = true;
            needPass = true;
        }
        const auto now = Clock::now();
        if (changesWaiting_ && (now >= lastChange_ + kSettle || now >= firstChange_ + kSettleAtMost)) {
            changesWaiting_ = false;
            needPass = true;
        }

        if (needPass) {
            if (placesChanged_) {
                placesChanged_ = false;
                publishNow();  // what is known already (the saved index), at once
            }
            // Watch new places before their folders are looked at, but after what
            // is known is shown: on Linux watching a tree means walking it.
            rewatch();
            bool changed = false;
            listed_ = checked_ = 0;
            const auto passStart = Clock::now();
            if (!updatePass(changed)) continue;  // commands came in: take them first
            const double passMs = std::chrono::duration<double, std::milli>(Clock::now() - passStart).count();
            const bool removed = removeUnreachable();
            if (changed || version_ == 0) publishNow();
            if (changed || removed) {
                unsaved_ = true;
                saveAt_ = Clock::now() + kSaveAfter;
            }
            rewatch();
            needPass = false;
            {
                std::lock_guard lock(mutex_);
                done_ = target;
                scheduled_ = changesWaiting_;
                status_.busy = done_ < requested_;
                status_.passMs = passMs;
                status_.listed = listed_;
                status_.checked = checked_;
            }
            idle_.notify_all();
            changed_();
            continue;
        }

        // Wait for a command, a change in a place, or a deadline.
        waiter.clear();
        waiter.add(wake_.handle());
        std::vector<size_t> watched;
        for (size_t p = 0; p < places_.size() && waiter.size() < platform::Waiter::kMaxHandles; ++p) {
            if (places_[p].watcher && places_[p].watcher->ok()) {
                waiter.add(places_[p].watcher->handle());
                watched.push_back(p);
            }
        }
        auto deadline = Clock::time_point::max();
        if (changesWaiting_) deadline = std::min(lastChange_ + kSettle, firstChange_ + kSettleAtMost);
        if (unsaved_) deadline = std::min(deadline, saveAt_);
        std::optional<std::chrono::milliseconds> timeout;
        if (deadline != Clock::time_point::max()) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            timeout = std::chrono::milliseconds(std::clamp<long long>(ms + 1, 0, kWaitAtMost.count()));
        }
        const int woke = waiter.wait(timeout);
        if (woke > 0)
            takeWatcherChanges(places_[watched[static_cast<size_t>(woke) - 1]]);
        else if (woke == platform::Waiter::kFailed)
            std::this_thread::sleep_for(50ms);
        if (unsaved_ && Clock::now() >= saveAt_) save();
    }
    if (unsaved_) save();
    places_.clear();  // stops watching
}

// --- The tree -----------------------------------------------------------------------

uint32_t Indexer::addNode(NativeString path, NativeString name, std::string key, std::string detail, int32_t parent) {
    uint32_t id;
    if (!free_.empty()) {
        id = free_.back();
        free_.pop_back();
    } else {
        id = static_cast<uint32_t>(nodes_.size());
        nodes_.emplace_back();
    }
    auto n = std::make_unique<Node>();
    n->name = std::move(name);
    n->key = std::move(key);
    n->detail = std::move(detail);
    n->detailLower = pyLower(n->detail);
    n->parent = parent;
    n->files = noFiles();
    setPath(*n, std::move(path));
    byKey_[n->key] = id;
    nodes_[id] = std::move(n);
    return id;
}

void Indexer::setPath(Node& n, NativeString path) {
    n.path = std::move(path);
    n.pathUtf8 = platform::fromNative(n.path);
    n.pathLower = pyLower(n.pathUtf8);
}

bool Indexer::removeUnreachable() {
    const uint32_t mark = ++pass_;
    bool removed = false;
    std::vector<uint32_t> stack;
    for (const Place& place : places_) stack.push_back(place.node);
    while (!stack.empty()) {
        const uint32_t id = stack.back();
        stack.pop_back();
        Node* n = nodes_[id].get();
        if (!n || n->mark == mark) continue;
        n->mark = mark;
        stack.insert(stack.end(), n->children.begin(), n->children.end());
    }
    for (uint32_t id = 0; id < nodes_.size(); ++id) {
        if (nodes_[id] && nodes_[id]->mark != mark) {
            byKey_.erase(nodes_[id]->key);
            nodes_[id].reset();
            free_.push_back(id);
            removed = true;
        }
    }
    for (auto& n : nodes_)
        if (n && n->parent >= 0 && !nodes_[static_cast<size_t>(n->parent)]) n->parent = -1;
    return removed;
}

void Indexer::applyPlaces(std::vector<PlaceSpec> specs) {
    std::vector<Place> places;
    for (auto& spec : specs) {
        // Keep what is known of a place that stays (and its watcher).
        auto same = std::find_if(places_.begin(), places_.end(), [&](const Place& p) {
            return p.watcher && p.spec.key == spec.key && p.spec.root == spec.root;
        });
        Place place;
        if (same != places_.end()) {
            place = std::move(*same);
            place.spec = std::move(spec);
        } else {
            place.spec = std::move(spec);
            const NativeString root = platform::toNative(place.spec.root);
            const auto known = byKey_.find(place.spec.key);
            if (known != byKey_.end()) {
                place.node = known->second;
                Node& n = node(place.node);
                if (n.parent < 0) {  // shown as this place's root
                    setPath(n, root);
                    n.detail = place.spec.detail;
                    n.detailLower = pyLower(n.detail);
                }
            } else {
                place.node = addNode(root, NativeString(), place.spec.key, place.spec.detail, -1);
            }
            // Its watcher comes with the next rewatch(), before the pass looks at it.
        }
        places.push_back(std::move(place));
    }
    places_ = std::move(places);  // the watchers of places gone stop here
    placesChanged_ = true;
}

bool Indexer::updatePass(bool& changed) {
    for (const Place& place : places_) {
        uint64_t count = 0;
        std::vector<std::pair<uint32_t, uint32_t>> stack{{place.node, 0}};
        while (!stack.empty() && count < limits_.maxFiles) {
            if (pendingCommands()) return false;
            const auto [id, depth] = stack.back();
            stack.pop_back();
            Node* n = nodes_[id].get();
            if (!n) continue;
            bool again = !n->listed || n->dirty;
            if (!again && n->checked != round_) {
                const auto time = platform::folderTime(n->path);
                n->checked = round_;
                ++checked_;
                again = !time || *time != n->time;
            }
            if (again) {
                ++listed_;
                if (list(id)) changed = true;
                publishWhileScanning();
            }
            count += n->files->size();
            if (depth < limits_.maxDepth)
                for (const uint32_t child : n->children) stack.push_back({child, depth + 1});
        }
    }
    return true;
}

bool Indexer::list(uint32_t id) {
    Node& n = node(id);
    const auto time = platform::folderTime(n.path);
    std::vector<platform::Entry> entries;
    std::vector<FolderFiles::Name> files;
    std::vector<NativeString> folders;
    if (time && platform::listFolder(n.path, entries)) {
        for (auto& entry : entries) {
            if (platform::hiddenName(entry.name)) continue;
            if (entry.folder) {
                folders.push_back(std::move(entry.name));
                continue;
            }
            std::string name = platform::fromNative(entry.name);
            std::string lower = pyLower(name);
            const bool audio = std::any_of(limits_.extensions.begin(), limits_.extensions.end(),
                                           [&](const std::string& ext) { return lower.ends_with(ext); });
            if (audio) files.push_back({std::move(name), std::move(lower)});
        }
    }
    bool changed = false;
    if (!n.files->sameNames(files)) {
        foundFiles_ += files.size();
        foundFiles_ -= std::min<uint64_t>(foundFiles_, n.files->size());
        n.files = FolderFiles::make(std::move(files));
        changed = true;
    }
    std::vector<uint32_t> children;
    children.reserve(folders.size());
    for (auto& name : folders) {
        std::string key = joinKey(n.key, name);
        const auto known = byKey_.find(key);
        if (known != byKey_.end()) {
            Node& child = node(known->second);
            if (child.parent < 0 && known->second != id) child.parent = static_cast<int32_t>(id);
            children.push_back(known->second);
        } else {
            std::string detail = platform::fromNative(name);
            children.push_back(addNode(joinPath(n.path, name), name, std::move(key), std::move(detail),
                                       static_cast<int32_t>(id)));
        }
    }
    if (children != n.children) {
        changed = true;
        n.children = std::move(children);
    }
    n.time = time.value_or(0);
    n.listed = true;
    n.dirty = false;
    n.checked = round_;
    return changed;
}

std::shared_ptr<Snapshot> Indexer::buildSnapshot() {
    auto snap = std::make_shared<Snapshot>();
    struct Ref {
        std::string_view lower;
        uint32_t folder, file;
    };
    std::vector<Ref> refs;
    std::vector<int32_t> order(nodes_.size(), -1);
    // The Python walk, over what is known: depth first, last folder first, at
    // most maxFiles files a place. Files keep the order the walk found them in.
    for (const Place& place : places_) {
        uint64_t count = 0;
        std::vector<std::pair<uint32_t, uint32_t>> stack{{place.node, 0}};
        while (!stack.empty() && count < limits_.maxFiles) {
            const auto [id, depth] = stack.back();
            stack.pop_back();
            const Node* n = nodes_[id].get();
            if (!n) continue;
            const auto files = static_cast<uint32_t>(n->files->size());
            count += files;
            if (files && order[id] < 0) {
                order[id] = static_cast<int32_t>(snap->folders.size());
                snap->folders.push_back({n->files, n->pathUtf8, n->pathLower, n->key, n->detail, n->detailLower});
                for (uint32_t f = 0; f < files; ++f)
                    refs.push_back({n->files->lower(f), static_cast<uint32_t>(order[id]), f});
            }
            if (depth < limits_.maxDepth)
                for (const uint32_t child : n->children) stack.push_back({child, depth + 1});
        }
    }
    // The list's own order: sorted by lower-case name, stable over the walk.
    std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
        if (const int c = a.lower.compare(b.lower)) return c < 0;
        return a.folder != b.folder ? a.folder < b.folder : a.file < b.file;
    });
    snap->audio.reserve(refs.size());
    for (const Ref& ref : refs) snap->audio.push_back({ref.folder, ref.file});
    refs = {};
    // And by casefolded name, stable over the own order. Files whose casefolded
    // name is their lower-case name are in that order already; only the others
    // (names with ß, ligatures...) need sorting, and then merging in.
    auto fold = [&](uint32_t i) {
        const AudioRef r = snap->audio[i];
        return snap->folders[r.folder].files->fold(r.file);
    };
    auto byFold = [&](uint32_t a, uint32_t b) {
        if (const int c = fold(a).compare(fold(b))) return c < 0;
        return a < b;
    };
    std::vector<uint32_t> plain, other;
    plain.reserve(snap->audio.size());
    for (uint32_t i = 0; i < snap->audio.size(); ++i) {
        const AudioRef r = snap->audio[i];
        (snap->folders[r.folder].files->foldIsLower(r.file) ? plain : other).push_back(i);
    }
    if (other.empty()) {
        snap->byFold = std::move(plain);
    } else {
        std::sort(other.begin(), other.end(), byFold);
        snap->byFold.reserve(snap->audio.size());
        std::merge(plain.begin(), plain.end(), other.begin(), other.end(), std::back_inserter(snap->byFold), byFold);
    }
    for (uint32_t f = 0; f < snap->folders.size(); ++f) {
        snap->folderByKey.emplace(snap->folders[f].key, f);
        snap->folderByPath.emplace(snap->folders[f].path, f);
    }
    return snap;
}

void Indexer::publish(std::shared_ptr<Snapshot> snap) {
    snap->version = ++version_;
    const auto files = static_cast<uint32_t>(snap->audio.size());
    const auto folders = static_cast<uint32_t>(snap->folders.size());
    {
        std::lock_guard lock(mutex_);
        snapshot_ = std::move(snap);
        status_.version = version_;
        status_.files = files;
        status_.folders = folders;
        status_.buildMs = buildMs_;
    }
    lastPublish_ = Clock::now();
    publishedFiles_ = files;
    changed_();
}

void Indexer::publishNow() {
    const auto start = Clock::now();
    auto snap = buildSnapshot();
    buildMs_ = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    publish(std::move(snap));
}

void Indexer::publishWhileScanning() {
    // The first files found soon, then less often the longer a snapshot takes to make.
    const double interval = publishedFiles_ == 0 ? kFirstPublishMs : std::max(kPublishEveryMs, 4.0 * buildMs_);
    if (std::chrono::duration<double, std::milli>(Clock::now() - lastPublish_).count() < interval) return;
    if (foundFiles_ == publishedFiles_) return;  // nothing new to show (roughly: it counts all it listed)
    publishNow();
}

// --- Watching -------------------------------------------------------------------------

void Indexer::takeWatcherChanges(Place& place) {
    std::vector<NativeString> paths;
    const auto result = place.watcher->take(paths);
    if (result == platform::FolderWatcher::Changes::Paths) {
        if (paths.empty()) return;
        for (const auto& path : paths) markChanged(place, path);
    } else {
        // Too many changes to tell, or it stopped watching: compare every folder's time.
        ++round_;
        if (result == platform::FolderWatcher::Changes::Failed) place.watcher.reset();
    }
    const auto now = Clock::now();
    if (!changesWaiting_) firstChange_ = now;
    changesWaiting_ = true;
    lastChange_ = now;
    std::lock_guard lock(mutex_);
    scheduled_ = true;
}

void Indexer::markChanged(const Place& place, const NativeString& relative) {
    // The folder the change was in, or the nearest one above it that is known
    // (new folders are found by listing that one).
    NativeView leaf = relative;
    size_t cut = leaf.find_last_of(kSeparator);
    NativeView folder = cut == NativeView::npos ? NativeView() : leaf.substr(0, cut);
    leaf = cut == NativeView::npos ? leaf : leaf.substr(cut + 1);
    if (platform::hiddenName(leaf)) return;  // never listed
    for (;;) {
        const std::string key = folder.empty() ? place.spec.key : joinKey(place.spec.key, folder);
        const auto known = byKey_.find(key);
        if (known != byKey_.end()) {
            node(known->second).dirty = true;
            return;
        }
        if (folder.empty()) return;
        cut = folder.find_last_of(kSeparator);
        const NativeView below = cut == NativeView::npos ? folder : folder.substr(cut + 1);
        if (platform::hiddenName(below)) return;  // inside a folder that is never listed
        folder = cut == NativeView::npos ? NativeView() : folder.substr(0, cut);
    }
}

void Indexer::rewatch() {
    for (Place& place : places_) {
        if (place.watcher && place.watcher->ok()) continue;
        const Node& root = node(place.node);
        if (!platform::folderTime(root.path)) continue;
        place.watcher = std::make_unique<platform::FolderWatcher>(root.path);
    }
}

// --- The saved index ------------------------------------------------------------------

void Indexer::save() {
    unsaved_ = false;
    saveAt_ = Clock::time_point::max();
    if (store_.empty()) return;
    std::vector<int32_t> index(nodes_.size(), -1);
    uint32_t count = 0;
    for (uint32_t id = 0; id < nodes_.size(); ++id)
        if (nodes_[id]) index[id] = static_cast<int32_t>(count++);

    ByteWriter w;
    w.raw(kMagic, sizeof kMagic);
    w.u32(kFormat);
    w.u32(static_cast<uint32_t>(limits_.extensions.size()));
    for (const auto& ext : limits_.extensions) w.str(ext);
    w.u32(count);
    for (const auto& n : nodes_) {
        if (!n) continue;
        w.u32(static_cast<uint32_t>(n->parent >= 0 ? index[static_cast<size_t>(n->parent)] : -1));
        w.str(n->pathUtf8);
        w.str(platform::fromNative(n->name));
        w.str(n->key);
        w.str(n->detail);
        w.u64(n->time);
        w.u8(n->listed && !n->dirty ? 1 : 0);  // changes not looked at yet: list it next time
        w.u32(static_cast<uint32_t>(n->children.size()));
        for (const uint32_t child : n->children) w.u32(static_cast<uint32_t>(index[child]));
        w.u32(static_cast<uint32_t>(n->files->size()));
        for (size_t f = 0; f < n->files->size(); ++f) w.str(n->files->name(f));
    }
    w.u64(fnv1a(w.bytes));

    platform::writeFileAtomically(store_, w.bytes);  // a store that can't be written only costs a scan
}

bool Indexer::load() {
    if (store_.empty()) return false;
    const std::optional<std::string> read = platform::readFile(store_);
    if (!read) return false;
    const std::string& data = *read;
    if (data.size() < sizeof kMagic + 12 || std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) return false;
    const std::string_view body(data.data(), data.size() - 8);
    if (ByteReader(std::string_view(data).substr(body.size())).u64() != fnv1a(body)) return false;

    ByteReader r(body.substr(sizeof kMagic));
    if (r.u32() != kFormat) return false;
    std::vector<std::string> extensions(r.u32());
    for (auto& ext : extensions) ext = r.str();
    if (!r.ok() || extensions != limits_.extensions) return false;  // other files would be listed

    struct Loaded {
        int32_t parent;
        std::string path, name, key, detail;
        uint64_t time;
        bool listed;
        std::vector<uint32_t> children;
        std::vector<FolderFiles::Name> files;
    };
    const uint32_t count = r.u32();
    if (!r.ok() || count > body.size()) return false;
    std::vector<Loaded> loaded(count);
    for (auto& l : loaded) {
        l.parent = static_cast<int32_t>(r.u32());
        l.path = r.str();
        l.name = r.str();
        l.key = r.str();
        l.detail = r.str();
        l.time = r.u64();
        l.listed = r.u8() != 0;
        const uint32_t children = r.u32();
        if (!r.ok() || children > count) return false;
        l.children.resize(children);
        for (auto& c : l.children) c = r.u32();
        const uint32_t files = r.u32();
        if (!r.ok() || files > body.size()) return false;
        l.files.resize(files);
        for (auto& f : l.files) {
            f.name = r.str();
            f.lower = pyLower(f.name);
        }
        if (!r.ok()) return false;
        if (l.parent >= static_cast<int32_t>(count) ||
            std::any_of(l.children.begin(), l.children.end(), [&](uint32_t c) { return c >= count; }))
            return false;
    }
    for (auto& l : loaded) {
        const uint32_t id = addNode(platform::toNative(l.path), platform::toNative(l.name), std::move(l.key),
                                    std::move(l.detail), l.parent);
        Node& n = node(id);
        n.children = std::move(l.children);
        foundFiles_ += l.files.size();
        n.files = l.files.empty() ? noFiles() : FolderFiles::make(std::move(l.files));
        n.time = l.time;
        n.listed = l.listed;
        n.checked = 0;  // compare every folder's time with the disk
    }
    return true;
}

}  // namespace sub::browser
