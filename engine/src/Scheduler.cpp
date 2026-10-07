#include "Scheduler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <xmmintrin.h>

#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#endif

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

void TaskGraph::orderRoots(bool byRank) noexcept {
    // A node's destinations come after it in the graph's order: backwards, their
    // ranks are known before its own.
    float total = 0.f, longest = 0.f;
    for (int i = size() - 1; i >= 0; --i) {
        float after = 0.f;
        const int* dests = destinations(i);
        for (int d = 0, n = destinationCount(i); d < n; ++d) after = std::max(after, ranks_[static_cast<size_t>(dests[d])]);
        const float cost = costs_[static_cast<size_t>(i)];
        ranks_[static_cast<size_t>(i)] = cost + after;
        total += cost;
        longest = std::max(longest, cost + after);
    }
    parallelism_ = longest > 0.f ? static_cast<int>(std::min(std::ceil(total / longest), 1e6f)) + 1 : 0;
    // Insertion sort: costs change slowly, so the last run's order is nearly right.
    const auto before = [this, byRank](int a, int b) {
        const float ra = byRank ? ranks_[static_cast<size_t>(a)] : 0.f;
        const float rb = byRank ? ranks_[static_cast<size_t>(b)] : 0.f;
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
    const int workers = std::clamp(threads - 1, 0, kMaxWorkers);
    slots_ = std::make_unique<Slot[]>(static_cast<size_t>(workers));
    workers_.reserve(static_cast<size_t>(workers));
    try {
        for (int w = 1; w <= workers; ++w) workers_.emplace_back([this, w] { workerMain(w); });
    } catch (...) {  // a thread couldn't start: those that did end (the destructor won't run)
        quit();
        throw;
    }
}

Scheduler::~Scheduler() { quit(); }

void Scheduler::quit() {
    state_.store(kQuit, std::memory_order_seq_cst);
    for (int w = 1; w <= static_cast<int>(workers_.size()); ++w) wake(w);
    for (auto& worker : workers_) worker.join();
}

void Scheduler::run(TaskGraph& graph, Job job, void* context, int threads) noexcept {
    const int size = graph.size();
    const int helpers = std::clamp(threads - 1, 0, static_cast<int>(workers_.size()));
    if (helpers == 0 || size < 2) {
        for (int node = 0; node < size; ++node) job(context, node, 0);  // the graph's order: inputs first
        return;
    }
    graph.reset();
    graph_ = &graph;
    job_ = job;
    context_ = context;
    // Open the run for the first `helpers` workers (publishing the above), and
    // wake the first of them: it wakes the next (wakeAfter()), and they theirs.
    const uint64_t open = (++runs_ << kRunShift) | (static_cast<uint64_t>(helpers) << 1) | 1;
    state_.store(open, std::memory_order_seq_cst);
    wake(1);

    work(0);
    while (graph.done_.load(std::memory_order_acquire) < size) _mm_pause();  // the last nodes on the workers
    // Close it, and wait for the workers still inside to leave: a worker that
    // joins counts itself first and then checks that the run is still open.
    state_.store(open & ~uint64_t{1}, std::memory_order_seq_cst);
    while (active_.load(std::memory_order_seq_cst) > 0) _mm_pause();
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
            // One node left (the last bus, say): the thread finishing what it
            // waits for takes it, so a worker has nothing more to wait for here.
            if (worker != 0 && size - head <= 1) return;
            if (++idle < kPausesBeforeYield) {
                _mm_pause();
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

void Scheduler::wake(int worker) noexcept {
    Slot& slot = slots_[static_cast<size_t>(worker) - 1];
    slot.wake.fetch_add(1, std::memory_order_seq_cst);
    if (slot.sleeping.load(std::memory_order_seq_cst)) slot.wake.notify_one();  // (spinning: it sees the state)
}

void Scheduler::wakeAfter(int worker, uint64_t state) noexcept {
    const int last = std::min(kWakeFanOut * worker + 1, workersOf(state));
    for (int next = kWakeFanOut * (worker - 1) + 2; next <= last; ++next) {
        if (state_.load(std::memory_order_relaxed) != state) return;  // the run is over: let them sleep
        wake(next);
    }
}

void Scheduler::workerMain(int worker) noexcept {
#ifdef _WIN32
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
#endif
    Slot& slot = slots_[static_cast<size_t>(worker) - 1];
    uint64_t joined = 0;  // the last run it took part in
    // An open run it hasn't taken part in yet, using at least this many workers.
    const auto mayJoin = [&joined, worker](uint64_t state) {
        return state != kQuit && (state & 1) && state != joined && workersOf(state) >= worker;
    };
    for (;;) {
        uint64_t state = state_.load(std::memory_order_seq_cst);
        const auto deadline = std::chrono::steady_clock::now() + kSpinBeforeSleep;
        for (int spins = 1; state != kQuit && !mayJoin(state); ++spins) {
            if (spins % 64 == 0 && std::chrono::steady_clock::now() > deadline) break;
            _mm_pause();
            state = state_.load(std::memory_order_seq_cst);
        }
        if (state == kQuit) break;
        if (mayJoin(state)) {
            wakeAfter(worker, state);  // first, and outside the run: the caller doesn't wait for it
            active_.fetch_add(1, std::memory_order_seq_cst);
            if (state_.load(std::memory_order_seq_cst) == state) {  // still open: the run's data is valid
                ScopedNoDenormals noDenormals;
                work(worker);
            }
            active_.fetch_sub(1, std::memory_order_seq_cst);
            joined = state;
            continue;
        }
        // Sleep until woken. A wake() that finds it not sleeping yet came before
        // it looks at the state again, which then shows the run; one after it
        // has changed `wake` from the token, so wait() returns at once.
        const uint32_t token = slot.wake.load(std::memory_order_seq_cst);
        slot.sleeping.store(true, std::memory_order_seq_cst);
        state = state_.load(std::memory_order_seq_cst);
        if (state != kQuit && !mayJoin(state)) slot.wake.wait(token, std::memory_order_seq_cst);
        slot.sleeping.store(false, std::memory_order_seq_cst);
    }
#ifdef _WIN32
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
#endif
}

}  // namespace sub
