#pragma once
// Routing as a graph: strips are nodes, and where a strip's output goes is an
// edge (a track into a group's bus; later sends, sidechains, resampling). The
// engine sees only edges, never groups. The graph must be acyclic: an edge
// that would close a cycle is refused where it is made (wouldCycle), and the
// snapshot builder sorts the graph so that every strip comes after its inputs.
//
// Delay compensation happens at every summing point, bottom-up: a strip's
// signal arrives `arrival = inputLatency + its own devices' latency` late; a
// summing point (a bus, the master, later a rack) hears its inputs as late as
// the latest of them, and delays each of the others by the difference.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace gil {

// Lines up the inputs of one summing point: `arrivals[i]` is how late input i
// arrives; `compensation[i]` (same size) gets how much it must be delayed to
// arrive with the latest. Returns how late the summing point hears its inputs
// (0 without any). Racks use it for their chains as buses do for their tracks.
inline int alignInputs(const std::vector<int>& arrivals, std::vector<int>& compensation) {
    const int latest = arrivals.empty() ? 0 : *std::max_element(arrivals.begin(), arrivals.end());
    compensation.resize(arrivals.size());
    for (size_t i = 0; i < arrivals.size(); ++i) compensation[i] = latest - arrivals[i];
    return latest;
}

// Node i's output goes to node outputs[i], or to the master (-1). Returns the
// nodes in an order in which each comes after every node that feeds it (inputs
// before their bus), keeping the given order where it can. Empty (unless there
// are no nodes) if there is a cycle.
inline std::vector<int> topologicalOrder(const std::vector<int>& outputs) {
    const int count = static_cast<int>(outputs.size());
    std::vector<int> pending(outputs.size(), 0);  // inputs not placed yet
    for (const int out : outputs) {
        if (out >= 0 && out < count) ++pending[out];
    }
    std::vector<int> order;
    order.reserve(outputs.size());
    std::vector<int> ready;  // a stack, seeded in reverse so the given order is kept
    for (int i = count - 1; i >= 0; --i) {
        if (pending[i] == 0) ready.push_back(i);
    }
    while (!ready.empty()) {
        const int node = ready.back();
        ready.pop_back();
        order.push_back(node);
        const int out = outputs[node];
        if (out >= 0 && out < count && --pending[out] == 0) ready.push_back(out);
    }
    if (static_cast<int>(order.size()) != count) order.clear();  // a cycle
    return order;
}

// Whether routing node `from` into node `to` (-1: the master) would close a
// cycle: `to` already feeds `from`, or is `from` itself.
inline bool wouldCycle(const std::vector<int>& outputs, int from, int to) {
    for (int node = to, steps = 0; node >= 0 && steps <= static_cast<int>(outputs.size()); ++steps) {
        if (node == from) return true;
        node = outputs[node];
    }
    return false;
}

// Delay compensation for a whole graph, bottom-up. `order` from
// topologicalOrder(); `latency[i]` is what node i's own devices add.
struct GraphLatencies {
    std::vector<int> inputLatency;  // how late each node hears its input (0: nothing feeds it)
    std::vector<int> compensation;  // how much each node is delayed to line up at its output's summing point
    int masterInput = 0;            // how late the master hears its inputs
};

inline GraphLatencies alignGraph(const std::vector<int>& order, const std::vector<int>& outputs,
                                 const std::vector<int>& latency) {
    const size_t count = outputs.size();
    GraphLatencies result;
    result.inputLatency.assign(count, 0);
    result.compensation.assign(count, 0);
    // Each summing point's inputs and their arrivals. Inputs come before their
    // bus in `order`, so a bus's input latency is known before its own arrival.
    std::vector<std::vector<int>> inputs(count + 1);  // the last one: the master
    for (const int node : order) inputs[outputs[node] >= 0 ? outputs[node] : count].push_back(node);
    std::vector<int> arrivals, compensation;
    const auto align = [&](size_t point) {
        arrivals.clear();
        for (const int input : inputs[point]) arrivals.push_back(result.inputLatency[input] + latency[input]);
        const int heard = alignInputs(arrivals, compensation);
        for (size_t i = 0; i < inputs[point].size(); ++i) result.compensation[inputs[point][i]] = compensation[i];
        return heard;
    };
    for (const int node : order) result.inputLatency[node] = align(static_cast<size_t>(node));
    result.masterInput = align(count);
    return result;
}

}  // namespace gil
