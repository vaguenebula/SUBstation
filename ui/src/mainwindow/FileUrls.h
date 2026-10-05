#pragma once

// File paths and the urls QtQuick.Dialogs' file dialogs take and give, for
// QML: the session's file functions take local paths.

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

namespace sub::ui {

class FileUrls : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit FileUrls(QObject* parent = nullptr) : QObject(parent) {}

    // A file url's local path ("" for none, or a url that isn't a local file).
    Q_INVOKABLE static QString localPath(const QUrl& url);
    // A local path's url (an empty url for "").
    Q_INVOKABLE static QUrl fileUrl(const QString& path);
    // The url of the folder a path is in (the path itself if it is a folder).
    Q_INVOKABLE static QUrl folderUrl(const QString& path);
    // The file's name, without its folder.
    Q_INVOKABLE static QString fileName(const QString& path);
    // The user's home folder, as a url (where folder dialogs start).
    Q_INVOKABLE static QUrl homeUrl();
};

}  // namespace sub::ui
