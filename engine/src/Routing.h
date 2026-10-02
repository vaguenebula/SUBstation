#pragma once
// Routing as a graph: strips are nodes, and every way a strip's signal goes on
// is an edge. Each strip has one output edge (into a group's bus, or the
// master) and any number of sends (into return tracks); a track resampling
// another takes its input on an input edge; a device's sidechain (its aux
// input) is an edge that ends at that device, not at its strip's input. The
// engine sees only edges, never groups. The graph must be acyclic: an edge that
// would close a cycle is refused where it is made (wouldCycle), and the snapshot
// builder sorts the graph so that every strip comes after everything that feeds it.
//
// Delay compensation happens at every summing point, per edge: a signal leaves
// its strip `arrival = inputLatency + latency of the devices before its tap`
// late; a summing point (a bus, a return, the master, later a rack) hears its
// inputs as late as the latest of them, and delays each of the other edges by
// the difference. A strip going to two places of different latency is delayed
// differently on each edge. An input edge isn't summed (the track hears it
// instead of its clips, while monitored, and records it): it orders the graph
// and can close cycles like any edge, but isn't aligned. A sidechain is aligned
// where it ends: with the strip's signal at its device (as late as the strip's
// input plus the devices before it). The earlier of the two is delayed: the
// sidechain, or the strip's own signal just before the device, which makes
// everything after it on the strip that much later.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace gil {

// Node `from`'s signal goes into node `to` (-1: the master).
struct RouteEdge {
    int from = 0;
    int to = -1;
    bool sums = true;  // `to` sums it into its input (false: an input edge or a sidechain)
    // Where it leaves `from`: after the first `tap` of its devices (0: before them
    // all), as it leaves the last of them (before any delay lining up the next
    // one); -1: after all of them (an output, a send, a pre-fader tap).
    int tap = -1;
    // A sidechain: the device of `to` it goes into (its place in `to`'s chain),
    // where it is aligned; -1: none (or one not aligned: a device switched off).
    int device = -1;
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
// topologicalOrder(); `chains[n]` lists the latency of each of node n's devices
// (0 for one switched off), and `chains[count]` the master's.
struct GraphLatencies {
    std::vector<int> inputLatency;  // how late each node hears its summed inputs (0: nothing feeds it)
    std::vector<int> compensation;  // per edge: how much it is delayed to line up at its destination
    // Per edge, a sidechain's: how much its destination's signal is delayed just
    // before the device, to line up with it (0 for other edges).
    std::vector<int> deviceDelay;
    // Per node, and the master last: how late each of its devices hears the
    // strip's input (the devices before it, and the delays before sidechained
    // ones), then how late its signal leaves the last of them (its latency).
    std::vector<std::vector<int>> deviceLatency;
    int masterInput = 0;  // how late the master hears its inputs
};

inline GraphLatencies alignGraph(const std::vector<int>& order, const std::vector<RouteEdge>& edges,
                                 const std::vector<std::vector<int>>& chains) {
    const size_t count = order.size();
    GraphLatencies result;
    result.inputLatency.assign(count, 0);
    result.compensation.assign(edges.size(), 0);
    result.deviceDelay.assign(edges.size(), 0);
    result.deviceLatency.assign(count + 1, {});
    // Each summing point's incoming edges, and the sidechains into each strip's
    // devices (the last: the master's). Sources come before their destination in
    // `order`, so their latencies are known before the arrivals of the edges
    // leaving them are needed.
    const auto point = [count](int to) { return to >= 0 && static_cast<size_t>(to) < count ? static_cast<size_t>(to) : count; };
    std::vector<std::vector<int>> incoming(count + 1), sidechains(count + 1);
    for (size_t e = 0; e < edges.size(); ++e) {
        if (edges[e].sums) {
            incoming[point(edges[e].to)].push_back(static_cast<int>(e));
        } else if (edges[e].device >= 0) {
            sidechains[point(edges[e].to)].push_back(static_cast<int>(e));
        }
    }
    const auto arrival = [&](int e) {
        const RouteEdge& edge = edges[static_cast<size_t>(e)];
        const size_t from = static_cast<size_t>(edge.from);
        const auto& at = result.deviceLatency[from];
        const size_t last = at.size() - 1;  // after all of its devices
        const size_t tap = edge.tap < 0 ? last : std::min(static_cast<size_t>(edge.tap), last);
        // After device tap - 1: as late as it hears its signal, plus its own latency
        // (not at[tap]: that has the delay before device tap in it, which comes later).
        const int latency = tap == last ? at[last] : tap == 0 ? 0 : at[tap - 1] + std::max(0, chains[from][tap - 1]);
        return result.inputLatency[from] + latency;
    };
    std::vector<int> arrivals, compensation;
    const auto align = [&](size_t node) {
        arrivals.clear();
        for (const int e : incoming[node]) arrivals.push_back(arrival(e));
        const int heard = alignInputs(arrivals, compensation);
        for (size_t i = 0; i < incoming[node].size(); ++i) {
            result.compensation[static_cast<size_t>(incoming[node][i])] = compensation[i];
        }
        // Down the chain, each sidechain lined up with the signal at its device.
        static const std::vector<int> kNoDevices;
        const std::vector<int>& chain = node < chains.size() ? chains[node] : kNoDevices;
        auto& at = result.deviceLatency[node];
        at.assign(chain.size() + 1, 0);
        auto& into = sidechains[node];
        std::stable_sort(into.begin(), into.end(), [&](int a, int b) {
            return edges[static_cast<size_t>(a)].device < edges[static_cast<size_t>(b)].device;
        });
        int latency = 0;  // from the strip's input
        size_t next = 0;
        for (size_t d = 0; d < chain.size(); ++d) {
            const size_t first = next;
            arrivals.assign(1, latency);
            for (; next < into.size() && edges[static_cast<size_t>(into[next])].device == static_cast<int>(d); ++next) {
                arrivals.push_back(arrival(into[next]) - heard);
            }
            if (next > first) {
                latency = alignInputs(arrivals, compensation);
                for (size_t i = first; i < next; ++i) {
                    result.compensation[static_cast<size_t>(into[i])] = compensation[i - first + 1];
                    result.deviceDelay[static_cast<size_t>(into[i])] = compensation[0];
                }
            }
            at[d] = latency;
            latency += std::max(0, chain[d]);
        }
        at[chain.size()] = latency;
        return heard;
    };
    for (const int node : order) result.inputLatency[static_cast<size_t>(node)] = align(static_cast<size_t>(node));
    result.masterInput = align(count);
    return result;
}

}  // namespace gil
