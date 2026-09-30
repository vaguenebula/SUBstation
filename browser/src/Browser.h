// The browser's backend: the index of files, what else the browser lists, use
// counts, and searches over all of it.
//
// Threads: the indexer's (background priority) and one for searches. Neither
// ever calls into Python or touches the audio engine. A search asked for
// replaces any that is waiting and stops the one running; only the latest
// search's results are handed out. When there are results or the index
// changed, `event()` is set, and the UI thread takes them with `take()`.

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "Indexer.h"
#include "Model.h"
#include "Platform.h"
#include "Search.h"

namespace gil::browser {

class Browser {
public:
    Browser(std::wstring store, Limits limits);
    ~Browser();
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

    void* event() const { return event_.handle(); }

    void setPlaces(std::vector<PlaceSpec> places) { indexer_.setPlaces(std::move(places)); }
    void rescan() { indexer_.rescan(); }
    void setExternal(int group, std::vector<ExternalItem> items);
    void setUsage(std::vector<UsageRecord> records, double halfLifeDays);

    // Starts a search; returns its generation.
    uint64_t search(Query query);

    struct Update {
        IndexStatus status;
        std::shared_ptr<const Result> result;  // the latest search's, if it finished since the last take()
    };
    Update take();
    IndexStatus status() const { return indexer_.status(); }
    bool searching() const;

    bool waitIdle(double seconds);  // the index settled and no search running (tests, benchmarks)
    void close();

private:
    void searchLoop();

    platform::Event event_;
    Indexer indexer_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::optional<std::pair<uint64_t, Query>> pending_;
    bool running_ = false;
    bool stop_ = false;
    std::atomic<uint64_t> latest_{0};
    std::shared_ptr<const Result> finished_;
    std::unordered_map<int, std::shared_ptr<const ExternalGroup>> groups_;
    std::shared_ptr<const Usage> usage_;
    uint64_t usageVersion_ = 0;
    std::thread thread_;
};

}  // namespace gil::browser
