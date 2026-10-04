#pragma once
// What a drag from the browser carries, and reading it where it is dropped.
//
// Audio files go as file URLs (text/uri-list); the other kinds as JSON under
// types of their own:
//   application/x-substation-plugin  a list of plug-ins (PluginRef fields:
//                                    format, uid, name, vendor, path, instrument)
//   application/x-substation-device  a list of built-in device kinds
//   application/x-substation-preset  a list of preset file paths

#include <QByteArray>
#include <QMimeData>
#include <QStringList>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "browser/BrowserItem.h"
#include "plugins/PluginInfo.h"

namespace sub::app {

inline constexpr const char* kPluginMime = "application/x-substation-plugin";
inline constexpr const char* kDeviceMime = "application/x-substation-device";
inline constexpr const char* kPresetMime = "application/x-substation-preset";

// The types a drag from the browser may carry.
QStringList browserMimeTypes();

// The drag's data for these items.
std::unique_ptr<QMimeData> browserMimeData(const std::vector<BrowserItem>& items);
// The same as {mime type: text}, for QML (Drag.mimeData).
QVariantMap browserDragData(const std::vector<BrowserItem>& items);

// Plug-ins dragged from the browser, if any (format, uid, name, vendor, path,
// instrument filled in). Nothing if the data is not as written.
std::vector<PluginInfo> pluginRefs(const QMimeData* mime);
std::vector<PluginInfo> pluginRefs(const QByteArray& data);
// Built-in device kinds dragged from the browser, if any.
QStringList deviceKinds(const QMimeData* mime);
QStringList deviceKinds(const QByteArray& data);
// Presets (their files) dragged from the browser, if any.
QStringList presetPaths(const QMimeData* mime);
QStringList presetPaths(const QByteArray& data);

}  // namespace sub::app
