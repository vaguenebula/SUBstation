#include "browser/BrowserItem.h"

#include <QDir>

#include "browser/PathKeys.h"
#include "builtin/BuiltinRegistry.h"

namespace sub::app {

namespace {

QString categoryName(const BuiltinInfo& device) {
    return device.isInstrument() ? QStringLiteral("Instruments") : QStringLiteral("Audio Effects");
}

}  // namespace

QString kindName(ItemKind kind) {
    switch (kind) {
        case ItemKind::Audio: return QStringLiteral("audio");
        case ItemKind::Plugin: return QStringLiteral("plugin");
        case ItemKind::Device: return QStringLiteral("device");
        case ItemKind::Preset: return QStringLiteral("preset");
    }
    return {};
}

QString BrowserItem::key() const {
    switch (kind) {
        case ItemKind::Audio: return audioKey(path);
        case ItemKind::Plugin:
            if (plugin) return QStringLiteral("plugin:%1:%2").arg(plugin->format, plugin->uid);
            break;
        case ItemKind::Preset: return QStringLiteral("preset:") + QDir::toNativeSeparators(path);
        case ItemKind::Device: break;
    }
    return kindName(kind) + QLatin1Char(':') + path;
}

std::vector<BrowserItem> builtinItems(const QString& category) {
    // By category (in the order they first come), then as the engine lists them.
    std::vector<BrowserItem> items;
    for (const QString& name : builtinCategoryNames()) {
        if (!category.isEmpty() && category != name) continue;
        for (const BuiltinInfo& device : BuiltinRegistry::instance().devices()) {
            if (categoryName(device) != name) continue;
            items.push_back({QString::fromStdString(device.name), QString::fromStdString(device.id), ItemKind::Device,
                             name, std::nullopt, {}});
        }
    }
    return items;
}

QStringList builtinCategoryNames() {
    QStringList categories;
    for (const BuiltinInfo& device : BuiltinRegistry::instance().devices())
        if (!categories.contains(categoryName(device))) categories << categoryName(device);
    return categories;
}

BrowserItem pluginItem(const PluginInfo& plugin) {
    return {plugin.name, plugin.path, ItemKind::Plugin, plugin.vendor, plugin, pluginToolTip(plugin)};
}

}  // namespace sub::app
