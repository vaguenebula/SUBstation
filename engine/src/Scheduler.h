#pragma once
// Runs the routing graph's strips on several threads: a fixed pool of workers
// plus the thread that asks (the audio thread, or one rendering offline).
//
// The graph (TaskGraph) is the snapshot's: node i is the i-th track, and its
// edges go into other nodes (a group's bus, a return), each of which runs once
// every edge going into it is done. Per run each node's counter starts at its
// input count (its incoming edges); the nodes without inputs are queued, those
// with the most work hanging off them first (see orderRoots()); a finished node
// counts each of its destinations down and queues those reaching zero. No locks and no allocation: the counters and the queue are the
// graph's, allocated with the snapshot. What a node computes must depend only
// on its inputs (and its own state), never on which thread runs it or when, so
// that the result is the same with or without workers.
//
// Between runs workers spin briefly (consecutive chunks of a callback come
// quickly), then sleep until the next run. They join MMCSS ("Pro Audio") and
// flush denormals while they render.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace sub {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas
#endif

class TaskGraph {
public:
    // `nodes` nodes and the edges between them, (from, to); an edge to -1 (or
    // past the last node) goes nowhere the graph runs (the master). Must be
    // acyclic, and the nodes listed so that each comes after those that go into it.
    TaskGraph(int nodes, const std::vector<std::pair<int, int>>& edges);
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    int size() const noexcept { return static_cast<int>(inputCounts_.size()); }
    int inputCount(int node) const noexcept { return inputCounts_[static_cast<size_t>(node)]; }
    // The nodes node `node`'s edges go into (one per edge), in edge order.
    const int* destinations(int node) const noexcept { return destinations_.data() + first_[static_cast<size_t>(node)]; }
    int destinationCount(int node) const noexcept {
        return first_[static_cast<size_t>(node) + 1] - first_[static_cast<size_t>(node)];
    }

    // Real-time, before a run: what rendering node i costs (any unit, the same
    // for every node; 0: unknown). orderRoots() then ranks each node by the work
    // from it to the end of its longest path (its cost and the highest rank
    // among its destinations) and queues the nodes without inputs by rank, highest first, so
    // that the longest chains start first. Equal ranks keep the graph's order;
    // without costs, so does everything. The order only changes how soon a
    // run is done, never what it computes.
    void setCost(int node, float cost) noexcept { costs_[static_cast<size_t>(node)] = cost; }
    void orderRoots() noexcept;
    const std::vector<int>& roots() const noexcept { return roots_; }  // in the order they are queued
    float rank(int node) const noexcept { return ranks_[static_cast<size_t>(node)]; }

private:
    friend class Scheduler;
    void reset() noexcept;
    void push(int node) noexcept;

    // Outgoing edges as flat arrays: node i's destinations are
    // destinations_[first_[i] .. first_[i + 1]).
    std::vector<int> first_;
    std::vector<int> destinations_;
    std::vector<int> inputCounts_;
    std::vector<float> costs_, ranks_;
    std::vector<int> roots_;  // the nodes without inputs; kept from run to run (nearly sorted already)
    // Per run: inputs not done yet, per node; and the ready nodes, in the order
    // they became ready (-1: a slot not filled yet). Every node is queued once.
    std::unique_ptr<std::atomic<int>[]> pending_;
    std::unique_ptr<std::atomic<int>[]> queue_;
    alignas(64) std::atomic<int> head_{0};  // the next slot to take a node from
    alignas(64) std::atomic<int> tail_{0};  // the next slot to queue a node in
    alignas(64) std::atomic<int> done_{0};  // nodes finished
};

class Scheduler {
public:
    // Real-time: renders one node; `worker` is 0 for the thread that called
    // run(), 1.. for the pool's (an index for per-thread scratch).
    using Job = void (*)(void* context, int node, int worker) noexcept;

    // `threads` counts the caller's: 1 starts no workers (run() is serial).
    explicit Scheduler(int threads);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    int threads() const noexcept { return static_cast<int>(workers_.size()) + 1; }

    // Real-time. Runs `job` for every node of the graph, each after the nodes
    // that go into it, and returns once all are done. `parallel` false (or no
    // workers): all on the calling thread, in the graph's order. One run at a time.
    // `first` (if any) runs on the calling thread before it takes nodes, while the
    // workers start on them (perf experiments: the pipelined master).
    void run(TaskGraph& graph, Job job, void* context, bool parallel, void (*first)(void*) noexcept = nullptr) noexcept;

    // Nodes the pool's workers have rendered, since the scheduler started (for tests).
    uint64_t nodesOnWorkers() const noexcept { return nodesOnWorkers_.load(std::memory_order_relaxed); }

private:
    static constexpr uint64_t kQuit = ~uint64_t{0};

    void workerMain(int worker) noexcept;
    void work(int worker) noexcept;  // takes and runs nodes until none is left to take

    std::vector<std::thread> workers_;
    // The current run (valid while it is open).
    TaskGraph* graph_ = nullptr;
    Job job_ = nullptr;
    void* context_ = nullptr;
    // (run number << 1) | open; kQuit ends the workers. Workers sleep on it.
    alignas(64) std::atomic<uint64_t> state_{0};
    alignas(64) std::atomic<int> active_{0};    // workers inside a run
    alignas(64) std::atomic<int> sleepers_{0};  // workers waiting on state_
    std::atomic<uint64_t> nodesOnWorkers_{0};
    uint64_t runs_ = 0;
    // (perf experiments: preWakeUs) When the last parallel run opened, and the time between runs, smoothed.
    std::atomic<int64_t> lastOpenNs_{0};
    std::atomic<int64_t> periodNs_{0};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace sub
