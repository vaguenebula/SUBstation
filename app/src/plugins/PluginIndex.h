#pragma once
// The installed plug-ins, for the browser and the preferences.
//
// Scanning (PluginScanner) runs on a thread of its own and reads only new or
// changed files, unless it is a rescan; the UI thread never waits for it. One
// scan at a time: a scan asked for while one runs is remembered (as a rescan if
// either asked for one) and runs when it finishes.
//
// The browser starts a scan when it is made; Rescan Plug-ins (the browser's
// menu, the preferences) reads every file again, also those that failed
// before. The user's own folders are kept in QSettings (PluginSettings.h):
// adding one scans at once (only its files are read, the rest come from the
// cache), removing one scans again, so its plug-ins leave the browser.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>

#include <memory>
#include <optional>
#include <vector>

#include "plugins/PluginFolderModel.h"
#include "plugins/PluginInfo.h"
#include "plugins/PluginListModel.h"

namespace sub::app {

class PluginIndex : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool scanning READ scanning NOTIFY updated)
    Q_PROPERTY(int pluginCount READ pluginCount NOTIFY updated)
    Q_PROPERTY(int failureCount READ failureCount NOTIFY updated)
    // The preferences' scan status: "Scanning 3/40: Name" or "Scanning plug-ins…"
    // while scanning, else "12 plug-ins found · 1 file could not be read".
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    // The files that could not be read and why ("Name.vst3: reason", the first 30), one a line.
    Q_PROPERTY(QString failuresText READ failuresText NOTIFY updated)
    // The same as a list of {path, name, reason}.
    Q_PROPERTY(QVariantList failureList READ failureList NOTIFY updated)
    Q_PROPERTY(QStringList standardFolders READ standardFolders NOTIFY foldersChanged)
    Q_PROPERTY(QStringList customFolders READ customFolders NOTIFY foldersChanged)
    Q_PROPERTY(sub::app::PluginListModel* plugins READ pluginModel CONSTANT)
    Q_PROPERTY(sub::app::PluginFolderModel* folders READ folderModel CONSTANT)

public:
    // `scanner`: the child process that reads plug-in files (empty:
    // PluginScanner::defaultProgram()).
    explicit PluginIndex(QObject* parent = nullptr, QString scanner = {});
    ~PluginIndex() override;  // stops a scan that runs (see wait())

    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    const std::vector<ScanFailure>& failures() const { return failures_; }
    bool scanning() const { return thread_ != nullptr; }
    int pluginCount() const { return static_cast<int>(plugins_.size()); }
    int failureCount() const { return static_cast<int>(failures_.size()); }
    QString statusText() const;
    QString failuresText() const;
    QVariantList failureList() const;
    QStringList standardFolders() const;
    QStringList customFolders() const;
    PluginListModel* pluginModel() const { return pluginModel_; }
    PluginFolderModel* folderModel() const { return folderModel_; }
    const QString& scanner() const { return scanner_; }

    // Scans the folders (pluginFolders()): only new or changed files are read,
    // unless `rescan`.
    Q_INVOKABLE void scan(bool rescan = false);
    // Reads every plug-in file again (also those that failed before).
    Q_INVOKABLE void rescan() { scan(true); }
    // Adds a folder of the user's and scans it, unless it is listed already (a
    // standard folder or one of theirs): false then.
    Q_INVOKABLE bool addFolder(const QString& folder);
    // Takes a folder of the user's off the list and scans again, so its plug-ins go.
    Q_INVOKABLE void removeFolder(const QString& folder);
    // Stops a scan that runs and waits for its thread (the application closing).
    // What it found so far is not taken.
    void wait();

signals:
    void updated();  // the plug-ins or failures changed, or scanning started or stopped
    void progress(int done, int total, const QString& path);  // before each file that is read
    void statusMessage(const QString& message);  // the scanner could not run (for the status line)
    void statusTextChanged();
    void foldersChanged();

private:
    void finished();
    void setProgressText(const QString& text);

    QString scanner_;
    std::vector<PluginInfo> plugins_;
    std::vector<ScanFailure> failures_;
    PluginListModel* pluginModel_;
    PluginFolderModel* folderModel_;
    QString progressText_;

    // The scan that runs, and what it hands back (written on its thread, read
    // once it finished).
    QThread* thread_ = nullptr;
    struct Outcome {
        ScanResult result;
        std::optional<QString> error;
    };
    std::shared_ptr<Outcome> outcome_;
    std::optional<bool> again_;  // a scan asked for while one runs (a rescan?)
};

}  // namespace sub::app
