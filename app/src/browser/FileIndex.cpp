#include "browser/FileIndex.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <limits>

#include "platform/Paths.h"
#include "browser/BrowserSearch.h"
#include "browser/PathKeys.h"

namespace sub::app {

namespace {

QString fromUtf8(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

// --- SearchResult ------------------------------------------------------------------------

SearchResult::SearchResult(std::shared_ptr<const browser::Result> result, QMap<QString, BrowserItem> items)
    : result_(std::move(result)), items_(std::move(items)) {}

int SearchResult::total() const { return result_ ? static_cast<int>(result_->hits.size()) : 0; }

uint64_t SearchResult::generation() const { return result_ ? result_->generation : 0; }

double SearchResult::searchMs() const { return result_ ? result_->searchMs : 0.0; }

std::vector<BrowserItem> SearchResult::items(int start, int count) const {
    std::vector<BrowserItem> out;
    if (!result_ || start < 0 || count <= 0) return out;
    const size_t end = std::min(result_->hits.size(), static_cast<size_t>(start) + static_cast<size_t>(count));
    for (size_t i = static_cast<size_t>(start); i < end; ++i) {
        const browser::Hit& hit = result_->hits[i];
        if (hit.group == browser::kAudioGroup) {
            const browser::AudioRef ref = result_->snapshot->audio[hit.index];
            const browser::SnapFolder& folder = result_->snapshot->folders[ref.folder];
            const std::string name(folder.files->name(ref.file));
            out.push_back({fromBackendPath(name), fromBackendPath(browser::Snapshot::join(folder.path, name)),
                           ItemKind::Audio, fromBackendPath(folder.detail), std::nullopt, {}});
        } else {
            const browser::ExternalItem& item = result_->groups.at(static_cast<int>(hit.group))->items[hit.index];
            const auto known = items_.constFind(fromUtf8(item.key));
            if (known != items_.cend())
                out.push_back(*known);
            else
                out.push_back({fromUtf8(item.name), fromUtf8(item.path), static_cast<ItemKind>(item.kind),
                               fromUtf8(item.detail), std::nullopt, {}});
        }
    }
    return out;
}

int SearchResult::find(const BrowserItem& item) const {
    if (!result_) return -1;
    const QString key = item.key();
    if (item.kind == ItemKind::Audio && !items_.contains(key))
        return static_cast<int>(result_->find(browser::Kind::Audio, toBackendPath(item.path)));
    return static_cast<int>(result_->find(static_cast<browser::Kind>(item.kind), key.toStdString()));
}

// --- Helpers -----------------------------------------------------------------------------

std::vector<browser::UsageRecord> usageRecords(const Library& library) {
    std::vector<browser::UsageRecord> records;
    const QJsonObject& all = library.records();
    for (auto it = all.begin(); it != all.end(); ++it) {
        const QJsonObject record = it.value().toObject();
        double score = 0.0;
        if (Library::truthy(record.value(QStringLiteral("score")))) {
            const auto number = Library::number(record.value(QStringLiteral("score")));
            if (!number) continue;  // not a number: left out
            score = *number;
        }
        double last = std::numeric_limits<double>::quiet_NaN();
        const QJsonValue lastUsed = record.value(QStringLiteral("last_used"));
        if (!lastUsed.isNull() && !lastUsed.isUndefined()) {
            const auto number = Library::number(lastUsed);
            if (!number) continue;
            last = *number;
        }
        records.push_back({it.key().toStdString(), score, last});
    }
    return records;
}

browser::PlaceSpec placeSpec(const QString& root) {
    const QString normal = normalPath(root);
    const std::string native = toBackendPath(normal);
    return {native, platform::pathKey(native), toBackendPath(QFileInfo(normal).fileName())};
}

// --- FileIndex ---------------------------------------------------------------------------

QStringList FileIndex::audioExtensions() {
    return {QStringLiteral(".wav"), QStringLiteral(".wave"), QStringLiteral(".flac"), QStringLiteral(".mp3")};
}

bool FileIndex::isAudioFile(const QString& path) {
    const QString lower = path.toLower();
    const QStringList extensions = audioExtensions();
    return std::any_of(extensions.begin(), extensions.end(), [&](const QString& ext) { return lower.endsWith(ext); });
}

QString FileIndex::defaultIndexPath() {
    const QString overridden = qEnvironmentVariable("SUBSTATION_BROWSER_INDEX");
    if (!overridden.isEmpty()) return overridden;
    return localDataDir() + QStringLiteral("/browser-index.bin");
}

FileIndex::FileIndex(QObject* parent, std::optional<QString> indexPath, uint32_t maxFiles, uint32_t maxDepth)
    : QObject(parent) {
    const QString store = indexPath ? *indexPath : defaultIndexPath();
    browser::Limits limits;
    limits.maxFiles = maxFiles;
    limits.maxDepth = maxDepth;
    for (const QString& extension : audioExtensions()) limits.extensions.push_back(extension.toStdString());
    backend_ = std::make_unique<browser::Browser>(store.isEmpty() ? std::string() : toBackendPath(store),
                                                  std::move(limits));
    // From the backend's threads: take it on this object's thread.
    backend_->setWakeCallback([this] { QMetaObject::invokeMethod(this, [this] { take(); }, Qt::QueuedConnection); });
}

FileIndex::~FileIndex() { close(); }

bool FileIndex::indexing() const { return backend_->status().busy; }

int FileIndex::fileCount() const { return static_cast<int>(backend_->status().files); }

void FileIndex::setPlaces(const QStringList& places) {
    std::vector<browser::PlaceSpec> specs;
    for (const QString& place : places) specs.push_back(placeSpec(place));
    backend_->setPlaces(std::move(specs));
}

void FileIndex::rebuild(const QStringList& places) {
    setPlaces(places);
    backend_->rescan();
}

void FileIndex::setItems(int group, const std::vector<std::pair<BrowserItem, QString>>& items) {
    QMap<QString, BrowserItem> byKey;
    std::vector<browser::ExternalItem> external;
    external.reserve(items.size());
    for (const auto& [item, tag] : items) {
        const QString key = item.key();
        byKey.insert(key, item);
        browser::ExternalItem e;
        e.kind = static_cast<browser::Kind>(item.kind);
        e.name = item.name.toStdString();
        e.path = item.path.toStdString();
        e.detail = item.detail.toStdString();
        e.key = key.toStdString();
        e.tag = tag.toStdString();
        external.push_back(std::move(e));
    }
    groups_.insert(group, byKey);
    items_.clear();
    for (const auto& keyed : std::as_const(groups_))
        for (auto it = keyed.cbegin(); it != keyed.cend(); ++it) items_.insert(it.key(), it.value());
    backend_->setExternal(group, std::move(external));
}

void FileIndex::setUsage(const Library& library) { backend_->setUsage(usageRecords(library), Library::kHalfLifeDays); }

uint64_t FileIndex::search(const QString& text, const QString& sort, double now, const std::vector<int>& groups,
                           const QString& tag, const std::string& placePrefix,
                           std::function<double(std::string_view)> score) {
    browser::Query query;
    query.text = text.toStdString();
    query.sort = sort == QStringLiteral("name") ? browser::Sort::Name
                 : sort == kSimilarSort         ? browser::Sort::Score
                                                : browser::Sort::Rank;
    query.score = std::move(score);
    query.now = now;
    query.groups = groups;
    query.tag = tag.toStdString();
    query.placePrefix = placePrefix;
    return backend_->search(std::move(query));
}

bool FileIndex::waitIdle(double seconds) { return backend_->waitIdle(seconds); }

void FileIndex::take() {
    if (closed_) return;
    const browser::Browser::Update update = backend_->take();
    if (update.result) Q_EMIT results(SearchResult(update.result, items_));
    const std::pair<bool, uint64_t> state{update.status.busy, update.status.version};
    if (state != state_) {
        state_ = state;
        Q_EMIT updated();
    }
}

void FileIndex::close() {
    if (closed_) return;
    closed_ = true;
    backend_->close();
}

}  // namespace sub::app
