#include "browser/PathKeys.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include "platform/Paths.h"

namespace sub::app {

QString normalPath(const QString& path) { return QDir::cleanPath(path); }

std::string toBackendPath(const QString& path) {
#ifdef _WIN32
    return QDir::toNativeSeparators(path).toStdString();
#else
    return QFile::encodeName(path).toStdString();
#endif
}

QString fromBackendPath(const std::string& path) {
#ifdef _WIN32
    return QDir::fromNativeSeparators(QString::fromStdString(path));
#else
    return QFile::decodeName(QByteArray::fromStdString(path));
#endif
}

QString caseKey(const QString& path) {
    const std::string key = platform::pathKey(toBackendPath(path));
#ifdef _WIN32
    return QString::fromStdString(key);  // in the system's form: backslashes, as Python made keys
#else
    return QFile::decodeName(QByteArray::fromStdString(key));
#endif
}

QString pathKey(const QString& path) { return caseKey(normalPath(path)); }

bool sameFolder(const QString& a, const QString& b) { return pathKey(a) == pathKey(b); }

QString audioKey(const QString& path) { return QStringLiteral("audio:") + pathKey(path); }

QString localDataDir() {
#ifdef _WIN32
    const QString base = qEnvironmentVariable("LOCALAPPDATA", QDir::home().filePath(QStringLiteral("AppData/Local")));
    return normalPath(QDir::fromNativeSeparators(base) + QStringLiteral("/SUBstation"));
#else
    return normalPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                      QStringLiteral("/SUBstation"));
#endif
}

}  // namespace sub::app
