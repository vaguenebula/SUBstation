#include "plugins/PluginScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "Text.h"
#include "browser/PathKeys.h"

namespace sub::app {

namespace {

using Clock = std::chrono::steady_clock;

// A path as a JSON string, as Python's json.dumps wrote it (non-ASCII escaped).
QByteArray jsonString(const QString& text) {
    QByteArray out = "\"";
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        switch (u) {
            case u'"': out += "\\\""; break;
            case u'\\': out += "\\\\"; break;
            case u'\n': out += "\\n"; break;
            case u'\r': out += "\\r"; break;
            case u'\t': out += "\\t"; break;
            case u'\b': out += "\\b"; break;
            case u'\f': out += "\\f"; break;
            default:
                if (u < 0x20 || u > 0x7E)
                    out += "\\u" + QByteArray::number(u, 16).rightJustified(4, '0');
                else
                    out += static_cast<char>(u);
        }
    }
    return out + "\"";
}

std::optional<QJsonObject> jsonObject(const QByteArray& line) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    return document.object();
}

QJsonValue signatureJson(const std::optional<std::array<int64_t, 2>>& signature) {
    if (!signature) return QJsonValue();
    return QJsonArray{QJsonValue(static_cast<qint64>((*signature)[0])), QJsonValue(static_cast<qint64>((*signature)[1]))};
}

// A file whose signature can't be read never matches.
bool sameSignature(const QJsonValue& cached, const std::optional<std::array<int64_t, 2>>& signature) {
    if (!signature || !cached.isArray()) return false;
    const QJsonArray array = cached.toArray();
    if (array.size() != 2) return false;
    constexpr qint64 kNone = std::numeric_limits<qint64>::min();
    return array[0].toInteger(kNone) == (*signature)[0] && array[1].toInteger(kNone) == (*signature)[1];
}

enum class Wait { Answer, Ended, TimedOut, Cancelled };

// One child process reading plug-in files.
class Worker {
public:
    Worker(const QString& program, const QStringList& arguments) {
        process_.setProgram(program);
        process_.setArguments(arguments);
        process_.setStandardErrorFile(QProcess::nullDevice());
#ifdef _WIN32
        process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
            args->flags |= 0x08000000;  // CREATE_NO_WINDOW: no console window flashing up
        });
#endif
        process_.start(QIODevice::ReadWrite);
    }

    // All the paths at once, then the end of its input (it ends when that is
    // read). QProcess writes them as the child takes them: a plug-in that hangs
    // stops it reading, and that must not stop us.
    void feed(const QStringList& paths) {
        QByteArray lines;
        for (const QString& path : paths) lines += jsonString(path) + '\n';
        process_.write(lines);
        process_.closeWriteChannel();
    }

    // The next JSON object it says, until `deadline`.
    Wait next(QJsonObject& out, Clock::time_point deadline, const PluginScanner::Cancelled& cancelled) {
        for (;;) {
            while (process_.canReadLine()) {
                if (auto object = jsonObject(process_.readLine())) {
                    out = std::move(*object);
                    return Wait::Answer;
                }
            }
            if (process_.state() == QProcess::NotRunning) {
                // A last line without its line break.
                if (process_.bytesAvailable() > 0) {
                    if (auto object = jsonObject(process_.readAll())) {
                        out = std::move(*object);
                        return Wait::Answer;
                    }
                }
                return Wait::Ended;
            }
            if (cancelled && cancelled()) return Wait::Cancelled;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) return Wait::TimedOut;
            process_.waitForReadyRead(static_cast<int>(std::min<long long>(left, 100)));
        }
    }

    // Its answer for `path`, within `timeout` seconds.
    Wait answer(const QString& path, QJsonObject& out, double timeout, const PluginScanner::Cancelled& cancelled) {
        const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout));
        for (;;) {
            QJsonObject line;
            const Wait wait = next(line, deadline, cancelled);
            if (wait != Wait::Answer) return wait;
            const QJsonValue answered = line.value(QStringLiteral("path"));
            if (answered.isString() && answered.toString() == path) {
                out = QJsonObject{{QStringLiteral("plugins"), line.value(QStringLiteral("plugins")).isUndefined()
                                                                   ? QJsonValue(QJsonArray())
                                                                   : line.value(QStringLiteral("plugins"))},
                                  {QStringLiteral("error"), line.value(QStringLiteral("error")).isUndefined()
                                                                 ? QJsonValue()
                                                                 : line.value(QStringLiteral("error"))}};
                return Wait::Answer;
            }
        }
    }

    // When done: it ends by itself once its input ends (up to 5 s); else, or at
    // once if it failed, it is killed.
    void finish(bool finished) {
        if (process_.state() == QProcess::NotRunning) return;
        if (finished && process_.waitForFinished(5000)) return;
        process_.kill();
        process_.waitForFinished(5000);
    }

