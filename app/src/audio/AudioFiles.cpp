#include "audio/AudioFiles.h"

#include "browser/FileIndex.h"
#include "model/Project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>

namespace sub::app {

namespace {

void appendLe16(QByteArray& bytes, quint16 value) {
    char data[2];
    qToLittleEndian(value, data);
    bytes.append(data, 2);
}

void appendLe32(QByteArray& bytes, quint32 value) {
    char data[4];
    qToLittleEndian(value, data);
    bytes.append(data, 4);
}

// The folder a saved project's files go in ("Recordings", "Freeze", ...), or none if it isn't saved.
QString besideProject(const Project& project, const QString& name) {
    if (project.path().isEmpty()) return {};
    return QFileInfo(project.path()).absoluteDir().filePath(name);
}

}  // namespace

QStringList audioExtensions() { return FileIndex::audioExtensions(); }

bool isAudioFile(const QString& path) { return FileIndex::isAudioFile(path); }

QString recordingsFolder(const Project& project) {
    const QString beside = besideProject(project, QStringLiteral("Recordings"));
    if (!beside.isEmpty()) return beside;
    const QString env = qEnvironmentVariable("SUBSTATION_RECORDINGS");
    if (!env.isEmpty()) return env;
    QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (music.isEmpty()) music = QDir::homePath();
    return QDir(music).filePath(QStringLiteral("SUBstation/Recordings"));
}

QString takePath(const QString& folder, const QString& trackName, const QDateTime& when) {
    QString name;
    for (const QChar c : trackName) {
        const bool forbidden = QStringLiteral("<>:\"/\\|?*").contains(c) || c.unicode() < 0x20;
        name.append(forbidden ? QChar(u'_') : c);
    }
    // Spaces and dots go from both ends (Windows drops them from file names).
    qsizetype first = 0;
    qsizetype last = name.size();
    while (first < last && (name[first] == u' ' || name[first] == u'.')) ++first;
    while (last > first && (name[last - 1] == u' ' || name[last - 1] == u'.')) --last;
    name = name.mid(first, last - first);
    if (name.isEmpty()) name = QStringLiteral("Audio");
    const QString stem = name + u' ' + when.toString(QStringLiteral("yyyy-MM-dd HHmmss"));
    const QDir dir(folder);
    QString path = dir.filePath(stem + QStringLiteral(".wav"));
    for (int n = 2; QFileInfo::exists(path); ++n) {
        path = dir.filePath(stem + u' ' + QString::number(n) + QStringLiteral(".wav"));
    }
    return path;
}

QString freezeFolder(const Project& project) {
    const QString beside = besideProject(project, QStringLiteral("Freeze"));
    return !beside.isEmpty() ? beside : QDir(recordingsFolder(project)).filePath(QStringLiteral("Freeze"));
}

QString reversedFolder(const Project& project) {
    const QString beside = besideProject(project, QStringLiteral("Reversed"));
    return !beside.isEmpty() ? beside : QDir(recordingsFolder(project)).filePath(QStringLiteral("Reversed"));
}

QString reversedPath(const QString& folder, const QString& source) {
    const QString stem = QFileInfo(source).completeBaseName();
    const QDir dir(folder);
    QString path = dir.filePath(stem + QStringLiteral(" R.wav"));
    for (int n = 2; QFileInfo::exists(path); ++n) {
        path = dir.filePath(stem + QStringLiteral(" R ") + QString::number(n) + QStringLiteral(".wav"));
    }
    return path;
}

QByteArray floatWavHeader(int channels, qint64 frames, int sampleRate) {
    const auto block = static_cast<quint32>(4 * channels);
    const auto size = static_cast<quint32>(frames * block);
    QByteArray header("RIFF");
    appendLe32(header, 4 + (8 + 18) + (8 + 4) + (8 + size));
    header.append("WAVEfmt ");
    appendLe32(header, 18);
    appendLe16(header, 3);  // IEEE float
    appendLe16(header, static_cast<quint16>(channels));
    appendLe32(header, static_cast<quint32>(sampleRate));
    appendLe32(header, static_cast<quint32>(sampleRate) * block);
    appendLe16(header, static_cast<quint16>(block));
    appendLe16(header, 32);
    appendLe16(header, 0);  // no extension
    header.append("fact");
    appendLe32(header, 4);
    appendLe32(header, static_cast<quint32>(frames));
    header.append("data");
    appendLe32(header, size);
    return header;
}

bool writeFloatWav(const QString& path, const std::vector<std::vector<float>>& channels, int sampleRate) {
    const auto count = static_cast<int>(channels.size());
    const qint64 frames = channels.empty() ? 0 : static_cast<qint64>(channels.front().size());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray data = floatWavHeader(count, frames, sampleRate);
    data.reserve(data.size() + static_cast<qsizetype>(frames * count * 4));
    for (qint64 i = 0; i < frames; ++i) {
        for (const std::vector<float>& channel : channels) {
            char bytes[4];
            qToLittleEndian(channel[static_cast<size_t>(i)], bytes);
            data.append(bytes, 4);
        }
    }
    return file.write(data) == data.size();
}

}  // namespace sub::app
