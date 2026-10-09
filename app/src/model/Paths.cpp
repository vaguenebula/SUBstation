#include "model/Paths.h"

#include "platform/Paths.h"

#include <QDir>
#include <QFileInfo>

#include <utility>

namespace sub::app {

QString absoluteCleanPath(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }

QString pathIdentity(const QString& path) {
    QString clean = absoluteCleanPath(path);
    if constexpr (platform::kCaseSensitivePaths) {
        return clean;
    } else {
        return std::move(clean).toCaseFolded();
    }
}

bool samePath(const QString& a, const QString& b) { return pathIdentity(a) == pathIdentity(b); }

}  // namespace sub::app
