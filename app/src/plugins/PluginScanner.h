#pragma once
// Finds what each VST3 plug-in file contains, without letting a broken plug-in
// take the program down.
//
// Reading a plug-in file means running its code, and a broken plug-in can crash
// or hang the process that loads it. So files are read in a child process
// (substation-scan, tools/scanner), many per process: if one takes the process
// down or doesn't answer in time, it is marked as failed and the rest carry on
// in a new process. Results are cached per file, so a file is read again only
// when it changes (or on a rescan).
//
// scan() blocks until it is done: PluginIndex runs it on a thread of its own.
// It needs no event loop (QProcess's waiting functions), but a thread Qt knows
// (a QThread, or the main thread).
//
// The child's protocol (UTF-8, one JSON value per line): it says {"ready": true}
// once it can scan, then reads one JSON-encoded path per line and answers each
// with {"path": ..., "plugins": [{"uid", "name", "vendor", "version",
// "category", "instrument"}, ...]} or {"path": ..., "error": ...}. Lines that
// are not JSON objects are not its answers and are skipped; answers are
// matched by path, so a stray line can't be taken for one.
//
// The cache (pluginCachePath()): {"version": 1, "files": {key: {"path",
// "signature": [mtime_ns, size], "plugins": [...], "error": null or reason}}},
// keyed by caseKey(path). Another version, or a file that can't be read or
// parsed, counts as an empty cache. It is written to a temporary file and moved
// into place, so a crash never leaves half a cache; one that can't be written
// only costs time. Failures are cached too: a file that crashed or timed out is
// not read again until it changes, or on a rescan. Entries of files not in a
// scan (a folder removed from the list) are kept while the file exists, so
// adding the folder back is quick; they don't appear in its result.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

#include "plugins/PluginInfo.h"
#include "plugins/PluginPaths.h"

namespace sub::app {

class PluginScanner {
public:
    // Called before each file that is read: (files read so far, files to read, this file).
    using Progress = std::function<void(int done, int total, const QString& path)>;
    // Asked between files, between child processes and while waiting for one:
    // a cancelled scan returns what it has. Files not reached are left out of
    // the result but keep their cache entries.
    using Cancelled = std::function<bool()>;

    // `program` (and `arguments`): the child process; empty for defaultProgram().
    // `cacheFile`: empty for pluginCachePath(). `folders`: where to look when
    // scan() is given no files (none: pluginSearchFolders()).
    explicit PluginScanner(QString program = {}, QString cacheFile = {}, double timeout = kPluginScanTimeout,
                           std::optional<QStringList> folders = std::nullopt, QStringList arguments = {});

    // substation-scan next to the application's executable, or SUBSTATION_SCANNER if set.
    static QString defaultProgram();

    const QString& program() const { return program_; }
    const QString& cacheFile() const { return cacheFile_; }

    // Everything in `files` (default: all plug-in files under the folders).
    // Only new or changed files are read, unless `rescan`. Throws
    // std::runtime_error("The plug-in scanner could not start.") if a child
    // process can't be started or doesn't say it is ready in time.
    ScanResult scan(const std::optional<QStringList>& files = std::nullopt, bool rescan = false,
                    const Progress& progress = {}, const Cancelled& cancelled = {});

private:
    QJsonObject loadCache() const;
    void saveCache(const QJsonObject& files) const;
    // Reads the paths in child processes; `answered(path, answer)` for each one
    // read, with its "plugins" and "error".
    void read(const QStringList& paths, const Progress& progress, const Cancelled& cancelled,
              const std::function<void(const QString&, QJsonObject)>& answered) const;

    QString program_;
    QStringList arguments_;
    QString cacheFile_;
    double timeout_;
    std::optional<QStringList> folders_;
};

}  // namespace sub::app
