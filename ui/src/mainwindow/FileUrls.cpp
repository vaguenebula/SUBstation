#include "FileUrls.h"

#include <QDir>
#include <QFileInfo>

namespace sub::ui {

QString FileUrls::localPath(const QUrl& url) { return url.isLocalFile() ? url.toLocalFile() : QString(); }

QUrl FileUrls::fileUrl(const QString& path) { return path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path); }

QUrl FileUrls::folderUrl(const QString& path) {
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    return QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
}

QString FileUrls::fileName(const QString& path) { return QFileInfo(path).fileName(); }

QUrl FileUrls::homeUrl() { return QUrl::fromLocalFile(QDir::homePath()); }

}  // namespace sub::ui
