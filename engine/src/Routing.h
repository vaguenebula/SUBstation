#pragma once
// Routing as a graph: strips are nodes, and every way a strip's signal goes on
// is an edge. Each strip has one output edge (into a group's bus, or the
// master) and any number of sends (into return tracks); a track resampling
// another takes its input on an input edge; later, sidechains are more edges.
// The engine sees only edges, never groups. The
// graph must be acyclic: an edge that would close a cycle is refused where it
// is made (wouldCycle), and the snapshot builder sorts the graph so that every
// strip comes after everything that feeds it.
//
// Delay compensation happens at every summing point, per edge: a signal leaves
// its strip `arrival = inputLatency + latency of the devices before its tap`
// late; a summing point (a bus, a return, the master, later a rack) hears its
// inputs as late as the latest of them, and delays each of the other edges by
// the difference. A strip going to two places of different latency is delayed
// differently on each edge. An input edge isn't summed (the track hears it
// instead of its clips, while monitored, and records it): it orders the graph
// and can close cycles like any edge, but isn't aligned.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace gil {

// Node `from`'s signal goes into node `to` (-1: the master).
struct RouteEdge {
    int from = 0;
    int to = -1;
    bool sums = true;  // `to` sums it into its input (false: an input edge, not aligned)
};

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

// The nodes (0..count-1) in an order in which each comes after every node that
// feeds it through any edge, keeping the given order where it can. Empty
// (unless there are no nodes) if there is a cycle.
inline std::vector<int> topologicalOrder(int count, const std::vector<RouteEdge>& edges) {
    std::vector<int> pending(static_cast<size_t>(count), 0);  // inputs not placed yet
    std::vector<std::vector<int>> destinations(static_cast<size_t>(count));
    for (const RouteEdge& edge : edges) {
        if (edge.to < 0 || edge.to >= count) continue;
        ++pending[static_cast<size_t>(edge.to)];
        destinations[static_cast<size_t>(edge.from)].push_back(edge.to);
    }
    std::vector<int> order;
    order.reserve(static_cast<size_t>(count));
    std::vector<int> ready;  // a stack, seeded in reverse so the given order is kept
    for (int i = count - 1; i >= 0; --i) {
        if (pending[static_cast<size_t>(i)] == 0) ready.push_back(i);
    }
    while (!ready.empty()) {
        const int node = ready.back();
        ready.pop_back();
        order.push_back(node);
        const auto& outs = destinations[static_cast<size_t>(node)];
        for (auto it = outs.rbegin(); it != outs.rend(); ++it) {  // reversed: the first destination comes first
            if (--pending[static_cast<size_t>(*it)] == 0) ready.push_back(*it);
        }
    }
    if (static_cast<int>(order.size()) != count) order.clear();  // a cycle
    return order;
}

// Whether a new edge from node `from` into node `to` (-1: the master) would
// close a cycle: `to` is `from`, or already feeds it along some path (through
// outputs and sends alike).
inline bool wouldCycle(int count, const std::vector<RouteEdge>& edges, int from, int to) {
    if (to < 0) return false;
    if (to == from) return true;
    std::vector<std::vector<int>> destinations(static_cast<size_t>(count));
    for (const RouteEdge& edge : edges) {
        if (edge.to >= 0 && edge.to < count) destinations[static_cast<size_t>(edge.from)].push_back(edge.to);
    }
    std::vector<char> seen(static_cast<size_t>(count), 0);
    std::vector<int> stack{to};
    seen[static_cast<size_t>(to)] = 1;
    while (!stack.empty()) {
        const int node = stack.back();
        stack.pop_back();
        for (const int next : destinations[static_cast<size_t>(node)]) {
            if (next == from) return true;
            if (!seen[static_cast<size_t>(next)]) {
                seen[static_cast<size_t>(next)] = 1;
                stack.push_back(next);
            }
        }
    }
    return false;
}

// Delay compensation for a whole graph, bottom-up. `order` from
// topologicalOrder(); `tapLatency[e]` is the latency of edge e's source devices
// before its tap (all of them, for a strip's output or a send).
struct GraphLatencies {
    std::vector<int> inputLatency;  // how late each node hears its summed inputs (0: nothing feeds it)
    std::vector<int> compensation;  // per edge: how much it is delayed to line up at its destination
    int masterInput = 0;            // how late the master hears its inputs
};

inline GraphLatencies alignGraph(const std::vector<int>& order, const std::vector<RouteEdge>& edges,
                                 const std::vector<int>& tapLatency) {
    const size_t count = order.size();
    GraphLatencies result;
    result.inputLatency.assign(count, 0);
    result.compensation.assign(edges.size(), 0);
    // Each summing point's incoming edges. Sources come before their
    // destination in `order`, so a node's input latency is known before the
    // arrivals of the edges leaving it are needed.
    std::vector<std::vector<int>> incoming(count + 1);  // the last one: the master
    for (size_t e = 0; e < edges.size(); ++e) {
        if (!edges[e].sums) continue;  // (compensation 0)
        const int to = edges[e].to;
        incoming[to >= 0 && static_cast<size_t>(to) < count ? static_cast<size_t>(to) : count].push_back(static_cast<int>(e));
    }
    std::vector<int> arrivals, compensation;
    const auto align = [&](size_t point) {
        arrivals.clear();
        for (const int e : incoming[point]) {
            arrivals.push_back(result.inputLatency[static_cast<size_t>(edges[static_cast<size_t>(e)].from)] +
                               tapLatency[static_cast<size_t>(e)]);
        }
        const int heard = alignInputs(arrivals, compensation);
        for (size_t i = 0; i < incoming[point].size(); ++i) {
            result.compensation[static_cast<size_t>(incoming[point][i])] = compensation[i];
        }
        return heard;
    };
    for (const int node : order) result.inputLatency[static_cast<size_t>(node)] = align(static_cast<size_t>(node));
    result.masterInput = align(count);
    return result;
}

}  // namespace gil
