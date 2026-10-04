#include "browser/Library.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

#include "browser/PathKeys.h"

namespace sub::app {

double Library::systemClock() { return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0; }

QString Library::defaultPath() {
    const QString overridden = qEnvironmentVariable("SUBSTATION_LIBRARY");
    if (!overridden.isEmpty()) return overridden;
    return localDataDir() + QStringLiteral("/library.json");
}

Library::Library(QString path, Clock clock)
    : path_(path.isEmpty() ? defaultPath() : std::move(path)), clock_(clock ? std::move(clock) : Clock(&systemClock)) {
    records_ = load();
}

std::optional<double> Library::number(const QJsonValue& value) {
    if (value.isDouble()) return value.toDouble();
    if (value.isBool()) return value.toBool() ? 1.0 : 0.0;
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().trimmed().toDouble(&ok);
        if (ok) return parsed;
    }
    return std::nullopt;
}

bool Library::truthy(const QJsonValue& value) {
    switch (value.type()) {
        case QJsonValue::Bool: return value.toBool();
        case QJsonValue::Double: return value.toDouble() != 0.0;
        case QJsonValue::String: return !value.toString().isEmpty();
        case QJsonValue::Array: return !value.toArray().isEmpty();
        case QJsonValue::Object: return !value.toObject().isEmpty();
        default: return false;
    }
}

void Library::recordUse(const QStringList& keys) {
    const double time = clock_();
    for (const QString& key : keys) {
        QJsonObject record = records_.value(key).toObject();
        record.insert(QStringLiteral("uses"), static_cast<qint64>(number(record.value(QStringLiteral("uses"))).value_or(0)) + 1);
        records_.insert(key, record);  // (rank() reads the record as it was)
        record.insert(QStringLiteral("score"), rank(key, time) + 1.0);
        record.insert(QStringLiteral("last_used"), time);
        records_.insert(key, record);
    }
    if (!keys.isEmpty()) save();
}

int Library::uses(const QString& key) const {
    const QJsonValue record = records_.value(key);
    if (!record.isObject()) return 0;
    return static_cast<int>(number(record.toObject().value(QStringLiteral("uses"))).value_or(0));
}

double Library::rank(const QString& key, std::optional<double> now) const {
    const QJsonObject record = records_.value(key).toObject();
    if (record.isEmpty() || !truthy(record.value(QStringLiteral("score")))) return 0.0;
    const double time = now ? *now : clock_();
    const double last = number(record.value(QStringLiteral("last_used"))).value_or(time);
    const double days = std::max(0.0, time - last) / 86400.0;
    return number(record.value(QStringLiteral("score"))).value_or(0.0) * std::pow(0.5, days / kHalfLifeDays);
}

QJsonObject Library::load() const {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonObject data = document.object();
    if (data.value(QStringLiteral("version")).toInteger(-1) != kVersion) return {};
    const QJsonValue items = data.value(QStringLiteral("items"));
    if (!items.isObject()) return {};
    QJsonObject records;
    const QJsonObject all = items.toObject();
    for (auto it = all.begin(); it != all.end(); ++it)
        if (it.value().isObject()) records.insert(it.key(), it.value());
    return records;
}

void Library::save() const {
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) return;
    const QJsonObject data{{QStringLiteral("version"), kVersion}, {QStringLiteral("items"), records_}};
    file.write(QJsonDocument(data).toJson(QJsonDocument::Indented));
    file.commit();
}

}  // namespace sub::app
