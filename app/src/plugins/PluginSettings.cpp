#include "plugins/PluginSettings.h"

#include <QSettings>
#include <QVariant>

#include "browser/PathKeys.h"
#include "plugins/PluginPaths.h"

namespace sub::app {

QStringList customPluginFolders() {
    // A one-item list can come back from QSettings as a string: toStringList()
    // makes it a list either way.
    return QSettings().value(QString::fromLatin1(kPluginFoldersKey)).toStringList();
}

void setCustomPluginFolders(const QStringList& folders) {
    QStringList normal;
    for (const QString& folder : folders) normal << normalPath(folder);
    QSettings().setValue(QString::fromLatin1(kPluginFoldersKey), normal);
}

QStringList pluginFolders() { return pluginSearchFolders(customPluginFolders()); }

}  // namespace sub::app