private:
    QProcess process_;
};

}  // namespace

PluginScanner::PluginScanner(QString program, QString cacheFile, double timeout, std::optional<QStringList> folders,
                             QStringList arguments)
    : program_(program.isEmpty() ? defaultProgram() : std::move(program)),
      arguments_(std::move(arguments)),
      cacheFile_(cacheFile.isEmpty() ? pluginCachePath() : std::move(cacheFile)),
      timeout_(timeout),
      folders_(std::move(folders)) {}

QString PluginScanner::defaultProgram() {
    const QString overridden = qEnvironmentVariable("SUBSTATION_SCANNER");
    if (!overridden.isEmpty()) return overridden;
#ifdef _WIN32
    return QCoreApplication::applicationDirPath() + QStringLiteral("/substation-scan.exe");
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/substation-scan");
#endif
}

QJsonObject PluginScanner::loadCache() const {
    QFile file(cacheFile_);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto data = jsonObject(file.readAll());
    if (!data || data->value(QStringLiteral("version")).toInteger(-1) != kPluginCacheVersion) return {};
    const QJsonValue files = data->value(QStringLiteral("files"));
    return files.isObject() ? files.toObject() : QJsonObject();
}

void PluginScanner::saveCache(const QJsonObject& files) const {
    QDir().mkpath(QFileInfo(cacheFile_).absolutePath());
    QSaveFile file(cacheFile_);
    if (!file.open(QIODevice::WriteOnly)) return;  // a cache we can't write only costs time
    const QJsonObject data{{QStringLiteral("version"), kPluginCacheVersion}, {QStringLiteral("files"), files}};
    file.write(QJsonDocument(data).toJson(QJsonDocument::Indented));
    file.commit();
}

