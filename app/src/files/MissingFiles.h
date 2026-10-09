#pragma once
// Finding missing files again (the File Manager's Search and Locate): what is
// worked out from paths alone, and the search itself.
//
// A missing file is looked for by its name (ignoring case). Of several files
// of that name, the best is the one whose folders match most of the missing
// one's, from the file up: Samples/Drums/Kick.wav is more likely the old
// D:/Samples/Drums/Kick.wav than Downloads/Kick.wav is. A file found tells
// where its folder went (D:/Samples/Drums -> E:/Backup/Samples/Drums): the
// other files missing from that folder, or from folders beside it, are looked
// for at the same place (moveAlong), which is how one file located by hand
// finds the rest.
//
// The search (MissingFileSearch) runs on a thread of its own: it walks folders
// on disk (as the browser's index does: 16 folders deep, no names starting
// with "." or "$", no symbolic links to folders) and the browser's index of
// its places, collecting the files with a missing file's name, and matches
// them. It never calls into the application: `finished` is called on its
// thread, once, with what it found (or nothing, cancelled).

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace sub::browser {
struct Snapshot;
}

namespace sub::app::missing {

// A file's name as files are matched by: lower case.
QString nameKey(const QString& path);
// How many of their folders (and the file name) two paths share, from the
// file up, ignoring case: 1 for the name alone.
int sharedTail(const QString& a, const QString& b);
// The best of `candidates` (files found with the missing file's name) for the
// missing file at `missing`: the one sharing the longest tail of folders with
// it; of those, the first in order. "" if none has its name.
QString bestMatch(const QString& missing, const QStringList& candidates);
// The paths missing files might be at now that `from` was found at `to`:
// where `from`'s folders and `to`'s part, the rest of each missing path is
// tried under `to`'s side (D:/Samples/Drums/Kick.wav found at
// E:/Samples/Drums/Kick.wav: D:/Samples/Bass/Sub.wav is tried at
// E:/Samples/Bass/Sub.wav). Those `exists` says are there: missing path ->
// found path. Nothing if the two paths share nothing but the name's place (no
// folder to go by) and their folders differ.
QMap<QString, QString> moveAlong(const QString& from, const QString& to, const QStringList& missing,
                                 const std::function<bool(const QString&)>& exists);
// What the files found (by name key: nameKey()) say about the missing ones:
// each one's best match, then, for those found nothing for, where the others'
// folders went (moveAlong). Missing path -> found path.
QMap<QString, QString> match(const QStringList& missing, const QMultiHash<QString, QString>& found,
                             const std::function<bool(const QString&)>& exists);

// A search for missing files, on a thread of its own (from the moment it is made).
class MissingFileSearch {
public:
    struct Request {
        QStringList missing;   // the files missing
        QStringList folders;   // walked on disk
        std::shared_ptr<const browser::Snapshot> index;  // the browser's files (null: none)
    };
    // What it found (missing path -> found path), or none at all if it was
    // cancelled (`cancelled`); called on its thread.
    using Finished = std::function<void(QMap<QString, QString> found, bool cancelled)>;
    // Which folder it walks now (on its thread, now and then).
    using Progress = std::function<void(const QString& folder)>;

    static constexpr int kMaxDepth = 16;

    MissingFileSearch(Request request, Finished finished, Progress progress = {});
    ~MissingFileSearch();  // cancelled, and waited for
    MissingFileSearch(const MissingFileSearch&) = delete;
    MissingFileSearch& operator=(const MissingFileSearch&) = delete;

    void cancel() { cancel_.store(true, std::memory_order_relaxed); }

private:
    void run();
    void walk(const QString& folder, int depth, QMultiHash<QString, QString>& found);

    Request request_;
    Finished finished_;
    Progress progress_;
    QHash<QString, bool> wanted_;  // name keys of the files missing
    std::atomic<bool> cancel_{false};
    qint64 reported_ = 0;  // when the folder walked was last told (ms)
    std::thread thread_;
};

}  // namespace sub::app::missing
