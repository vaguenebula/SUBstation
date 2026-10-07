#include "similarity/SoundIndex.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/AudioReader.h"
#include "similarity/SoundStore.h"

namespace sub::intelligence {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kProgressWakeInterval = std::chrono::milliseconds(250);
constexpr auto kWaitForLoad = std::chrono::seconds(10);
constexpr auto kWaitForLibrary = std::chrono::seconds(3);

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Clock::duration seconds(double s) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); }

}  // namespace

// --- SimilarityResult ------------------------------------------------------------------

uint64_t SimilarityResult::pathHash(std::string_view path) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (const char c : path) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h ? h : 1;  // (0 marks an empty slot)
}

void SimilarityResult::setScores(const std::vector<std::pair<uint64_t, float>>& scores) {
    size_t capacity = 16;
    while (capacity < scores.size() * 2) capacity <<= 1;
    keys_.assign(capacity, 0);
    values_.assign(capacity, 0.f);
    mask_ = capacity - 1;
    for (const auto& [key, value] : scores) {
        uint64_t slot = key & mask_;
        while (keys_[slot] != 0 && keys_[slot] != key) slot = (slot + 1) & mask_;
        keys_[slot] = key;
        values_[slot] = value;
    }
}

float SimilarityResult::similarity(std::string_view path) const {
    if (keys_.empty()) return std::numeric_limits<float>::quiet_NaN();
    const uint64_t key = pathHash(path);
    for (uint64_t slot = key & mask_;; slot = (slot + 1) & mask_) {
        if (keys_[slot] == key) return values_[slot];
        if (keys_[slot] == 0) return std::numeric_limits<float>::quiet_NaN();
    }
}

// --- SoundIndex ------------------------------------------------------------------------

SoundIndex::SoundIndex(SoundIndexOptions options) : options_(std::move(options)) {
    analysers_ = options_.threads ? options_.threads : std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
    if (!options_.analyse) analysers_ = 0;
    for (unsigned i = 0; i < analysers_; ++i) workers_.emplace_back([this] { workerLoop(); });
    keeper_ = std::thread([this] { keeperLoop(); });
    search_ = std::thread([this] { searchLoop(); });
}

SoundIndex::~SoundIndex() { close(); }

void SoundIndex::setWakeCallback(std::function<void()> wake) {
    std::function<void()> now;
    {
        std::lock_guard lock(wakeMutex_);
        wakeCallback_ = std::move(wake);
        if (signalled_) now = wakeCallback_;
    }
    if (now) now();
}

void SoundIndex::wake() {
    std::function<void()> callback;
    {
        std::lock_guard lock(wakeMutex_);
        if (signalled_) return;
        signalled_ = true;
        callback = wakeCallback_;
    }
    if (callback) callback();
}

void SoundIndex::wakeForProgress() {
    {
        std::lock_guard lock(wakeMutex_);
        const auto now = Clock::now();
        if (now - lastProgressWake_ < kProgressWakeInterval) return;
        lastProgressWake_ = now;
    }
    wake();
}

void SoundIndex::setLibrarySource(LibrarySource source) {
    {
        std::lock_guard lock(sourceMutex_);
        source_ = std::move(source);
    }
    libraryChanged();
}

void SoundIndex::libraryChanged() {
    std::lock_guard lock(mutex_);
    libraryDirty_ = true;
    keeperWake_.notify_all();
}

void SoundIndex::setLibrary(Library files) {
    auto shared = std::make_shared<const Library>(std::move(files));
    setLibrarySource([shared] { return shared; });
}

uint64_t SoundIndex::find(SoundQuery query) {
    std::lock_guard lock(mutex_);
    const uint64_t generation = ++latest_;
    pending_ = {generation, std::move(query)};
    finished_.reset();
    searchWake_.notify_all();
    return generation;
}

SoundIndex::Update SoundIndex::take() {
    Update update;
    {
        std::lock_guard lock(wakeMutex_);
        signalled_ = false;
    }
    std::lock_guard lock(mutex_);
    update.status = statusLocked();
    if (finished_ && finished_->generation == latest_.load()) update.result = std::move(finished_);
    finished_.reset();
    return update;
}

SoundIndexStatus SoundIndex::status() const {
    std::lock_guard lock(mutex_);
    return statusLocked();
}

SoundIndexStatus SoundIndex::statusLocked() const {
    SoundIndexStatus s = counts_;
    s.busy = busyLocked();
    s.analysing = !loaded_ || running_ > 0 || !analyseQueue_.empty() || !checkQueue_.empty();
    return s;
}

bool SoundIndex::busyLocked() const {
    return !loaded_ || libraryDirty_ || refreshing_ || running_ > 0 || !analyseQueue_.empty() || !checkQueue_.empty();
}

bool SoundIndex::searching() const {
    std::lock_guard lock(mutex_);
    return pending_.has_value() || searchRunning_;
}

