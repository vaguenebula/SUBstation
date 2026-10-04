#include "model/Routing.h"

#include <QSet>

namespace sub::app {

std::optional<QString> treeProblem(const std::vector<Track>& tracks) {
    QStringList path;  // the groups the next track can be in (outermost first)
    QSet<QString> seen;
    for (const Track& track : tracks) {
        if (seen.contains(track.id)) return QStringLiteral("%1 is listed twice").arg(track.name);
        seen.insert(track.id);
        if (!track.parent) {
            path.clear();
        } else if (const qsizetype at = path.indexOf(*track.parent); at >= 0) {
            path.resize(at + 1);
        } else {
            return QStringLiteral("%1 is not with the other tracks of its group").arg(track.name);
        }
        if (track.isGroup()) path.append(track.id);
    }
    return std::nullopt;
}

void repairTree(std::vector<Track>& tracks) {
    QStringList path;
    for (Track& track : tracks) {
        if (track.parent && !path.contains(*track.parent)) track.parent.reset();
        if (!track.parent) {
            path.clear();
        } else {
            path.resize(path.indexOf(*track.parent) + 1);
        }
        if (track.isGroup()) path.append(track.id);
    }
}

RoutingGraph routingGraph(const std::vector<Track>& tracks, const std::vector<Track>& returns) {
    RoutingGraph graph;
    for (const auto* list : {&tracks, &returns}) {
        for (const Track& t : *list) graph.insert(t.id, {});
    }
    for (const auto* list : {&tracks, &returns}) {
        for (const Track& track : *list) {
            if (track.parent && graph.contains(*track.parent)) graph[track.id].append(*track.parent);
            for (auto it = track.sends.constBegin(); it != track.sends.constEnd(); ++it) {
                if (graph.contains(it.key())) graph[track.id].append(it.key());
            }
            if (track.inputTrack && graph.contains(*track.inputTrack)) graph[*track.inputTrack].append(track.id);
            for (const Device* device : iterDevices(track.devices)) {
                if (device->sidechain && graph.contains(device->sidechain->trackId)) {
                    graph[device->sidechain->trackId].append(track.id);
                }
            }
        }
    }
    return graph;
}

bool feeds(const RoutingGraph& graph, const QString& source, const QString& target) {
    QSet<QString> seen;
    QStringList stack{source};
    while (!stack.isEmpty()) {
        const QString current = stack.takeLast();
        if (current == target) return true;
        if (!seen.contains(current)) {
            seen.insert(current);
            stack.append(graph.value(current));
        }
    }
    return false;
}

bool wouldCycle(const std::vector<Track>& tracks, const std::vector<Track>& returns, const QString& trackId,
                const QString& returnId) {
    return feeds(routingGraph(tracks, returns), returnId, trackId);
}

bool inputWouldCycle(const std::vector<Track>& tracks, const std::vector<Track>& returns, const QString& trackId,
                     const QString& sourceId) {
    return sourceId != kMaster && feeds(routingGraph(tracks, returns), trackId, sourceId);
}

bool sidechainWouldCycle(const std::vector<Track>& tracks, const std::vector<Track>& returns,
                         const QString& trackId, const QString& sourceId) {
    if (sourceId == kMaster) return true;
    return trackId != kMaster && feeds(routingGraph(tracks, returns), trackId, sourceId);
}

}  // namespace sub::app
