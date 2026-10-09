#include "files/MissingFiles.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <string>

#include "Browser.h"  // the browser's index (sub_browser: its Snapshot)
#include "audio/AudioFiles.h"
#include "browser/PathKeys.h"

namespace sub::app::missing {

namespace {

// A path's parts, '/' between them, "." and ".." gone.
QStringList parts(const QString& path) { return QDir::cleanPath(QDir::fromNativeSeparators(path)).split(u'/'); }

bool sameName(const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) == 0; }

// How many parts two lists share from their ends.
int sharedEnds(const QStringList& a, const QStringList& b) {
    int shared = 0;
    while (shared < a.size() && shared < b.size() && sameName(a[a.size() - 1 - shared], b[b.size() - 1 - shared]))
        ++shared;
    return shared;
}

QStringList sortedPaths(QStringList paths) {
    std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) {
        const int order = a.compare(b, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : a < b;
    });
    return paths;
}

}  // namespace

QString nameKey(const QString& path) { return QFileInfo(path).fileName().toLower(); }

int sharedTail(const QString& a, const QString& b) { return sharedEnds(parts(a), parts(b)); }

QString bestMatch(const QString& missing, const QStringList& candidates) {
    const QString name = nameKey(missing);
    QString best;
    int bestShared = 0;
    for (const QString& candidate : candidates) {
        if (nameKey(candidate) != name) continue;
        const int shared = sharedTail(missing, candidate);
        if (best.isEmpty() || shared > bestShared) {
            best = candidate;
            bestShared = shared;
        }
    }
    return best;
}

QMap<QString, QString> moveAlong(const QString& from, const QString& to, const QStringList& missing,
                                 const std::function<bool(const QString&)>& exists) {
    // Where the two folders part: what is left of each once the folders they
    // share at their ends are taken away.
    QStringList fromDir = parts(from);
    QStringList toDir = parts(to);
    fromDir.removeLast();
    toDir.removeLast();
    const int shared = sharedEnds(fromDir, toDir);
    const QString fromRoot = fromDir.mid(0, fromDir.size() - shared).join(u'/');
    const QString toRoot = toDir.mid(0, toDir.size() - shared).join(u'/');
    QMap<QString, QString> moved;
    if (sameName(fromRoot, toRoot)) return moved;  // (nothing moved: the same folder)
    const QString prefix = fromRoot + u'/';
    for (const QString& path : missing) {
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
        if (!clean.startsWith(prefix, Qt::CaseInsensitive)) continue;
        const QString candidate = toRoot + clean.mid(fromRoot.size());
        if (exists && exists(candidate)) moved.insert(path, candidate);
    }
    return moved;
}

QMap<QString, QString> match(const QStringList& missing, const QMultiHash<QString, QString>& found,
                             const std::function<bool(const QString&)>& exists) {
    QMap<QString, QString> result;
    for (const QString& path : missing) {
        const QString best = bestMatch(path, sortedPaths(found.values(nameKey(path))));
        if (!best.isEmpty()) result.insert(path, best);
    }
    // Where the files found went tells where the others did: a file there that
    // shares more of the missing one's folders wins.
    const QMap<QString, QString> byName = result;
    for (auto it = byName.constBegin(); it != byName.constEnd(); ++it) {
        const QMap<QString, QString> moved = moveAlong(it.key(), it.value(), missing, exists);
        for (auto m = moved.constBegin(); m != moved.constEnd(); ++m) {
            const auto had = result.constFind(m.key());
            if (had == result.constEnd() || sharedTail(m.key(), m.value()) > sharedTail(m.key(), *had))
                result.insert(m.key(), m.value());
        }
    }
    return result;
}

// --- The search -------------------------------------------------------------------------

MissingFileSearch::MissingFileSearch(Request request, Finished finished, Progress progress)
    : request_(std::move(request)), finished_(std::move(finished)), progress_(std::move(progress)) {
    for (const QString& path : request_.missing) wanted_.insert(nameKey(path), true);
    thread_ = std::thread([this] { run(); });
}

MissingFileSearch::~MissingFileSearch() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

void MissingFileSearch::run() {
    QMultiHash<QString, QString> found;
    // The browser's files first: they are in memory.
    if (request_.index) {
        for (const browser::SnapFolder& folder : request_.index->folders) {
            if (cancel_.load(std::memory_order_relaxed)) break;
            for (size_t i = 0; i < folder.files->size(); ++i) {
                const std::string_view name = folder.files->name(i);
                const QString key = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())).toLower();
                if (!wanted_.contains(key)) continue;
                found.insert(key, fromBackendPath(browser::Snapshot::join(folder.path, name)));
            }
        }
    }
    for (const QString& folder : request_.folders) {
        if (cancel_.load(std::memory_order_relaxed)) break;
        walk(folder, 0, found);
    }
    if (cancel_.load(std::memory_order_relaxed)) {
        if (finished_) finished_({}, true);
        return;
    }
    // (The same file found twice, by the index and on disk: once.)
    QMultiHash<QString, QString> unique;
    for (const QString& key : found.uniqueKeys()) {
        QStringList seen;
        for (const QString& path : found.values(key)) {
            const QString clean = QDir::cleanPath(path);
            if (seen.contains(clean, Qt::CaseInsensitive)) continue;
            seen << clean;
            unique.insert(key, clean);
        }
    }
    const QMap<QString, QString> matched =
        match(request_.missing, unique, [](const QString& path) { return QFileInfo(path).isFile(); });
    if (finished_) finished_(matched, cancel_.load(std::memory_order_relaxed));
}

void MissingFileSearch::walk(const QString& folder, int depth, QMultiHash<QString, QString>& found) {
    if (depth > kMaxDepth || cancel_.load(std::memory_order_relaxed)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (progress_ && now - reported_ >= 150) {
        reported_ = now;
        progress_(folder);
    }
    const QFileInfoList entries =
        QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        if (cancel_.load(std::memory_order_relaxed)) return;
        const QString name = entry.fileName();
        if (name.startsWith(u'.') || name.startsWith(u'$')) continue;
        if (entry.isDir()) {
            if (!entry.isSymLink()) walk(entry.filePath(), depth + 1, found);
            continue;
        }
        const QString key = name.toLower();
        if (wanted_.contains(key) && isAudioFile(name)) found.insert(key, entry.filePath());
    }
}

}  // namespace sub::app::missing
