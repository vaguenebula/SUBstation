#pragma once
// The browser backend (sub_browser) on the application thread's side.
//
// The backend indexes the audio files under the places and runs every search
// of the browser's lists, on threads of its own:
//
// - one keeps an index of the folders under the places, at background CPU and
//   I/O priority. It is saved (browser-index.bin next to the plug-in cache), so
//   the next start shows it at once and only lists folders that changed; while
//   running it watches the places and lists again what changed in them.
// - another runs searches over snapshots of it. A new search stops the one
//   running, and only the latest one's results are handed out.
//
// Neither calls into the application. When there are results, or the index
// changed, the backend calls its wake callback; FileIndex then takes them on
// its own thread (a queued call) and emits `results` and `updated`.

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Browser.h"
#include "browser/BrowserItem.h"
#include "browser/Library.h"

namespace sub::app {

// A search's results, read a page at a time (see ItemListModel). Cheap to copy.
class SearchResult {
public:
    SearchResult() = default;
    SearchResult(std::shared_ptr<const browser::Result> result, QMap<QString, BrowserItem> items);

    bool isNull() const { return !result_; }
    int total() const;
    uint64_t generation() const;
    double searchMs() const;  // how long the search took on its thread
    // Rows [start, start + count): files are made fresh, other items are the
    // ones handed over with FileIndex::setItems() (by key) as they were when
    // the search ran.
    std::vector<BrowserItem> items(int start, int count) const;
    // The row of an item (an indexed file by path, others by key), or -1.
    int find(const BrowserItem& item) const;

private:
    std::shared_ptr<const browser::Result> result_;
    QMap<QString, BrowserItem> items_;
};

// The library's use counts as the backend takes them: (key, score, last used or NaN).
std::vector<browser::UsageRecord> usageRecords(const Library& library);

// A place as the backend takes it: its root (as given), its key (pathKey()) and
// the detail its own files show (the root's name). In the backend's form.
browser::PlaceSpec placeSpec(const QString& root);

class FileIndex : public QObject {
    Q_OBJECT

public:
    static constexpr uint32_t kMaxFiles = 300000;  // per place
    static constexpr uint32_t kMaxDepth = 16;

    // The audio files listed (lower case, with the dot): .wav, .wave, .flac, .mp3.
    static QStringList audioExtensions();
    static bool isAudioFile(const QString& path);
    // browser-index.bin in localDataDir(), or SUBSTATION_BROWSER_INDEX if set.
    static QString defaultIndexPath();

    // `indexPath`: where the index is saved (none: defaultIndexPath(); empty: nowhere).
    explicit FileIndex(QObject* parent = nullptr, std::optional<QString> indexPath = std::nullopt,
                       uint32_t maxFiles = kMaxFiles, uint32_t maxDepth = kMaxDepth);
    ~FileIndex() override;

    bool indexing() const;
    int fileCount() const;

    // Index these places: new ones are scanned, gone ones dropped, the rest kept.
    void setPlaces(const QStringList& places);
    // Index these places, listing every folder again.
    void rebuild(const QStringList& places);

    // Other items to list (built-in devices, plug-ins, presets), each with a tag to filter by.
    void setItems(int group, const std::vector<std::pair<BrowserItem, QString>>& items);
    void setUsage(const Library& library);

    // Starts a search (stopping any that runs); its results come as `results`.
    // `sort` is "rank" or "name"; `now` is the library's time (for how recent uses are).
    uint64_t search(const QString& text, const QString& sort, double now, const std::vector<int>& groups,
                    const QString& tag = {}, const std::string& placePrefix = {});

    // Blocks until the index settled and no search runs (tests, benchmarks).
    bool waitIdle(double seconds = 10.0);
    // Takes what the backend has now (what the wake callback leads to; tests call it directly).
    void take();
    // Stops the backend's threads (saving the index). Idempotent.
    void close();

    browser::Browser& backend() { return *backend_; }

signals:
    void updated();                             // files were found or went, or indexing started or stopped
    void results(const sub::app::SearchResult& result);  // of the latest search()

private:
    std::unique_ptr<browser::Browser> backend_;
    QMap<int, QMap<QString, BrowserItem>> groups_;  // by group number: later groups win a key both have
    QMap<QString, BrowserItem> items_;  // the other items, by key
    std::pair<bool, uint64_t> state_{true, 0};  // indexing, index version
    bool closed_ = false;
};

}  // namespace sub::app
