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
//
// Racks: a strip's devices are a tree (ChainSlot). A rack is a summing point
// of its own, inside the strip: its chains all hear its input, and it hears them
// as late as the slowest of them; the others are delayed after their fader to
// line up with it (the same alignInputs). A sidechain into a device in a chain
// lines up with the signal at that device (the devices before the rack, then
// those before it in its chain), and a delay before the device makes its chain
// later, and so maybe the rack. A tap after a device in a chain leaves as that
// device's signal does: before the chain's fader and the delay lining it up.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

namespace sub {

// Node `from`'s signal goes into node `to` (-1: the master).
struct RouteEdge {
    int from = 0;
    int to = -1;
    bool sums = true;  // `to` sums it into its input (false: an input edge or a sidechain)
    // Where it leaves `from`: after the device in slot `tap - 1` of its devices
    // (ChainSlot: depth first, so devices in its racks' chains too), as it leaves
    // it (before any delay lining up the next one, and in a rack's chain before
    // the chain's fader); 0: before them all; -1: after all of them (an output, a
    // send, a pre-fader tap).
    int tap = -1;
    // A sidechain: the device of `to` it goes into (its slot: ChainSlot), where
    // it is aligned; -1: none (or one not aligned: a device switched off).
    int device = -1;
};

// One of a strip's devices as delay compensation sees them. A strip lists its
// devices depth first: each device of its own chain, and after a rack the
// devices of its chains, chain by chain (and so on, for racks in them). A
// device's place in that list is its slot.
struct ChainSlot {
    int latency = 0;  // a device's latency (0 if it, or a rack it is in, is switched off)
    int rack = -1;    // the slot of the rack whose chain it is in; -1: the strip's own chain
    int chain = 0;    // which of that rack's chains
    int chains = -1;  // a rack: how many chains it has; -1: a device
    bool enabled = true;  // a rack switched off (or one in one) passes its input on: its chains don't count

