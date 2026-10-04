#include "plugins/PluginInfo.h"

#include <QStringList>

namespace sub::app {

QString pluginToolTip(const PluginInfo& plugin) {
    const QString kind = plugin.instrument ? QStringLiteral("Instrument") : QStringLiteral("Audio Effect");
    QStringList lines;
    for (const QString& line : {QStringLiteral("%1 (%2 %3)").arg(plugin.name, plugin.format, kind), plugin.vendor,
                                QString(plugin.category).replace(QLatin1Char('|'), QStringLiteral(", ")), plugin.path})
        if (!line.isEmpty()) lines << line;
    return lines.join(QLatin1Char('\n'));
}

}  // namespace sub::app
