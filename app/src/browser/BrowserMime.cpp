#include "browser/BrowserMime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QUrl>

#include <optional>

namespace sub::app {

namespace {

struct Payload {
    QList<QUrl> urls;
    QJsonArray plugins, devices, presets;
};

Payload payload(const std::vector<BrowserItem>& items) {
    Payload out;
    for (const BrowserItem& item : items) {
        switch (item.kind) {
            case ItemKind::Audio: out.urls << QUrl::fromLocalFile(item.path); break;
            case ItemKind::Plugin:
                if (item.plugin) out.plugins.append(QJsonObject::fromVariantMap(item.plugin->toRef()));
                break;
            case ItemKind::Device: out.devices.append(item.path); break;
            case ItemKind::Preset: out.presets.append(item.path); break;
        }
    }
    return out;
}

QByteArray compact(const QJsonArray& array) { return QJsonDocument(array).toJson(QJsonDocument::Compact); }

std::optional<QJsonArray> jsonArray(const QByteArray& data) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) return std::nullopt;
    return document.array();
}

QStringList strings(const QByteArray& data) {
    QStringList out;
    if (const auto array = jsonArray(data))
        for (const QJsonValue& value : *array)
            if (value.isString()) out << value.toString();
    return out;
}

QByteArray dataOf(const QMimeData* mime, const char* type) {
    const QString format = QString::fromLatin1(type);
    return mime && mime->hasFormat(format) ? mime->data(format) : QByteArray();
}

}  // namespace

QStringList browserMimeTypes() {
    return {QStringLiteral("text/uri-list"), QString::fromLatin1(kPluginMime), QString::fromLatin1(kDeviceMime),
            QString::fromLatin1(kPresetMime)};
}

std::unique_ptr<QMimeData> browserMimeData(const std::vector<BrowserItem>& items) {
    auto mime = std::make_unique<QMimeData>();
    const Payload p = payload(items);
    if (!p.urls.isEmpty()) mime->setUrls(p.urls);
    if (!p.plugins.isEmpty()) mime->setData(QString::fromLatin1(kPluginMime), compact(p.plugins));
    if (!p.devices.isEmpty()) mime->setData(QString::fromLatin1(kDeviceMime), compact(p.devices));
    if (!p.presets.isEmpty()) mime->setData(QString::fromLatin1(kPresetMime), compact(p.presets));
    return mime;
}

QVariantMap browserDragData(const std::vector<BrowserItem>& items) {
    QVariantMap out;
    const Payload p = payload(items);
    if (!p.urls.isEmpty()) {
        QString list;
        for (const QUrl& url : p.urls) list += url.toString(QUrl::FullyEncoded) + QStringLiteral("\r\n");
        out.insert(QStringLiteral("text/uri-list"), list);
    }
    if (!p.plugins.isEmpty()) out.insert(QString::fromLatin1(kPluginMime), QString::fromUtf8(compact(p.plugins)));
    if (!p.devices.isEmpty()) out.insert(QString::fromLatin1(kDeviceMime), QString::fromUtf8(compact(p.devices)));
    if (!p.presets.isEmpty()) out.insert(QString::fromLatin1(kPresetMime), QString::fromUtf8(compact(p.presets)));
    return out;
}

std::vector<PluginInfo> pluginRefs(const QByteArray& data) {
    std::vector<PluginInfo> refs;
    const auto array = jsonArray(data);
    if (!array) return {};
    for (const QJsonValue& value : *array) {
        const QJsonObject item = value.toObject();
        // format, uid and name are needed; the rest has defaults.
        if (!value.isObject() || !item.contains(QStringLiteral("format")) || !item.contains(QStringLiteral("uid")) ||
            !item.contains(QStringLiteral("name")))
            return {};
        PluginInfo ref;
        ref.format = item.value(QStringLiteral("format")).toVariant().toString();
        ref.uid = item.value(QStringLiteral("uid")).toVariant().toString();
        ref.name = item.value(QStringLiteral("name")).toVariant().toString();
        ref.vendor = item.value(QStringLiteral("vendor")).toVariant().toString();
        ref.path = item.value(QStringLiteral("path")).toVariant().toString();
        ref.instrument = item.value(QStringLiteral("instrument")).toVariant().toBool();
        refs.push_back(std::move(ref));
    }
    return refs;
}

std::vector<PluginInfo> pluginRefs(const QMimeData* mime) { return pluginRefs(dataOf(mime, kPluginMime)); }

QStringList deviceKinds(const QByteArray& data) { return strings(data); }

QStringList deviceKinds(const QMimeData* mime) { return deviceKinds(dataOf(mime, kDeviceMime)); }

QStringList presetPaths(const QByteArray& data) { return strings(data); }

QStringList presetPaths(const QMimeData* mime) { return presetPaths(dataOf(mime, kPresetMime)); }

QByteArray movedDevicesData(const QString& trackId, const QStringList& deviceIds) {
    return (QStringList{trackId} + deviceIds).join(u'\n').toUtf8();
}

std::optional<MovedDevices> movedDevices(const QByteArray& data) {
    QStringList lines = QString::fromUtf8(data).split(u'\n');
    if (lines.isEmpty() || lines.front().isEmpty()) return std::nullopt;
    MovedDevices moved;
    moved.trackId = lines.takeFirst();
    for (const QString& line : std::as_const(lines)) {
        if (!line.isEmpty()) moved.deviceIds << line;
    }
    return moved;
}

std::optional<MovedDevices> movedDevices(const QMimeData* mime) {
    if (mime == nullptr || !mime->hasFormat(QString::fromLatin1(kDeviceMoveMime))) return std::nullopt;
    return movedDevices(mime->data(QString::fromLatin1(kDeviceMoveMime)));
}

}  // namespace sub::app
