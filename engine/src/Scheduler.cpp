#include "Scheduler.h"

#include <algorithm>
#include <chrono>

#include "platform/Threads.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

// How long an idle worker waits for the next run before it sleeps: consecutive
// chunks of one callback come within microseconds, the next callback a buffer later.
constexpr auto kSpinBeforeSleep = std::chrono::microseconds(50);
// Inside a run, waiting for a node to become ready: pause, then give way.
constexpr int kPausesBeforeYield = 2000;

}  // namespace

// ---------------------------------------------------------------------------
// TaskGraph

TaskGraph::TaskGraph(int nodes, const std::vector<std::pair<int, int>>& edges)
    : first_(static_cast<size_t>(std::max(nodes, 0)) + 1, 0), inputCounts_(static_cast<size_t>(std::max(nodes, 0)), 0) {
    const int count = size();
    const auto inside = [count](const std::pair<int, int>& edge) {
        return edge.first >= 0 && edge.first < count && edge.second >= 0 && edge.second < count;
    };
    for (const auto& edge : edges) {
        if (!inside(edge)) continue;
        ++first_[static_cast<size_t>(edge.first) + 1];
        ++inputCounts_[static_cast<size_t>(edge.second)];
    }
    for (int i = 0; i < count; ++i) first_[static_cast<size_t>(i) + 1] += first_[static_cast<size_t>(i)];
    destinations_.resize(static_cast<size_t>(first_[static_cast<size_t>(count)]));
    std::vector<int> filled(first_.begin(), first_.end() - 1);
    for (const auto& edge : edges) {
        if (inside(edge)) destinations_[static_cast<size_t>(filled[static_cast<size_t>(edge.first)]++)] = edge.second;
    }
    pending_ = std::make_unique<std::atomic<int>[]>(static_cast<size_t>(count));
    queue_ = std::make_unique<std::atomic<int>[]>(static_cast<size_t>(count));
    costs_.assign(static_cast<size_t>(count), 0.f);
    ranks_.assign(static_cast<size_t>(count), 0.f);
    for (int i = 0; i < count; ++i) {
        if (inputCounts_[static_cast<size_t>(i)] == 0) roots_.push_back(i);
    }
}

void TaskGraph::orderRoots() noexcept {
    // A node's destinations come after it in the graph's order: backwards, their
    // ranks are known before its own.
    for (int i = size() - 1; i >= 0; --i) {
        float after = 0.f;
        const int* dests = destinations(i);
        for (int d = 0, n = destinationCount(i); d < n; ++d) after = std::max(after, ranks_[static_cast<size_t>(dests[d])]);
        ranks_[static_cast<size_t>(i)] = costs_[static_cast<size_t>(i)] + after;
    }
    // Insertion sort: costs change slowly, so the last run's order is nearly right.
    const auto before = [this](int a, int b) {
        const float ra = ranks_[static_cast<size_t>(a)];
        const float rb = ranks_[static_cast<size_t>(b)];
        return ra != rb ? ra > rb : a < b;
    };
    for (size_t i = 1; i < roots_.size(); ++i) {
        const int node = roots_[i];
        size_t j = i;
        for (; j > 0 && before(node, roots_[j - 1]); --j) roots_[j] = roots_[j - 1];
        roots_[j] = node;
    }
}

void TaskGraph::reset() noexcept {
    const int count = size();
    for (int i = 0; i < count; ++i) {
        pending_[i].store(inputCounts_[static_cast<size_t>(i)], std::memory_order_relaxed);
        queue_[i].store(-1, std::memory_order_relaxed);
    }
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    done_.store(0, std::memory_order_relaxed);
    for (const int node : roots_) push(node);
}