    bool isRack() const noexcept { return chains >= 0; }
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
// topologicalOrder(); `chains[n]` lists node n's devices (ChainSlot), and
// `chains[count]` the master's.
struct GraphLatencies {
    std::vector<int> inputLatency;  // how late each node hears its summed inputs (0: nothing feeds it)
    std::vector<int> compensation;  // per edge: how much it is delayed to line up at its destination
    // Per edge, a sidechain's: how much its destination's signal is delayed just
    // before the device, to line up with it (0 for other edges).
    std::vector<int> deviceDelay;
    // Per node, and the master last, from the strip's input: how late each of
    // its slots hears its signal (the devices before it, and the delays before
    // sidechained ones), then how late its signal leaves the last of them (its latency).
    std::vector<std::vector<int>> deviceLatency;
    // Per node (and the master last), per slot: how late its signal leaves it
    // (a rack: its slowest chain, after its fader).
    std::vector<std::vector<int>> deviceOut;
    // Per node (and the master last), per slot, for a rack, per chain: how late
    // the chain's fader hears its signal, and how much the chain is delayed after
    // that to line up with the slowest one (empty for a device).
    std::vector<std::vector<std::vector<int>>> chainEnd, chainCompensation;
    int masterInput = 0;  // how late the master hears its inputs
};

inline GraphLatencies alignGraph(const std::vector<int>& order, const std::vector<RouteEdge>& edges,
                                 const std::vector<std::vector<ChainSlot>>& chains) {
    const size_t count = order.size();
    GraphLatencies result;
    result.inputLatency.assign(count, 0);
    result.compensation.assign(edges.size(), 0);
    result.deviceDelay.assign(edges.size(), 0);
    result.deviceLatency.assign(count + 1, {});
    result.deviceOut.assign(count + 1, {});
    result.chainEnd.assign(count + 1, {});
    result.chainCompensation.assign(count + 1, {});
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
    static const std::vector<ChainSlot> kNoDevices;
    const auto slotsOf = [&](size_t node) -> const std::vector<ChainSlot>& {
        return node < chains.size() ? chains[node] : kNoDevices;
    };
    const auto arrival = [&](int e) {
        const RouteEdge& edge = edges[static_cast<size_t>(e)];
        const size_t from = static_cast<size_t>(edge.from);
        const auto& out = result.deviceOut[from];
        // After the device in slot tap - 1: as it leaves it (not as late as the
        // next one hears it: a delay before that comes later).
        int latency = result.deviceLatency[from].back();  // after all of them
        if (edge.tap == 0) {
            latency = 0;
        } else if (edge.tap > 0 && static_cast<size_t>(edge.tap) <= out.size()) {
            latency = out[static_cast<size_t>(edge.tap) - 1];
        }
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
        // Down the device tree, each sidechain lined up with the signal at its
        // device, and each rack's chains with its slowest.
        const std::vector<ChainSlot>& slots = slotsOf(node);
        auto& at = result.deviceLatency[node];
        auto& out = result.deviceOut[node];
        auto& ends = result.chainEnd[node];
        auto& lineUp = result.chainCompensation[node];
        at.assign(slots.size() + 1, 0);
        out.assign(slots.size(), 0);
        ends.assign(slots.size(), {});
        lineUp.assign(slots.size(), {});
        std::vector<std::vector<int>> into(slots.size());  // the sidechains into each slot
        for (const int e : sidechains[node]) {
            const int device = edges[static_cast<size_t>(e)].device;
            if (static_cast<size_t>(device) < slots.size()) into[static_cast<size_t>(device)].push_back(e);
        }
        // The strip's own chain, and each rack's chains, as lists of their slots.
        std::vector<std::vector<std::vector<int>>> kids(slots.size());
        std::vector<int> top;
        for (size_t s = 0; s < slots.size(); ++s) {
            if (slots[s].isRack()) kids[s].resize(static_cast<size_t>(slots[s].chains));
            const int rack = slots[s].rack;
            if (rack < 0 || static_cast<size_t>(rack) >= s) {
                top.push_back(static_cast<int>(s));
            } else if (slots[static_cast<size_t>(rack)].isRack() && slots[s].chain >= 0 &&
                       slots[s].chain < slots[static_cast<size_t>(rack)].chains) {
                kids[static_cast<size_t>(rack)][static_cast<size_t>(slots[s].chain)].push_back(static_cast<int>(s));
            }
        }
        std::function<int(const std::vector<int>&, int)> walk = [&](const std::vector<int>& list, int latency) {
            for (const int s : list) {
                const auto slot = static_cast<size_t>(s);
                if (!into[slot].empty()) {
                    arrivals.assign(1, latency);
                    for (const int e : into[slot]) arrivals.push_back(arrival(e) - heard);
                    latency = alignInputs(arrivals, compensation);
                    for (size_t i = 0; i < into[slot].size(); ++i) {
                        result.compensation[static_cast<size_t>(into[slot][i])] = compensation[i + 1];
                        result.deviceDelay[static_cast<size_t>(into[slot][i])] = compensation[0];
                    }
                }
                at[slot] = latency;
                if (slots[slot].isRack()) {
                    if (slots[slot].enabled && !kids[slot].empty()) {
                        std::vector<int> chainEnds;
                        for (const auto& chain : kids[slot]) chainEnds.push_back(walk(chain, latency));
                        std::vector<int> delays;
                        latency = alignInputs(chainEnds, delays);
                        ends[slot] = std::move(chainEnds);
                        lineUp[slot] = std::move(delays);
                    }
                } else {
                    latency += std::max(0, slots[slot].latency);
                }
                out[slot] = latency;
            }
            return latency;
        };
        at[slots.size()] = walk(top, 0);
        return heard;
    };
    for (const int node : order) result.inputLatency[static_cast<size_t>(node)] = align(static_cast<size_t>(node));
    result.masterInput = align(count);
    return result;
}

}  // namespace sub
