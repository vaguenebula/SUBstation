#pragma once
// JSON: what values in the application's files mean (truthy, optionalString),
// reading it from bytes and files, and writing a file so that it is never left
// half written.
//
// The browser's library (library.json) and the plug-in cache (vst3-cache.json)
// are files of ours that keep an object under a version: {"version": n, key:
// {...}}. What they keep only saves work, so one that can't be read (missing,
// not JSON, of another version) is taken as empty, and one that can't be
// written is left as it was.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <optional>

namespace sub::app {

// Whether a value counts as true, as Python's bool() has it (the files were
// Python's): false, 0, "", an empty list or object, null and nothing are false.
inline bool truthy(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Bool: return value.toBool();
    case QJsonValue::Double: return value.toDouble() != 0.0;
    case QJsonValue::String: return !value.toString().isEmpty();
    case QJsonValue::Array: return !value.toArray().isEmpty();
    case QJsonValue::Object: return !value.toObject().isEmpty();
    default: return false;
    }
}

// Text, or null for none.
inline QJsonValue optionalString(const std::optional<QString>& text) {
    return text ? QJsonValue(*text) : QJsonValue(QJsonValue::Null);
}

// The JSON object, or list, `bytes` hold; none if they don't hold one.
std::optional<QJsonObject> parseJsonObject(const QByteArray& bytes);
std::optional<QJsonArray> parseJsonArray(const QByteArray& bytes);

// Writes `data` to `path`, indented, without ever leaving a half-written file
// behind. Whether it did; if not, `error` (if given) says why.
bool writeJsonFile(const QString& path, const QJsonObject& data, QString* error = nullptr);

// What a file of ours keeps under `key`; empty if it can't be read, isn't one
// of this `version`, or holds no object there.
QJsonObject readVersionedObject(const QString& path, int version, const QString& key);
// Writes one (its folder made first), keeping `object` under `key`. A failure is ignored.
void writeVersionedObject(const QString& path, int version, const QString& key, const QJsonObject& object);

}  // namespace sub::app
