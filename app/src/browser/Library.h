#pragma once
// What the browser remembers about its items, beyond the files themselves.
//
// One record per item, keyed by BrowserItem::key(), in a JSON file next to the
// plug-in cache: {"version": 1, "items": {key: record}}. For now a record
// counts how often the item was used (added to the project from the browser);
// search results are ranked by that. Records are plain JSON objects, and
// fields this version does not know are kept, so later versions can add their
// own (hidden from search, sound features...) without losing anything. A file
// that can't be read or has another version counts as empty; a failed save is
// ignored (losing a use count is not worth an error).

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace sub::app {

class Library {
public:
    static constexpr int kVersion = 1;
    static constexpr double kHalfLifeDays = 30.0;  // a use counts half as much after this long

    // Seconds since 1970, as Python's time.time() (tests give their own).
    using Clock = std::function<double()>;
    static double systemClock();

    // library.json in localDataDir(), or SUBSTATION_LIBRARY if set.
    static QString defaultPath();

    explicit Library(QString path = {}, Clock clock = {});

    const QString& path() const { return path_; }
    double now() const { return clock_(); }

    // The items were used (added to the project) just now: each gets one more
    // use, and its score becomes its rank now plus 1. Saved at once.
    void recordUse(const QStringList& keys);
    int uses(const QString& key) const;
    // How much, and how recently, the item was used: each use counts 1 when it
    // happens, and half of that every kHalfLifeDays after. 0 if never used.
    double rank(const QString& key, std::optional<double> now = std::nullopt) const;

    // The records by key ("uses", "score", "last_used", and what else they hold).
    const QJsonObject& records() const { return records_; }
    void setRecord(const QString& key, const QJsonObject& record) { records_.insert(key, record); }

    void save() const;

    // Python's float() of a record's field: a number, or a string of one;
    // nothing for anything else (null, missing, a list...).
    static std::optional<double> number(const QJsonValue& value);

private:
    QJsonObject load() const;

    QString path_;
    Clock clock_;
    QJsonObject records_;
};

}  // namespace sub::app
