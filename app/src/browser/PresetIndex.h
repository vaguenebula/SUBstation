#pragma once
// The presets in the user's library (io/Presets.h), for the browser.
//
// The library is small (a file per preset), so it is listed on the UI thread:
// at start, when the application saves, renames or deletes a preset
// (rescan()), and when its folders change on disk (a watcher on the library and
// its folders; a burst of changes is listed once, 200 ms after the last). The
// session hands what it lists to the browser (BrowserController::setPresets).

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <vector>

#include "browser/BrowserItem.h"
#include "io/Presets.h"

namespace sub::app {

// The browser's name for presets straight in the library folder.
inline const QString kUngroupedPresets = QStringLiteral("Other");

// A preset as the browser lists it: the device it is for as its detail.
BrowserItem presetItem(const PresetFile& preset);

class PresetIndex : public QObject {
    Q_OBJECT

public:
    static constexpr int kSettleMs = 200;  // a burst of changes on disk: listed once, this long after the last

    // `root`: the library's folder ("": libraryDir()).
    explicit PresetIndex(QObject* parent = nullptr, const QString& root = {});

    QString root() const { return root_; }
    const std::vector<BrowserItem>& items() const { return items_; }
    // The devices the presets are for, in the order the browser lists them.
    const QStringList& groups() const { return groups_; }
    // The folders watched now (the library and its folders, or the nearest
    // folder above a library not made yet).
    QStringList watched() const { return watcher_.directories(); }

    // Lists the library again (updated, if anything changed).
    void rescan();

Q_SIGNALS:
    void updated();  // the presets changed

private:
    void watch();

    QString root_;
    std::vector<BrowserItem> items_;
    QStringList groups_;
    QFileSystemWatcher watcher_;
    QTimer timer_;
};

}  // namespace sub::app
