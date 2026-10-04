#pragma once
// An item the browser lists: an audio file, a plug-in, a built-in device or a
// preset.

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

#include "plugins/PluginInfo.h"

namespace sub::app {

// The kinds of item, numbered as the browser backend numbers them (sub::browser::Kind).
enum class ItemKind { Audio = 0, Plugin = 1, Device = 2, Preset = 3 };

// "audio", "plugin", "device", "preset" (what QML sees, and the keys' prefixes).
QString kindName(ItemKind kind);

struct BrowserItem {
    QString name;
    // An audio file or a preset: its file. A plug-in: its .vst3. A built-in
    // device: the device's kind ("utility").
    QString path;
    ItemKind kind = ItemKind::Audio;
    // The parent folder, the plug-in's vendor, the device's category, or the
    // device a preset is for.
    QString detail;
    std::optional<PluginInfo> plugin;
    QString toolTip;

    bool operator==(const BrowserItem&) const = default;

    // Who the item is, for what the browser remembers about it (Library):
    //   audio   "audio:" + os.path.normcase(os.path.normpath(path)) (audioKey())
    //   plugin  "plugin:<format>:<uid>"
    //   device  "device:<kind>"
    //   preset  "preset:<path>" (in the system's form, as Python wrote it)
    QString key() const;
};

// Built-in devices, all or one category's ("Instruments", "Audio Effects"), as
// the engine lists them (instruments first, then by name).
std::vector<BrowserItem> builtinItems(const QString& category = {});
// The categories of built-in devices, in that order.
QStringList builtinCategoryNames();

// A plug-in as the browser lists it: its vendor as the detail.
BrowserItem pluginItem(const PluginInfo& plugin);

}  // namespace sub::app