ScanResult PluginScanner::scan(const std::optional<QStringList>& filesGiven, bool rescan, const Progress& progress,
                               const Cancelled& cancelled) {
    const QStringList files = filesGiven ? *filesGiven : folders_ ? findPluginFiles(*folders_) : findPluginFiles();
    const QJsonObject cache = rescan ? QJsonObject() : loadCache();
    QHash<QString, QJsonObject> entries;
    QStringList todo;
    for (const QString& path : files) {
        const QString key = caseKey(path);
        const QJsonValue entry = cache.value(key);
        if (entry.isObject() && sameSignature(entry.toObject().value(QStringLiteral("signature")), pluginSignature(path)))
            entries.insert(key, entry.toObject());
        else
            todo << path;
    }

    read(todo, progress, cancelled, [&](const QString& path, QJsonObject answer) {
        answer.insert(QStringLiteral("path"), path);
        answer.insert(QStringLiteral("signature"), signatureJson(pluginSignature(path)));
        entries.insert(caseKey(path), answer);
    });

    // Keep what is known about files not scanned this time, while they exist.
    QSet<QString> scanned;
    for (const QString& path : files) scanned.insert(caseKey(path));
    QJsonObject saved;
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (scanned.contains(it.key()) || !it.value().isObject()) continue;
        const QString path = it.value().toObject().value(QStringLiteral("path")).toString();
        if (!path.isEmpty() && QFileInfo::exists(path)) saved.insert(it.key(), it.value());
    }
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) saved.insert(it.key(), it.value());
    saveCache(saved);

    ScanResult result;
    for (const QString& path : files) {
        const auto entry = entries.constFind(caseKey(path));
        if (entry == entries.cend()) continue;  // cancelled before this one
        const QJsonValue error = entry->value(QStringLiteral("error"));
        if (error.isString() && !error.toString().isEmpty())
            result.failures.push_back({path, friendlyScanReason(error.toString())});
        for (const QJsonValue& value : entry->value(QStringLiteral("plugins")).toArray()) {
            const QJsonObject p = value.toObject();
            PluginInfo info;
            info.name = p.value(QStringLiteral("name")).toString();
            if (info.name.isEmpty()) info.name = QFileInfo(path).completeBaseName();  // named after its file
            info.format = QStringLiteral("VST3");
            info.path = path;
            info.uid = p.value(QStringLiteral("uid")).toString();
            info.vendor = p.value(QStringLiteral("vendor")).toString();
            info.version = p.value(QStringLiteral("version")).toString();
            info.category = p.value(QStringLiteral("category")).toString();
            info.instrument = p.value(QStringLiteral("instrument")).toBool();
            result.plugins.push_back(std::move(info));
        }
    }
    // By name, then vendor, ignoring case (Python's str.lower).
    std::vector<std::pair<std::pair<std::string, std::string>, size_t>> order;
    for (size_t i = 0; i < result.plugins.size(); ++i) {
        const PluginInfo& p = result.plugins[i];
        order.push_back({{browser::pyLower(p.name.toStdString()), browser::pyLower(p.vendor.toStdString())}, i});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<PluginInfo> sorted;
    sorted.reserve(order.size());
    for (const auto& [_, i] : order) sorted.push_back(std::move(result.plugins[i]));
    result.plugins = std::move(sorted);
    return result;
}

void PluginScanner::read(const QStringList& paths, const Progress& progress, const Cancelled& cancelled,
                         const std::function<void(const QString&, QJsonObject)>& answered) const {
    const int total = static_cast<int>(paths.size());
    int done = 0;
    QStringList remaining = paths;
    while (!remaining.isEmpty()) {
        if (cancelled && cancelled()) return;
        const QStringList batch = std::exchange(remaining, {});
        Worker worker(program_, arguments_);
        worker.feed(batch);
        QJsonObject ready;
        const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout_));
        const Wait started = worker.next(ready, deadline, cancelled);
        if (started == Wait::Cancelled) {
            worker.finish(false);
            return;
        }
        if (started != Wait::Answer) {
            worker.finish(false);
            throw std::runtime_error("The plug-in scanner could not start.");
        }
        bool finished = true;
        for (qsizetype index = 0; index < batch.size(); ++index) {
            const QString& path = batch[index];
            if (cancelled && cancelled()) {
                worker.finish(false);
                return;
            }
            if (progress) progress(done, total, path);
            QJsonObject answer;
            const Wait wait = worker.answer(path, answer, timeout_, cancelled);
            if (wait == Wait::Cancelled) {
                worker.finish(false);
                return;
            }
            ++done;
            if (wait != Wait::Answer) {
                // This file crashed or hung the worker: note it, go on without it in a new one.
                const QString reason = wait == Wait::Ended ? QStringLiteral("The plug-in crashed while loading.")
                                                           : QStringLiteral("The plug-in timed out.");
                answered(path, QJsonObject{{QStringLiteral("error"), reason}, {QStringLiteral("plugins"), QJsonArray()}});
                remaining = batch.mid(index + 1);
                finished = false;
                break;
            }
            answered(path, answer);
        }
        worker.finish(finished);
    }
}

}  // namespace sub::app
