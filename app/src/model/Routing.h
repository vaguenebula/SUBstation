#pragma once
// The group tree kept in the flat track list, and the routing graph: where each
// track's signal goes.
//
// Group tracks hold other tracks. The hierarchy is each track's `parent` (the
// group it is in, or none); the tracks stay a flat list, with the invariant
// that a group's descendants follow it, together (treeProblem).
//
// A track's output goes into its group by default, or where Track::output
// says (the master, another track, a device's sidechain, nowhere); returns are
// fed by sends; a track's input may be another track's output
// (Track::inputTrack); a device's sidechain hears a track's signal. Each is an
// edge of the routing graph, which never has a cycle: whatever adds an edge
// checks wouldCycle / inputWouldCycle / sidechainWouldCycle / outputWouldCycle
// first. The master is no part of that graph: everything reaches it.

#include "model/Track.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace sub::app {

// A track's place in the tree: its id and its group.
struct TreeEntry {
    QString id;
    std::optional<QString> parent;

    friend bool operator==(const TreeEntry&, const TreeEntry&) = default;
};
// Every track's (id, parent), in order.
using TrackTree = std::vector<TreeEntry>;

// Why these tracks, in this order, don't make a valid tree (none if they do): a
// track's parent must be a group listed before it, and everything between a
// group and its last descendant must be a descendant of it. Then a group's
// tracks follow it, together, and no group is in itself.
std::optional<QString> treeProblem(const std::vector<Track>& tracks);
// Takes tracks out of groups they can't be in (see treeProblem), keeping their order.
void repairTree(std::vector<Track>& tracks);

// Track id -> the tracks its signal goes into.
using RoutingGraph = QHash<QString, QStringList>;

// The track (or return) a device is on, in a rack or not; none: none (or the master's).
std::optional<QString> deviceTrack(const std::vector<Track>& tracks, const std::vector<Track>& returns,
                                   const QString& deviceId);
// The track a track's (or a return's) output goes into: its group (by
// default), the track it goes into, or the track the device whose sidechain it
// goes into is on. None: the master (or a device on it), or nowhere.
std::optional<QString> outputTarget(const Track& track, const std::vector<Track>& tracks,
                                    const std::vector<Track>& returns);

// Where each track's (and return's) signal goes: where its output goes (into
// its group, by default), into the returns it sends to, into the tracks taking
// their input from it, and into the tracks whose devices take it as their
// sidechain. (Not the master, which isn't in the graph.)
RoutingGraph routingGraph(const std::vector<Track>& tracks, const std::vector<Track>& returns);
// Whether `source`'s signal reaches `target` (or `source` is `target`).
bool feeds(const RoutingGraph& graph, const QString& source, const QString& target);
// Whether a send from a track into a return would close a cycle: the return is
// the track, or feeds it (through outputs, sends and inputs).
bool wouldCycle(const std::vector<Track>& tracks, const std::vector<Track>& returns, const QString& trackId,
                const QString& returnId);
// Whether taking its input from `sourceId`'s output would close a cycle: the
// source is the track, or the track feeds it. Never the master's.
bool inputWouldCycle(const std::vector<Track>& tracks, const std::vector<Track>& returns, const QString& trackId,
                     const QString& sourceId);
// Whether a track's output going there would close a cycle: it goes into the
// track itself (a device on it), or into one the track feeds.
bool outputWouldCycle(const std::vector<Track>& tracks, const std::vector<Track>& returns, const QString& trackId,
                      const Output& output);
// Whether a device on `trackId` taking `sourceId`'s signal as its sidechain
// would close a cycle: the source is the device's track, or that track feeds
// it. Never on the master (everything goes into it); the master is never a source.
bool sidechainWouldCycle(const std::vector<Track>& tracks, const std::vector<Track>& returns,
                         const QString& trackId, const QString& sourceId);

}  // namespace sub::app