bool SoundIndex::waitIdle(double secondsToWait) {
    std::unique_lock lock(mutex_);
    return idle_.wait_for(lock, seconds(secondsToWait),
                          [this] { return closed_ || (!busyLocked() && !pending_ && !searchRunning_); });
}

void SoundIndex::close() {
    {
        std::lock_guard lock(mutex_);
        if (closed_) return;
        stop_ = true;
        keeperWake_.notify_all();
        workerWake_.notify_all();
        searchWake_.notify_all();
    }
    for (auto& t : workers_) t.join();
    if (search_.joinable()) search_.join();
    if (keeper_.joinable()) keeper_.join();  // (it saves what changed last)
    {
        std::lock_guard lock(wakeMutex_);
        wakeCallback_ = nullptr;
    }
    std::lock_guard lock(mutex_);
    closed_ = true;
    idle_.notify_all();
}

// --- Entries -----------------------------------------------------------------------------

uint32_t SoundIndex::addEntry(std::string path) {
    const auto index = static_cast<uint32_t>(entries_.size());
    Entry entry;
    entry.hash = SimilarityResult::pathHash(path);
    entry.path = std::move(path);
    byPath_.emplace(entry.path, index);
    entries_.push_back(std::move(entry));
    fingerprints_.resize(entries_.size() * kDims, 0.f);
    return index;
}

void SoundIndex::setState(Entry& entry, State state) {
    if (entry.inLibrary) {
        if (entry.state == State::Analysed) --counts_.analysed;
        if (entry.state == State::Failed) --counts_.failed;
        if (state == State::Analysed) ++counts_.analysed;
        if (state == State::Failed) ++counts_.failed;
    }
    entry.state = state;
}

void SoundIndex::changed() {
    ++counts_.version;
    if (!saveDue_) {
        saveDue_ = true;
        saveAt_ = Clock::now() + seconds(options_.saveDelaySeconds);
        keeperWake_.notify_all();
    }
}

void SoundIndex::applyLibrary(const Library& files) {
    const bool analyse = analysers_ > 0;  // (else the files wait: nothing analyses them)
    for (Entry& entry : entries_) entry.inLibrary = false;
    for (const std::string& path : files) {
        const auto found = byPath_.find(std::string_view(path));
        const uint32_t index = found == byPath_.end() ? addEntry(path) : found->second;
        Entry& entry = entries_[index];
        entry.inLibrary = true;
        if (!analyse) continue;
        if (entry.state == State::Pending) {
            if (!entry.queued) {
                entry.queued = true;
                analyseQueue_.push_back(index);
            }
        } else if (!entry.verified && !entry.checking) {
            entry.checking = true;
            checkQueue_.push_back(index);
        }
    }
    counts_.library = counts_.analysed = counts_.failed = 0;
    for (const Entry& entry : entries_) {
        if (!entry.inLibrary) continue;
        ++counts_.library;
        if (entry.state == State::Analysed) ++counts_.analysed;
        if (entry.state == State::Failed) ++counts_.failed;
    }
    workerWake_.notify_all();
}

// --- The keeper --------------------------------------------------------------------------

void SoundIndex::load() {
    const auto start = Clock::now();
    std::optional<std::vector<StoredSound>> stored;
    if (!options_.store.empty()) stored = readStore(options_.store);
    std::lock_guard lock(mutex_);
    if (stored) {
        entries_.reserve(stored->size());
        fingerprints_.reserve(stored->size() * kDims);
        for (StoredSound& s : *stored) {
            if (byPath_.count(std::string_view(s.path))) continue;
            const uint32_t index = addEntry(std::move(s.path));
            Entry& entry = entries_[index];
            entry.stamp = s.stamp;
            entry.state = s.analysed ? State::Analysed : State::Failed;
            entry.reference = s.reference;
            entry.used = s.used;
            used_ = std::max(used_, s.used);
            if (s.analysed) std::copy(s.fingerprint.begin(), s.fingerprint.end(), fingerprints_.begin() + index * kDims);
        }
    }
    counts_.loadMs = millisecondsSince(start);
    loaded_ = true;
    searchWake_.notify_all();
    idle_.notify_all();
}

