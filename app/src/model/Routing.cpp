#include "model/Routing.h"

#include <QSet>

#include <algorithm>

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

std::optional<QString> deviceTrack(const std::vector<Track>& tracks, const std::vector<Track>& returns,
                                   const QString& deviceId) {
    for (const auto* list : {&tracks, &returns}) {
        for (const Track& track : *list) {
            for (const Device* device : iterDevices(track.devices)) {
                if (device->id == deviceId) return track.id;
            }
        }
    }
    return std::nullopt;
}

std::optional<QString> outputTarget(const Track& track, const std::vector<Track>& tracks,
                                    const std::vector<Track>& returns) {
    switch (track.output.to) {
    case Output::To::Group: return track.parent;
    case Output::To::Track: return track.output.id;
    case Output::To::Sidechain: return deviceTrack(tracks, returns, track.output.id);
    case Output::To::Master:
    case Output::To::None: break;
    }
    return std::nullopt;
}

RoutingGraph routingGraph(const std::vector<Track>& tracks, const std::vector<Track>& returns) {
    RoutingGraph graph;
    for (const auto* list : {&tracks, &returns}) {
        for (const Track& t : *list) graph.insert(t.id, {});
    }
    // The devices' tracks, once, if an output goes into a device.
    QHash<QString, QString> deviceTracks;
    const auto anyIntoDevice = [](const std::vector<Track>& list) {
        return std::any_of(list.begin(), list.end(), [](const Track& t) { return t.output.to == Output::To::Sidechain; });
    };
    if (anyIntoDevice(tracks) || anyIntoDevice(returns)) {
        for (const auto* list : {&tracks, &returns}) {
            for (const Track& track : *list) {
                for (const Device* device : iterDevices(track.devices)) deviceTracks.insert(device->id, track.id);
            }
        }
    }
    for (const auto* list : {&tracks, &returns}) {
        for (const Track& track : *list) {
            std::optional<QString> output;
            if (track.output.to == Output::To::Group) {
                output = track.parent;
            } else if (track.output.to == Output::To::Track) {
                output = track.output.id;
            } else if (track.output.to == Output::To::Sidechain && deviceTracks.contains(track.output.id)) {
                output = deviceTracks.value(track.output.id);
            }
            if (output && graph.contains(*output)) graph[track.id].append(*output);
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

bool outputWouldCycle(const std::vector<Track>& tracks, const std::vector<Track>& returns, const QString& trackId,
                      const Output& output) {
    std::optional<QString> target;
    if (output.to == Output::To::Track) target = output.id;
    if (output.to == Output::To::Sidechain) target = deviceTrack(tracks, returns, output.id);
    if (output.to == Output::To::Group) {
        for (const Track& t : tracks) {
            if (t.id == trackId) target = t.parent;
        }
    }
    return target && feeds(routingGraph(tracks, returns), *target, trackId);
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
