#include "session/SessionSupport.h"

#include <QFileInfo>
#include <QSet>

#include "audio/EngineBridge.h"
#include "model/Devices.h"
#include "model/Project.h"

namespace sub::app {

std::optional<PluginRef> pluginRefFromMap(const QVariantMap& map) {
    PluginRef ref;
    ref.uid = map.value(QStringLiteral("uid")).toString();
    if (ref.uid.isEmpty()) return std::nullopt;
    const QString format = map.value(QStringLiteral("format")).toString();
    if (!format.isEmpty()) ref.format = format;
    ref.name = map.value(QStringLiteral("name")).toString();
    ref.vendor = map.value(QStringLiteral("vendor")).toString();
    ref.path = map.value(QStringLiteral("path")).toString();
    ref.instrument = map.value(QStringLiteral("instrument")).toBool();
    return ref;
}

std::vector<PluginRef> pluginRefsFromList(const QVariantList& list) {
    std::vector<PluginRef> refs;
    for (const QVariant& item : list) {
        if (const auto ref = pluginRefFromMap(item.toMap())) refs.push_back(*ref);
    }
    return refs;
}

QString fileStem(const QString& path) { return QFileInfo(path).completeBaseName(); }

QStringList ownersAmong(const Project& project, const QStringList& trackIds) {
    QStringList owners;
    for (const QString& id : trackIds) {
        if (!owners.contains(id) && (project.hasTrack(id) || project.hasReturn(id))) owners.append(id);
    }
    return owners;
}

void storeTrackPluginStates(EngineBridge& bridge, const Project& project, const QStringList& trackIds) {
    QSet<QString> ids;
    for (const QString& trackId : trackIds) {
        if (!project.hasTrack(trackId)) continue;
        std::vector<const Track*> tracks{&project.track(trackId)};
        for (const Track* inside : project.descendants(trackId)) tracks.push_back(inside);
        for (const Track* track : tracks) ids.unite(deviceIdsOfList(track->devices));
    }
    bridge.storePluginStates(ids);
}

QVariantMap clipRefMap(const ClipRef& ref) {
    return {{QStringLiteral("trackId"), ref.trackId}, {QStringLiteral("clipId"), ref.clipId}};
}

QVariantList clipRefList(const ClipRefs& refs) {
    QVariantList list;
    for (const ClipRef& ref : refs) list.append(clipRefMap(ref));
    return list;
}

}  // namespace sub::app