void SoundIndex::save(std::unique_lock<std::mutex>& lock) {
    saveDue_ = false;
    if (options_.store.empty()) return;
    // What is kept: the library's files, and the references searched from most recently.
    std::vector<uint32_t> keep, references;
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (e.state == State::Pending) continue;
        if (e.inLibrary) keep.push_back(i);
        else if (e.reference) references.push_back(i);
    }
    if (references.size() > options_.maxReferences) {
        std::nth_element(references.begin(), references.begin() + static_cast<ptrdiff_t>(options_.maxReferences),
                         references.end(), [this](uint32_t a, uint32_t b) { return entries_[a].used > entries_[b].used; });
        references.resize(options_.maxReferences);
    }
    keep.insert(keep.end(), references.begin(), references.end());
    StoreWriter writer(static_cast<uint32_t>(keep.size()));
    StoredSound sound;
    for (const uint32_t i : keep) {
        const Entry& e = entries_[i];
        sound.path = e.path;
        sound.stamp = e.stamp;
        sound.analysed = e.state == State::Analysed;
        sound.reference = e.reference;
        sound.used = e.used;
        if (sound.analysed)
            std::copy(fingerprints_.begin() + i * kDims, fingerprints_.begin() + (i + 1) * kDims, sound.fingerprint.begin());
        writer.add(sound);
    }
    const std::string bytes = writer.finish();
    lock.unlock();
    writeStore(options_.store, bytes);  // (a failed save is tried again after the next change)
    lock.lock();
}

void SoundIndex::keeperLoop() {
    if (options_.background) platform::enterBackgroundMode();
    load();
    wake();
    std::unique_lock lock(mutex_);
    for (;;) {
        const auto now = Clock::now();
        const bool refreshNow = libraryDirty_ && now >= nextRefresh_;
        const bool saveNow = saveDue_ && now >= saveAt_;
        if (stop_) break;
        if (refreshNow) {
            libraryDirty_ = false;
            refreshing_ = true;
            lock.unlock();
            std::shared_ptr<const Library> files;
            {
                std::lock_guard source(sourceMutex_);
                if (source_) files = source_();
            }
            lock.lock();
            if (files) {
                applyLibrary(*files);
                haveLibrary_ = true;
            }
            refreshing_ = false;
            nextRefresh_ = Clock::now() + seconds(options_.refreshSeconds);
            searchWake_.notify_all();
            idle_.notify_all();
            lock.unlock();
            wake();
            lock.lock();
            continue;
        }
        if (saveNow) {
            save(lock);
            continue;
        }
        auto deadline = Clock::time_point::max();
        if (libraryDirty_) deadline = std::min(deadline, nextRefresh_);
        if (saveDue_) deadline = std::min(deadline, saveAt_);
        if (deadline == Clock::time_point::max())
            keeperWake_.wait(lock);
        else
            keeperWake_.wait_until(lock, deadline);
    }
    if (saveDue_) save(lock);
}

// --- The analysers -----------------------------------------------------------------------

void SoundIndex::workerLoop() {
    if (options_.background) platform::enterBackgroundMode();
    SoundAnalyzer analyzer;
    std::unique_lock lock(mutex_);
    for (;;) {
        workerWake_.wait(lock, [this] { return stop_ || (loaded_ && (!analyseQueue_.empty() || !checkQueue_.empty())); });
        if (stop_) break;
        // New files first, then checking the saved ones against their files.
        uint32_t index = 0;
        bool check = false, wanted = true;
        if (!analyseQueue_.empty()) {
            index = analyseQueue_.front();
            analyseQueue_.pop_front();
            Entry& e = entries_[index];
            e.queued = false;
            wanted = e.inLibrary && e.state == State::Pending;  // (a search may have analysed it meanwhile)
        } else {
            index = checkQueue_.front();
            checkQueue_.pop_front();
            Entry& e = entries_[index];
            e.checking = false;
            wanted = e.inLibrary && !e.verified && e.state != State::Pending;
            check = true;
        }
        if (!wanted) {
            if (!busyLocked()) {
                idle_.notify_all();
                lock.unlock();
                wake();
                lock.lock();
            }
            continue;
        }
        const std::string path = entries_[index].path;
        const platform::FileStamp saved = entries_[index].stamp;
        ++running_;
        lock.unlock();

        const std::optional<platform::FileStamp> stamp = platform::stamp(path);
        bool analysed = false;
        std::optional<Fingerprint> fingerprint;
        if (!check || !stamp || *stamp != saved) {
            analysed = true;
            if (stamp) {
                try {
                    fingerprint = analyzer.analyzeFile(path);
                } catch (const AudioError&) {
                } catch (const std::exception&) {
                }
            }
        }

        lock.lock();
        --running_;
        Entry& e = entries_[index];
        e.verified = true;
        if (analysed) {
            e.stamp = stamp.value_or(platform::FileStamp{});
            if (fingerprint)
                std::copy(fingerprint->begin(), fingerprint->end(), fingerprints_.begin() + index * kDims);
            setState(e, fingerprint ? State::Analysed : State::Failed);
            ++counts_.analysedThisRun;
            changed();
        }
        const bool idleNow = !busyLocked();
        if (idleNow) idle_.notify_all();
        lock.unlock();
        if (idleNow) wake();
        else if (analysed) wakeForProgress();
        lock.lock();
    }
}

// --- Searches ------------------------------------------------------------------------------

