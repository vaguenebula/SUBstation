#include "TestSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <memory>

namespace sub::app::test {

namespace {

// Folders made for the whole run, gone when it ends.
std::vector<std::unique_ptr<QTemporaryDir>>& runFolders() {
    static std::vector<std::unique_ptr<QTemporaryDir>> folders;
    return folders;
}

QString runFolder(const QString& prefix) {
    auto dir = std::make_unique<QTemporaryDir>(QDir::tempPath() + u'/' + prefix + QStringLiteral("-XXXXXX"));
    const QString path = dir->path();
    runFolders().push_back(std::move(dir));
    return path;
}

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

}  // namespace

void prepareApplication() {
    QCoreApplication::setOrganizationName(QStringLiteral("SUBstation Tests"));  // keep tests out of the user's settings
    QCoreApplication::setApplicationName(QStringLiteral("SUBstation Tests"));
    QSettings().clear();
    qputenv("SUBSTATION_PRESETS", (runFolder(QStringLiteral("sub-presets")) + QStringLiteral("/Presets")).toUtf8());
    qputenv("SUBSTATION_LIBRARY", (runFolder(QStringLiteral("sub-library")) + QStringLiteral("/library.json")).toUtf8());
    qputenv("SUBSTATION_BROWSER_INDEX",
            (runFolder(QStringLiteral("sub-index")) + QStringLiteral("/browser-index.bin")).toUtf8());
    qputenv("SUBSTATION_PLUGIN_CACHE",
            (runFolder(QStringLiteral("sub-plugin-cache")) + QStringLiteral("/vst3-cache.json")).toUtf8());
    qputenv("SUBSTATION_RECORDINGS", (runFolder(QStringLiteral("sub-recordings")) + QStringLiteral("/Recordings")).toUtf8());
}

TempDir::TempDir() : dir_(QDir::tempPath() + QStringLiteral("/sub-test-XXXXXX")) {}

QString TempDir::path(const QString& name) const { return dir_.filePath(name); }

QString writeWav(const QString& path, const std::vector<float>& samples, int channels, int sampleRate) {
    QByteArray pcm;
    pcm.reserve(static_cast<qsizetype>(samples.size() * 2));
    for (float sample : samples) {
        // Rounded half to even, as the Python tests' numpy did.
        const double value = std::clamp(std::nearbyint(static_cast<double>(sample) * 32768.0), -32768.0, 32767.0);
        appendLe16(pcm, static_cast<quint16>(static_cast<qint16>(value)));
    }
    QByteArray bytes("RIFF");
    appendLe32(bytes, static_cast<quint32>(36 + pcm.size()));
    bytes.append("WAVEfmt ");
    appendLe32(bytes, 16);
    appendLe16(bytes, 1);  // PCM
    appendLe16(bytes, static_cast<quint16>(channels));
    appendLe32(bytes, static_cast<quint32>(sampleRate));
    appendLe32(bytes, static_cast<quint32>(sampleRate * channels * 2));
    appendLe16(bytes, static_cast<quint16>(channels * 2));
    appendLe16(bytes, 16);
    bytes.append("data");
    appendLe32(bytes, static_cast<quint32>(pcm.size()));
    bytes.append(pcm);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(bytes);
    return path;
}

Track makeTrack(const QString& id, const QString& name, const QString& kind, const std::optional<QString>& parent) {
    Track track;
    track.id = id;
    track.name = name;
    track.color = QStringLiteral("#ff94a6");
    track.kind = kind;
    track.parent = parent;
    return track;
}

Device makeDevice(const QString& id, const QString& kind, const QMap<QString, double>& params) {
    Device device;
    device.id = id;
    device.kind = kind;
    device.params = params;
    return device;
}

}  // namespace sub::app::test
