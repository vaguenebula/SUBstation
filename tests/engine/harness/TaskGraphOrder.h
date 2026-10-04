#pragma once
// The render graph's queue order and ranks for a graph given by hand (what the
// removed bindings' task_graph_order did for the Python tests): the order a
// TaskGraph queues its nodes without inputs, and each node's rank (the cost
// from it to the end of its longest path).

#include <stdexcept>
#include <utility>
#include <vector>

#include "Scheduler.h"

namespace subtest {

// Node i goes into each of destinations[i] (-1: nowhere the graph runs, the
// master), listed after the nodes that go into it. Throws std::invalid_argument
// for a node going into one listed before it, or not one cost per node.
inline std::pair<std::vector<int>, std::vector<float>> taskGraphOrder(const std::vector<std::vector<int>>& destinations,
                                                                       const std::vector<float>& costs) {
    if (costs.size() != destinations.size()) throw std::invalid_argument("One cost per node");
    std::vector<std::pair<int, int>> edges;
    for (size_t i = 0; i < destinations.size(); ++i) {
        for (const int to : destinations[i]) {
            if (to >= 0 && static_cast<size_t>(to) <= i) throw std::invalid_argument("A node goes into one listed before it");
            edges.emplace_back(static_cast<int>(i), to);
        }
    }
    sub::TaskGraph graph(static_cast<int>(destinations.size()), edges);
    for (int i = 0; i < graph.size(); ++i) graph.setCost(i, costs[static_cast<size_t>(i)]);
    graph.orderRoots();
    std::vector<float> ranks;
    for (int i = 0; i < graph.size(); ++i) ranks.push_back(graph.rank(i));
    return {graph.roots(), ranks};
}

// The same with one edge per node: node i goes into outputs[i] (-1: none).
inline std::pair<std::vector<int>, std::vector<float>> taskGraphOrder(const std::vector<int>& outputs,
                                                                       const std::vector<float>& costs) {
    std::vector<std::vector<int>> destinations;
    for (const int out : outputs) destinations.push_back(out >= 0 ? std::vector<int>{out} : std::vector<int>{});
    return taskGraphOrder(destinations, costs);
}

}  // namespace subtest
