#pragma once
// What a drag from the browser carries, and reading it where it is dropped.
//
// Audio files go as file URLs (text/uri-list); the other kinds as JSON under
// types of their own:
//   application/x-substation-plugin  a list of plug-ins (PluginRef fields:
//                                    format, uid, name, vendor, path, instrument)
//   application/x-substation-device  a list of built-in device kinds
//   application/x-substation-preset  a list of preset file paths
//
// Devices dragged from a track's chain in the device view, to move them (along
// its chain, into a rack's, or onto another track in the arrangement), go as
// plain text under a type of their own:
//   application/x-substation-device-move  the track's id, then the devices' ids, a line each

#include <QByteArray>
#include <QMimeData>
#include <QStringList>
#include <QVariantMap>

#include <memory>
#include <optional>
#include <vector>

#include "browser/BrowserItem.h"
#include "plugins/PluginInfo.h"

namespace sub::app {

inline constexpr const char* kPluginMime = "application/x-substation-plugin";
inline constexpr const char* kDeviceMime = "application/x-substation-device";
inline constexpr const char* kPresetMime = "application/x-substation-preset";
inline constexpr const char* kDeviceMoveMime = "application/x-substation-device-move";

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

// Devices dragged from a track's chain: the track and the devices (in its order).
struct MovedDevices {
    QString trackId;
    QStringList deviceIds;

    friend bool operator==(const MovedDevices&, const MovedDevices&) = default;
};
// The drag's data for devices of a track moved.
QByteArray movedDevicesData(const QString& trackId, const QStringList& deviceIds);
// Devices dragged from a track's chain, if the drag is of those.
std::optional<MovedDevices> movedDevices(const QMimeData* mime);
std::optional<MovedDevices> movedDevices(const QByteArray& data);

}  // namespace sub::app
