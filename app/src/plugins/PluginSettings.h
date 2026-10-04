#pragma once
// The user's own VST3 folders, kept in QSettings (plugins/vst3_folders).

#include <QString>
#include <QStringList>

namespace sub::app {

inline constexpr const char* kPluginFoldersKey = "plugins/vst3_folders";

// The folders the user added, searched after the standard ones.
QStringList customPluginFolders();
void setCustomPluginFolders(const QStringList& folders);

// Every folder a scan looks in: pluginSearchFolders(customPluginFolders()).
QStringList pluginFolders();

}  // namespace sub::app
