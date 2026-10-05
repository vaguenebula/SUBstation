// The browser's backend: the index of files, what else the browser lists, use
// counts, and searches over all of it.
//
// Threads: the indexer's (background priority) and one for searches. Neither
// ever calls into the application or touches the audio engine, except for the
// wake callback. A search asked for replaces any that is waiting and stops the
// one running; only the latest search's results are handed out. When there are
// results or the index changed, the wake callback is called (from one of those
// threads), and the application takes them on its own thread with `take()`.

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "Indexer.h"
#include "Model.h"
#include "Search.h"

namespace sub::browser {

class Browser {
public:
    // `store`: where the index is saved (UTF-8; '' for nowhere).
    Browser(std::string store, Limits limits);
    ~Browser();
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

    // Called from the browser's threads when there is something to take():
    // results of the latest search, or a change of the index or its status.
    // It is called once until the next take() (as a set event stays set), and
    // must only hand the work over (post it to the application's thread), never
    // call back into the browser. Once setWakeCallback() returned, the previous
    // callback is no longer called. If something waits to be taken already, the
    // new callback is called at once.
    void setWakeCallback(std::function<void()> wake);

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
    void close();                   // stops the threads (saving the index); no wake calls after it

private:
    void searchLoop();
    void wake();

    // The wake callback, and whether it was called since the last take().
    // (Before the indexer: its thread may call wake() as soon as it starts.)
    std::mutex wakeMutex_;
    std::function<void()> wakeCallback_;
    bool signalled_ = false;

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

}  // namespace sub::browser
