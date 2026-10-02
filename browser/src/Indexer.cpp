#include "Indexer.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>

#include "Text.h"

namespace sub::browser {

using namespace std::chrono_literals;

namespace {

constexpr auto kSettle = 250ms;         // after the last change seen, before looking
constexpr auto kSettleAtMost = 1000ms;  // after the first
constexpr auto kSaveAfter = 5s;
constexpr double kFirstPublishMs = 30.0;   // while scanning, with no files shown yet
constexpr double kPublishEveryMs = 150.0;  // while scanning; more if building takes long

bool hidden(std::wstring_view name) { return !name.empty() && (name[0] == L'.' || name[0] == L'$'); }

std::wstring joinPath(const std::wstring& folder, std::wstring_view name) {
    std::wstring path = folder;
    const wchar_t last = path.empty() ? L'\0' : path.back();
    if (last != L'\\' && last != L'/' && last != L':') path += L'\\';
    path += name;
    return path;
}

std::string joinKey(const std::string& folderKey, std::wstring_view name) {
    std::string key = folderKey;
    if (key.empty() || key.back() != '\\') key += '\\';
    key += ntLower(name);
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

uint64_t fnv1a(std::string_view data) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char c : data) hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    return hash;
}

class Writer {
public:
    std::string data;
    void u8(uint8_t v) { data.push_back(static_cast<char>(v)); }
    void u32(uint32_t v) { data.append(reinterpret_cast<const char*>(&v), sizeof v); }
    void u64(uint64_t v) { data.append(reinterpret_cast<const char*>(&v), sizeof v); }
    void str(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        data.append(s);
    }
};

class Reader {
public:
    explicit Reader(std::string_view data) : p_(data.data()), end_(data.data() + data.size()) {}
    bool ok() const { return ok_; }
    uint8_t u8() { return take<uint8_t>(); }
    uint32_t u32() { return take<uint32_t>(); }
    uint64_t u64() { return take<uint64_t>(); }
    std::string str() {
        const uint32_t n = u32();
        if (!ok_ || static_cast<size_t>(end_ - p_) < n) {
            ok_ = false;
            return {};
        }
        std::string s(p_, n);
        p_ += n;
        return s;
    }

private:
    template <typename T>
    T take() {
        T v{};
        if (static_cast<size_t>(end_ - p_) < sizeof(T)) {
            ok_ = false;
            return v;
        }
        std::memcpy(&v, p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }
    const char* p_;
    const char* end_;
    bool ok_ = true;
};

}  // namespace

Indexer::Indexer(std::wstring store, Limits limits, std::function<void()> changed)
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
    for (;;) {
        std::optional<std::vector<PlaceSpec>> places;
        bool rescan = false;
        uint64_t target = 0;
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
        std::vector<HANDLE> handles{static_cast<HANDLE>(wake_.handle())};
        std::vector<size_t> watched;
        for (size_t p = 0; p < places_.size() && handles.size() < MAXIMUM_WAIT_OBJECTS; ++p) {
            if (places_[p].watcher && places_[p].watcher->ok()) {
                handles.push_back(static_cast<HANDLE>(places_[p].watcher->event()));
                watched.push_back(p);
            }
        }
        auto deadline = Clock::time_point::max();
        if (changesWaiting_) deadline = std::min(lastChange_ + kSettle, firstChange_ + kSettleAtMost);
        if (unsaved_) deadline = std::min(deadline, saveAt_);
        DWORD timeout = INFINITE;
        if (deadline != Clock::time_point::max()) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            timeout = static_cast<DWORD>(std::clamp<long long>(ms + 1, 0, 60'000));
        }
        const DWORD woke = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout);
        if (woke > WAIT_OBJECT_0 && woke < WAIT_OBJECT_0 + handles.size())
            takeWatcherChanges(places_[watched[woke - WAIT_OBJECT_0 - 1]]);
        else if (woke == WAIT_FAILED)
            Sleep(50);
        if (unsaved_ && Clock::now() >= saveAt_) save();
    }
    if (unsaved_) save();
    places_.clear();  // stops watching
}

