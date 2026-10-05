#include "browser/PresetIndex.h"

#include <QDir>
#include <QFileInfo>

namespace sub::app {

BrowserItem presetItem(const PresetFile& preset) {
    const QString group = preset.group.isEmpty() ? kUngroupedPresets : preset.group;
    BrowserItem item;
    item.name = preset.name;
    item.path = preset.path;
    item.kind = ItemKind::Preset;
    item.detail = group;
    item.toolTip = preset.name + u'\n' + group + QStringLiteral(" preset\n") + QDir::toNativeSeparators(preset.path);
    return item;
}

PresetIndex::PresetIndex(QObject* parent, const QString& root)
    : QObject(parent), root_(root.isEmpty() ? libraryDir() : root), watcher_(this), timer_(this) {
    timer_.setSingleShot(true);
    timer_.setInterval(kSettleMs);
    connect(&timer_, &QTimer::timeout, this, &PresetIndex::rescan);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { timer_.start(); });
    rescan();
}

void PresetIndex::rescan() {
    timer_.stop();
    std::vector<BrowserItem> items;
    for (const PresetFile& preset : listPresets(root_)) items.push_back(presetItem(preset));
    watch();
    if (items == items_) return;
    items_ = std::move(items);
    groups_.clear();
    for (const BrowserItem& item : items_) {
        if (!groups_.contains(item.detail)) groups_.append(item.detail);
    }
    Q_EMIT updated();
}

// Watch the library and its folders (those there now); while there is no
// library, the nearest folder above it that there is, to see it made.
void PresetIndex::watch() {
    QStringList folders;
    const QFileInfo library(root_);
    if (library.isDir()) {
        folders.append(library.absoluteFilePath());
        const auto filters = QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
        for (const QFileInfo& entry : QDir(root_).entryInfoList(filters)) {
            if (!entry.fileName().startsWith(u'.')) folders.append(entry.absoluteFilePath());
        }
    } else {
        QString above = QFileInfo(library.absoluteFilePath()).path();
        while (!QFileInfo(above).isDir()) {
            const QString up = QFileInfo(above).path();
            if (up == above) break;
            above = up;
        }
        if (QFileInfo(above).isDir()) folders.append(above);
    }
    const QStringList watched = watcher_.directories();
    QStringList gone;
    for (const QString& folder : watched) {
        if (!folders.contains(folder)) gone.append(folder);
    }
    if (!gone.isEmpty()) watcher_.removePaths(gone);
    QStringList added;
    for (const QString& folder : folders) {
        if (!watched.contains(folder)) added.append(folder);
    }
    if (!added.isEmpty()) watcher_.addPaths(added);
}

}  // namespace sub::app