void TaskGraph::push(int node) noexcept {
    // Every node is queued once per run, so the slots never run out.
    const int slot = tail_.fetch_add(1, std::memory_order_relaxed);
    queue_[slot].store(node, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Scheduler

Scheduler::Scheduler(int threads) {
    const int workers = std::max(0, threads - 1);
    workers_.reserve(static_cast<size_t>(workers));
    try {
        for (int w = 1; w <= workers; ++w) workers_.emplace_back([this, w] { workerMain(w); });
    } catch (...) {  // a thread couldn't start: those that did end (the destructor won't run)
        state_.store(kQuit, std::memory_order_seq_cst);
        state_.notify_all();
        for (auto& worker : workers_) worker.join();
        throw;
    }
}

Scheduler::~Scheduler() {
    state_.store(kQuit, std::memory_order_seq_cst);
    state_.notify_all();
    for (auto& worker : workers_) worker.join();
}

void Scheduler::run(TaskGraph& graph, Job job, void* context, bool parallel) noexcept {
    const int size = graph.size();
    if (!parallel || workers_.empty() || size < 2) {
        for (int node = 0; node < size; ++node) job(context, node, 0);  // the graph's order: inputs first
        return;
    }
    graph.reset();
    graph_ = &graph;
    job_ = job;
    context_ = context;
    // Open the run (publishing the above), and wake the workers that sleep. A
    // worker either sees the new state before it sleeps, or is counted here.
    const uint64_t open = (++runs_ << 1) | 1;
    state_.store(open, std::memory_order_seq_cst);
    if (sleepers_.load(std::memory_order_seq_cst) > 0) state_.notify_all();

    work(0);
    while (graph.done_.load(std::memory_order_acquire) < size) cpuRelax();  // the last nodes on the workers
    // Close it, and wait for the workers still inside to leave: a worker that
    // joins counts itself first and then checks that the run is still open.
    state_.store(open & ~uint64_t{1}, std::memory_order_seq_cst);
    while (active_.load(std::memory_order_seq_cst) > 0) cpuRelax();
}

void Scheduler::work(int worker) noexcept {
    TaskGraph& graph = *graph_;
    const int size = graph.size();
    int idle = 0;
    for (;;) {
        int head = graph.head_.load(std::memory_order_acquire);
        if (head >= size) return;  // every node is taken: none will become ready any more
        const int node = graph.queue_[head].load(std::memory_order_acquire);
        if (node < 0) {  // none ready: the nodes running will queue the next
            if (++idle < kPausesBeforeYield) {
                cpuRelax();
            } else {
                std::this_thread::yield();
            }
            continue;
        }
        if (!graph.head_.compare_exchange_weak(head, head + 1, std::memory_order_acq_rel)) continue;
        idle = 0;
        job_(context_, node, worker);
        if (worker != 0) nodesOnWorkers_.fetch_add(1, std::memory_order_relaxed);
        // The last input to finish queues each destination (acq_rel: whoever runs
        // it sees what every input wrote).
        const int* dests = graph.destinations(node);
        for (int d = 0, n = graph.destinationCount(node); d < n; ++d) {
            if (graph.pending_[dests[d]].fetch_sub(1, std::memory_order_acq_rel) == 1) graph.push(dests[d]);
        }
        graph.done_.fetch_add(1, std::memory_order_release);
    }
}

void Scheduler::workerMain(int worker) noexcept {
    const platform::ScopedRealtimePriority realtime;  // (MMCSS's "Pro Audio" on Windows, as the driver's thread)
    uint64_t joined = 0;  // the last run it took part in
    for (;;) {
        uint64_t state = state_.load(std::memory_order_seq_cst);
        const auto deadline = std::chrono::steady_clock::now() + kSpinBeforeSleep;
        for (int spins = 1; state != kQuit && (!(state & 1) || state == joined); ++spins) {
            if (spins % 64 == 0 && std::chrono::steady_clock::now() > deadline) break;
            cpuRelax();
            state = state_.load(std::memory_order_seq_cst);
        }
        if (state == kQuit) break;
        if ((state & 1) && state != joined) {
            active_.fetch_add(1, std::memory_order_seq_cst);
            if (state_.load(std::memory_order_seq_cst) == state) {  // still open: the run's data is valid
                ScopedNoDenormals noDenormals;
                work(worker);
            }
            active_.fetch_sub(1, std::memory_order_seq_cst);
            joined = state;
            continue;
        }
        sleepers_.fetch_add(1, std::memory_order_seq_cst);
        state_.wait(state, std::memory_order_seq_cst);  // returns at once if a run opened meanwhile
        sleepers_.fetch_sub(1, std::memory_order_seq_cst);
    }
}

}  // namespace sub
