#include "io/Presets.h"

#include "io/Serialization.h"
#include "model/Devices.h"
#include "model/Errors.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace sub::app {

namespace {

// Not in Windows file names.
const QRegularExpression& forbidden() {
    static const QRegularExpression re(QStringLiteral("[<>:\"/\\\\|?*\\x{0000}-\\x{001f}]"));
    return re;
}

bool reserved(const QString& name) {
    static const QStringList names = [] {
        QStringList list{QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL")};
        for (int i = 1; i < 10; ++i) {
            list.append(QStringLiteral("COM%1").arg(i));
            list.append(QStringLiteral("LPT%1").arg(i));
        }
        return list;
    }();
    return names.contains(name);
}

// A file's name without its extension ("Glue.gilpreset": "Glue").
QString stemOf(const QString& fileName) {
    const qsizetype dot = fileName.lastIndexOf(u'.');
    return (dot > 0 && dot < fileName.size() - 1) ? fileName.left(dot) : fileName;
}

bool isPresetFile(const QFileInfo& info) {
    return info.isFile() && info.fileName().toLower().endsWith(kPresetExtension);
}

QString rootOr(const QString& root) { return root.isEmpty() ? libraryDir() : root; }

// Strips trailing dots and spaces.
QString withoutTrailingDots(QString text) {
    while (!text.isEmpty() && (text.endsWith(u'.') || text.endsWith(u' '))) text.chop(1);
    return text;
}

}  // namespace

QString libraryDir() {
    const QString overridden = qEnvironmentVariable("SUBSTATION_PRESETS");
    if (!overridden.isEmpty()) return overridden;
    return QDir::homePath() + QStringLiteral("/Documents/SUBstation/Presets");
}

QString presetFileName(const QString& name) {
    QString cleaned = name;
    cleaned.replace(forbidden(), QStringLiteral("_"));
    cleaned = withoutTrailingDots(cleaned.trimmed());
    if (cleaned.isEmpty() || reserved(cleaned.section(u'.', 0, 0).toUpper())) {
        throw EditError(QStringLiteral("'%1' can't be a preset's name").arg(name));
    }
    return cleaned;
}

QString groupOf(const Device& device) {
    const QString name = presetFileName(kindName(device));
    return name.compare(kDefaultsFolder, Qt::CaseInsensitive) == 0 ? name + QStringLiteral(" (Device)") : name;
}

QString presetPath(const Device& device, const QString& name, const QString& root) {
    return rootOr(root) + u'/' + groupOf(device) + u'/' + presetFileName(name) + kPresetExtension;
}

QString saveToLibrary(const Device& device, const QString& name, const QString& root) {
    const QString path = presetPath(device, name, root);
    QDir().mkpath(QFileInfo(path).path());
    savePreset(device, path);
    return path;
}

std::vector<PresetFile> listPresets(const QString& root) {
    const QDir dir(rootOr(root));
    std::vector<PresetFile> found;
    if (!dir.exists()) return found;  // no library yet
    const auto filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    for (const QFileInfo& entry : dir.entryInfoList(filters)) {
        if (isPresetFile(entry)) {
            found.push_back({entry.filePath(), stemOf(entry.fileName()), QString()});
        } else if (entry.isDir() && !entry.fileName().startsWith(u'.') &&
                   entry.fileName().compare(kDefaultsFolder, Qt::CaseInsensitive) != 0) {
            for (const QFileInfo& file : QDir(entry.filePath()).entryInfoList(filters)) {
                if (isPresetFile(file)) found.push_back({file.filePath(), stemOf(file.fileName()), entry.fileName()});
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const PresetFile& a, const PresetFile& b) {
        const QString groupA = a.group.toCaseFolded();
        const QString groupB = b.group.toCaseFolded();
        if (groupA != groupB) return groupA < groupB;
        return a.name.toCaseFolded() < b.name.toCaseFolded();
    });
    return found;
}

QString renamePreset(const QString& path, const QString& name) {
    const QFileInfo info(path);
    const QString target = info.path() + u'/' + presetFileName(name) + kPresetExtension;
    if (QDir::cleanPath(target) == QDir::cleanPath(path)) return path;
#ifdef Q_OS_WIN
    const bool sameFile = QDir::cleanPath(target).compare(QDir::cleanPath(path), Qt::CaseInsensitive) == 0;
#else
    const bool sameFile = false;
#endif
    if (QFileInfo::exists(target) && !sameFile) {  // (a change of case is no clash)
        throw EditError(QStringLiteral("There is a preset called %1 already").arg(stemOf(QFileInfo(target).fileName())));
    }
    if (!QFile::rename(path, target)) {
        throw EditError(QStringLiteral("Could not rename %1").arg(stemOf(info.fileName())));
    }
    return target;
}

std::optional<QString> defaultPath(const QString& kind, const std::optional<PluginRef>& plugin, const QString& root) {
    if (kind == kRackKind || (kind == kPluginKind && !plugin)) return std::nullopt;
    Device device;
    device.kind = kind;
    device.plugin = plugin;
    QString name = presetFileName(kindName(device));
    if (plugin) name = QStringLiteral("%1 (%2)").arg(name, presetFileName(plugin->uid));
    return rootOr(root) + u'/' + kDefaultsFolder + u'/' + name + kPresetExtension;
}

bool hasDefault(const QString& kind, const std::optional<PluginRef>& plugin, const QString& root) {
    const auto path = defaultPath(kind, plugin, root);
    return path && QFileInfo(*path).isFile();
}

QString saveDefault(const Device& device, const QString& root) {
    const auto path = defaultPath(device.kind, device.plugin, root);
    if (!path) throw EditError(QStringLiteral("Racks have no default preset"));
    QDir().mkpath(QFileInfo(*path).path());
    savePreset(device, *path);
    return *path;
}

bool clearDefault(const QString& kind, const std::optional<PluginRef>& plugin, const QString& root) {
    const auto path = defaultPath(kind, plugin, root);
    if (!path || !QFileInfo(*path).isFile()) return false;
    QFile::remove(*path);
    return true;
}

std::optional<Device> defaultDevice(const QString& kind, const std::optional<PluginRef>& plugin, const QString& root) {
    const auto path = defaultPath(kind, plugin, root);
    if (!path || !QFileInfo(*path).isFile()) return std::nullopt;
    Device device;
    try {
        device = loadPreset(*path);
    } catch (const ProjectFileError&) {
        return std::nullopt;
    }
    if (device.kind != kind) return std::nullopt;
    if (plugin) {
        if (!device.plugin || device.plugin->uid != plugin->uid) return std::nullopt;
        device.plugin = plugin;
    } else if (const BuiltinDevice* builtin = builtinDevice(kind)) {
        QMap<QString, double> params = builtin->defaults;
        for (auto it = device.params.constBegin(); it != device.params.constEnd(); ++it) params.insert(it.key(), it.value());
        device.params = params;
    }
    return device;
}

}  // namespace sub::app
