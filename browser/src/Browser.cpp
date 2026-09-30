#include "Browser.h"

namespace gil::browser {

Browser::Browser(std::wstring store, Limits limits)
    : indexer_(std::move(store), std::move(limits), [this] { event_.set(); }) {
    thread_ = std::thread([this] { searchLoop(); });
}

Browser::~Browser() { close(); }

void Browser::close() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        latest_.fetch_add(1);  // stops a search that runs
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    idle_.notify_all();
    indexer_.close();
}

void Browser::setExternal(int group, std::vector<ExternalItem> items) {
    auto prepared = std::make_shared<ExternalGroup>();
    for (auto& item : items) item.prepare();
    prepared->items = std::move(items);
    std::lock_guard lock(mutex_);
    groups_[group] = std::move(prepared);
}

void Browser::setUsage(std::vector<UsageRecord> records, double halfLifeDays) {
    auto usage = std::make_shared<Usage>();
    usage->halfLifeDays = halfLifeDays;
    usage->records = std::move(records);
    for (uint32_t r = 0; r < usage->records.size(); ++r) usage->byKey.emplace(usage->records[r].key, r);
    std::lock_guard lock(mutex_);
    usage->version = ++usageVersion_;
    usage_ = std::move(usage);
}

uint64_t Browser::search(Query query) {
    uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        generation = latest_.fetch_add(1) + 1;  // a running search sees it and stops
        pending_.emplace(generation, std::move(query));
        finished_.reset();
    }
    wake_.notify_one();
    return generation;
}

Browser::Update Browser::take() {
    Update update;
    update.status = indexer_.status();
    std::lock_guard lock(mutex_);
    if (finished_ && finished_->generation == latest_.load()) update.result = std::move(finished_);
    finished_.reset();
    return update;
}

bool Browser::searching() const {
    std::lock_guard lock(mutex_);
    return pending_.has_value() || running_;
}

bool Browser::waitIdle(double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    if (!indexer_.waitIdle(seconds)) return false;
    std::unique_lock lock(mutex_);
    return idle_.wait_until(lock, deadline, [this] { return stop_ || (!pending_ && !running_); });
}

void Browser::searchLoop() {
    UsageCache cache;
    for (;;) {
        std::pair<uint64_t, Query> job;
        SearchInputs inputs;
        {
            std::unique_lock lock(mutex_);
            running_ = false;
            idle_.notify_all();
            wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
            if (stop_) return;
            job = std::move(*pending_);
            pending_.reset();
            running_ = true;
            inputs.groups = groups_;
            inputs.usage = usage_;
        }
        inputs.snapshot = indexer_.snapshot();
        auto result = runSearch(job.second, job.first, inputs, latest_, cache);
        if (!result) continue;  // a newer search replaced it
        {
            std::lock_guard lock(mutex_);
            if (job.first != latest_.load()) continue;
            finished_ = std::move(result);
        }
        event_.set();
    }
}

}  // namespace gil::browser