void SoundIndex::searchLoop() {
    std::unique_lock lock(mutex_);
    for (;;) {
        searchWake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
        if (stop_) break;
        // The saved fingerprints first, and the first library if it is on its way.
        searchWake_.wait_for(lock, kWaitForLoad, [this] { return stop_ || loaded_; });
        searchWake_.wait_for(lock, kWaitForLibrary,
                             [this] { return stop_ || haveLibrary_ || (!libraryDirty_ && !refreshing_); });
        if (stop_ || !pending_) continue;
        auto [generation, query] = std::move(*pending_);
        pending_.reset();
        searchRunning_ = true;
        lock.unlock();
        std::shared_ptr<SimilarityResult> result;
        try {
            result = runSearch(generation, query);
        } catch (const std::exception& e) {
            result = std::make_shared<SimilarityResult>();
            result->generation = generation;
            result->query = query;
            result->error = e.what();
        }
        lock.lock();
        searchRunning_ = false;
        const bool deliver = result && generation == latest_.load() && !stop_;
        if (deliver) finished_ = std::move(result);
        idle_.notify_all();
        if (deliver) {
            lock.unlock();
            wake();
            lock.lock();
        }
    }
}

std::shared_ptr<SimilarityResult> SoundIndex::runSearch(uint64_t generation, const SoundQuery& query) {
    const auto start = Clock::now();
    auto result = std::make_shared<SimilarityResult>();
    result->generation = generation;
    result->query = query;

    // The sound's fingerprint: its saved one if it is a whole file whose
    // fingerprint is up to date, else made now.
    std::optional<Fingerprint> reference;
    std::optional<platform::FileStamp> stamp;
    if (query.wholeFile()) {
        stamp = platform::stamp(query.path);
        std::lock_guard lock(mutex_);
        const auto found = byPath_.find(std::string_view(query.path));
        if (found != byPath_.end()) {
            Entry& e = entries_[found->second];
            if (e.state == State::Analysed && stamp && *stamp == e.stamp) {
                Fingerprint fp;
                std::copy(fingerprints_.begin() + found->second * kDims, fingerprints_.begin() + (found->second + 1) * kDims,
                          fp.begin());
                reference = fp;
                if (e.reference) e.used = ++used_;
            }
        }
    }
    if (!reference) {
        static thread_local SoundAnalyzer analyzer;  // (only this thread searches)
        try {
            reference = analyzer.analyzeFile(query.path, query.start, query.length);
            if (!reference) result->error = "silent";
        } catch (const AudioError& e) {
            result->error = e.what();
        }
        if (latest_.load() != generation) return nullptr;
        if (reference && query.wholeFile() && stamp) {
            // Kept, so the next search from it (or the analysers) needn't analyse it again.
            std::lock_guard lock(mutex_);
            const auto found = byPath_.find(std::string_view(query.path));
            const uint32_t index = found == byPath_.end() ? addEntry(query.path) : found->second;
            Entry& e = entries_[index];
            std::copy(reference->begin(), reference->end(), fingerprints_.begin() + index * kDims);
            e.stamp = *stamp;
            e.verified = true;
            if (!e.inLibrary) {
                e.reference = true;
                e.used = ++used_;
            }
            setState(e, State::Analysed);
            changed();
        }
    }
    if (!reference) {
        result->searchMs = millisecondsSince(start);
        return result;
    }

    // Every analysed library file against it.
    std::vector<std::pair<uint64_t, float>> scores;
    std::vector<std::pair<float, uint32_t>> ranked;
    {
        std::lock_guard lock(mutex_);
        if (latest_.load() != generation) return nullptr;
        std::vector<uint32_t> rows;
        rows.reserve(counts_.analysed);
        for (uint32_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].inLibrary && entries_[i].state == State::Analysed) rows.push_back(i);
        result->libraryFiles = counts_.library;
        const Comparison comparison = Comparison::fit(fingerprints_.data(), entries_.size(), &rows, options_.weights);
        scores.reserve(rows.size());
        ranked.reserve(rows.size());
        for (const uint32_t i : rows) {
            const float similarity =
                Comparison::similarity(comparison.distance(reference->data(), fingerprints_.data() + i * kDims));
            scores.push_back({entries_[i].hash, similarity});
            if (entries_[i].path != query.path) ranked.push_back({similarity, i});
        }
        const size_t best = std::min(ranked.size(), SimilarityResult::kBest);
        std::partial_sort(ranked.begin(), ranked.begin() + static_cast<ptrdiff_t>(best), ranked.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
        result->best.reserve(best);
        for (size_t i = 0; i < best; ++i) result->best.push_back({entries_[ranked[i].second].path, ranked[i].first});
    }
    result->scored = scores.size();
    result->setScores(scores);
    result->searchMs = millisecondsSince(start);
    return result;
}

}  // namespace sub::intelligence
