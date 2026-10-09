#include "io/Json.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

namespace sub::app {

namespace {

std::optional<QJsonDocument> parse(const QByteArray& bytes) {
    QJsonParseError error;
    QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) return std::nullopt;
    return document;
}

}  // namespace

std::optional<QJsonObject> parseJsonObject(const QByteArray& bytes) {
    const auto document = parse(bytes);
    if (!document || !document->isObject()) return std::nullopt;
    return document->object();
}

std::optional<QJsonArray> parseJsonArray(const QByteArray& bytes) {
    const auto document = parse(bytes);
    if (!document || !document->isArray()) return std::nullopt;
    return document->array();
}

bool writeJsonFile(const QString& path, const QJsonObject& data, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(data).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    return true;
}

QJsonObject readVersionedObject(const QString& path, int version, const QString& key) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto data = parseJsonObject(file.readAll());
    if (!data || data->value(QStringLiteral("version")).toInteger(-1) != version) return {};
    const QJsonValue kept = data->value(key);
    return kept.isObject() ? kept.toObject() : QJsonObject();
}

void writeVersionedObject(const QString& path, int version, const QString& key, const QJsonObject& object) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    writeJsonFile(path, QJsonObject{{QStringLiteral("version"), version}, {key, object}});
}

}  // namespace sub::app
