#pragma once
// Small helpers the session's parts share (the application layer's own).

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <optional>
#include <vector>

#include "editor/ClipRef.h"
#include "model/Device.h"

namespace sub::app {

class EngineBridge;
class Project;

// A plug-in as QML hands it over (PluginRef fields: format, uid, name, vendor,
// path, instrument; as the browser's pluginActivated has them). None without a uid.
std::optional<PluginRef> pluginRefFromMap(const QVariantMap& map);
std::vector<PluginRef> pluginRefsFromList(const QVariantList& list);

// A file's name without its last extension (Python's Path.stem).
QString fileStem(const QString& path);

// The tracks (groups too) and returns among these, in order, each once.
QStringList ownersAmong(const Project& project, const QStringList& trackIds);

// Stores the plug-ins' states on these tracks (and what is in them, if
// groups) in the model, as they are now: before copying them.
void storeTrackPluginStates(EngineBridge& bridge, const Project& project, const QStringList& trackIds);

// Clips as QML sees them: [{trackId, clipId}].
QVariantList clipRefList(const ClipRefs& refs);
QVariantMap clipRefMap(const ClipRef& ref);

}  // namespace sub::app
