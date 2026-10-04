#pragma once
// What the plug-in scanner finds: the plug-ins in each file, and the files it
// could not read.

#include <QString>
#include <QVariantMap>

#include <vector>

namespace sub::app {

// One plug-in (a VST3 class) in a file. A module with several plug-ins (an
// instrument and its FX version) gives one each.
struct PluginInfo {
    QString name;
    QString format;    // "VST3"
    QString path;      // the .vst3 bundle or file
    QString uid;       // VST3 class id (32 hex digits)
    QString vendor;
    QString version;
    QString category;  // VST3 sub-categories, e.g. "Instrument|Synth" or "Fx|EQ"
    bool instrument = false;

    bool operator==(const PluginInfo&) const = default;

    // What a device needs to load it (the model's PluginRef): format, uid,
    // name, vendor, path, instrument.
    QVariantMap toRef() const {
        return {{QStringLiteral("format"), format}, {QStringLiteral("uid"), uid},
                {QStringLiteral("name"), name},     {QStringLiteral("vendor"), vendor},
                {QStringLiteral("path"), path},     {QStringLiteral("instrument"), instrument}};
    }
};

// What the browser shows when hovering over a plug-in: "Name (VST3 Instrument)",
// its vendor, its categories and its file, one a line (those it has).
QString pluginToolTip(const PluginInfo& plugin);

struct ScanFailure {
    QString path;
    QString reason;  // for the user

    bool operator==(const ScanFailure&) const = default;
};

struct ScanResult {
    std::vector<PluginInfo> plugins;  // by name, then vendor (ignoring case)
    std::vector<ScanFailure> failures;
};

}  // namespace sub::app