// --- The tree -----------------------------------------------------------------------

uint32_t Indexer::addNode(std::wstring path, std::wstring name, std::string key, std::string detail, int32_t parent) {
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

void Indexer::setPath(Node& n, std::wstring path) {
    n.path = std::move(path);
    n.pathUtf8 = toUtf8(n.path);
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
            const std::wstring root = toWide(place.spec.root);
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
                place.node = addNode(root, L"", place.spec.key, place.spec.detail, -1);
            }
            place.watcher = std::make_unique<platform::FolderWatcher>(root);
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
    std::vector<std::wstring> folders;
    if (time && platform::listFolder(n.path, entries)) {
        for (auto& entry : entries) {
            if (hidden(entry.name)) continue;
            if (entry.folder) {
                folders.push_back(std::move(entry.name));
                continue;
            }
            std::string name = toUtf8(entry.name);
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
            std::string detail = toUtf8(name);
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
    std::vector<std::wstring> paths;
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

void Indexer::markChanged(const Place& place, const std::wstring& relative) {
    // The folder the change was in, or the nearest one above it that is known
    // (new folders are found by listing that one).
    std::wstring_view leaf = relative;
    size_t cut = leaf.find_last_of(L'\\');
    std::wstring_view folder = cut == std::wstring_view::npos ? std::wstring_view() : leaf.substr(0, cut);
    leaf = cut == std::wstring_view::npos ? leaf : leaf.substr(cut + 1);
    if (hidden(leaf)) return;  // never listed
    for (;;) {
        const std::string key = folder.empty() ? place.spec.key : joinKey(place.spec.key, folder);
        const auto known = byKey_.find(key);
        if (known != byKey_.end()) {
            node(known->second).dirty = true;
            return;
        }
        if (folder.empty()) return;
        cut = folder.find_last_of(L'\\');
        const std::wstring_view below = cut == std::wstring_view::npos ? folder : folder.substr(cut + 1);
        if (hidden(below)) return;  // inside a folder that is never listed
        folder = cut == std::wstring_view::npos ? std::wstring_view() : folder.substr(0, cut);
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

    Writer w;
    w.data.append(kMagic, sizeof kMagic);
    w.u32(kFormat);
    w.u32(static_cast<uint32_t>(limits_.extensions.size()));
    for (const auto& ext : limits_.extensions) w.str(ext);
    w.u32(count);
    for (const auto& n : nodes_) {
        if (!n) continue;
        w.u32(static_cast<uint32_t>(n->parent >= 0 ? index[static_cast<size_t>(n->parent)] : -1));
        w.str(n->pathUtf8);
        w.str(toUtf8(n->name));
        w.str(n->key);
        w.str(n->detail);
        w.u64(n->time);
        w.u8(n->listed && !n->dirty ? 1 : 0);  // changes not looked at yet: list it next time
        w.u32(static_cast<uint32_t>(n->children.size()));
        for (const uint32_t child : n->children) w.u32(static_cast<uint32_t>(index[child]));
        w.u32(static_cast<uint32_t>(n->files->size()));
        for (size_t f = 0; f < n->files->size(); ++f) w.str(n->files->name(f));
    }
    w.u64(fnv1a(w.data));

    const std::filesystem::path path(store_);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const std::filesystem::path temp = path.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(w.data.data(), static_cast<std::streamsize>(w.data.size()));
        if (!out) return;
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool Indexer::load() {
    if (store_.empty()) return false;
    std::string data;
    {
        std::ifstream in(std::filesystem::path(store_), std::ios::binary);
        if (!in) return false;
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (data.size() < sizeof kMagic + 12 || std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) return false;
    const std::string_view body(data.data(), data.size() - 8);
    uint64_t checksum;
    std::memcpy(&checksum, data.data() + body.size(), sizeof checksum);
    if (checksum != fnv1a(body)) return false;

    Reader r(body.substr(sizeof kMagic));
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
        const uint32_t id = addNode(toWide(l.path), toWide(l.name), std::move(l.key), std::move(l.detail), l.parent);
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
