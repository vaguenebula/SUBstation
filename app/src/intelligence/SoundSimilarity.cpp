#include "intelligence/SoundSimilarity.h"

#include <QMetaObject>

#include <cmath>
#include <limits>

#include "Browser.h"
#include "browser/FileIndex.h"
#include "browser/PathKeys.h"

namespace sub::app {

// --- SimilarSounds ---------------------------------------------------------------------

SimilarSounds::SimilarSounds(std::shared_ptr<const intelligence::SimilarityResult> result) : result_(std::move(result)) {}

uint64_t SimilarSounds::generation() const { return result_ ? result_->generation : 0; }

QString SimilarSounds::path() const { return result_ ? fromBackendPath(result_->query.path) : QString(); }

double SimilarSounds::start() const { return result_ ? result_->query.start : 0.0; }

double SimilarSounds::length() const { return result_ ? result_->query.length : -1.0; }

QString SimilarSounds::error() const { return result_ ? QString::fromStdString(result_->error) : QString(); }

int SimilarSounds::count() const { return result_ ? static_cast<int>(result_->scored) : 0; }

double SimilarSounds::searchMs() const { return result_ ? result_->searchMs : 0.0; }

double SimilarSounds::similarity(const QString& path) const {
    if (!result_) return std::numeric_limits<double>::quiet_NaN();
    return result_->similarity(toBackendPath(normalPath(path)));
}

QList<QPair<QString, double>> SimilarSounds::best(int count) const {
    QList<QPair<QString, double>> out;
    if (!result_) return out;
    for (const auto& match : result_->best) {
        if (out.size() >= count) break;
        out.append({fromBackendPath(match.path), match.similarity});
    }
    return out;
}

std::function<double(std::string_view)> SimilarSounds::scorer() const {
    if (!result_) return {};
    return [result = result_](std::string_view path) { return static_cast<double>(result->similarity(path)); };
}

// --- SoundSimilarity -------------------------------------------------------------------

QString SoundSimilarity::defaultStorePath() {
    const QString overridden = qEnvironmentVariable("SUBSTATION_SOUND_INDEX");
    if (!overridden.isEmpty()) return overridden;
    return localDataDir() + QStringLiteral("/sound-index.bin");
}

SoundSimilarity::SoundSimilarity(QObject* parent) : SoundSimilarity(Options(), parent) {}

SoundSimilarity::SoundSimilarity(Options options, QObject* parent) : QObject(parent) {
    intelligence::SoundIndexOptions o;
    const QString store = options.storePath ? *options.storePath : defaultStorePath();
    o.store = store.isEmpty() ? std::string() : toBackendPath(store);
    o.threads = options.threads;
    o.analyse = options.analyse;
    o.background = options.background;
    o.refreshSeconds = options.refreshSeconds;
    index_ = std::make_unique<intelligence::SoundIndex>(std::move(o));
    // From the index's threads: take it on this object's thread.
    index_->setWakeCallback([this] { QMetaObject::invokeMethod(this, [this] { take(); }, Qt::QueuedConnection); });
}

SoundSimilarity::~SoundSimilarity() { close(); }

void SoundSimilarity::setLibrary(FileIndex* index) {
    if (libraryUpdates_) disconnect(libraryUpdates_);
    library_ = index;
    if (!index || closed_) {
        index_->setLibrarySource(nullptr);
        return;
    }
    // The browser's files, as its index lists them (folder by folder, which
    // keeps the analysers on one part of the disk at a time). Called on the
    // keeper's thread, which reads only the browser's immutable snapshots;
    // an unchanged one isn't handed over again.
    browser::Browser* browser = &index->backend();
    auto lastVersion = std::make_shared<uint64_t>(std::numeric_limits<uint64_t>::max());
    index_->setLibrarySource([browser, lastVersion]() -> std::shared_ptr<const intelligence::SoundIndex::Library> {
        const std::shared_ptr<const browser::Snapshot> snapshot = browser->snapshot();
        if (!snapshot || snapshot->version == *lastVersion) return nullptr;
        *lastVersion = snapshot->version;
        auto files = std::make_shared<intelligence::SoundIndex::Library>();
        files->reserve(snapshot->audio.size());
        for (const browser::SnapFolder& folder : snapshot->folders)
            for (size_t i = 0; i < folder.files->size(); ++i)
                files->push_back(browser::Snapshot::join(folder.path, folder.files->name(i)));
        return files;
    });
    libraryUpdates_ = connect(index, &FileIndex::updated, this, [this] { index_->libraryChanged(); });
}

uint64_t SoundSimilarity::find(const QString& path, double start, double length) {
    intelligence::SoundQuery query;
    query.path = toBackendPath(normalPath(path));
    query.start = std::max(0.0, start);
    query.length = length;
    return index_->find(std::move(query));
}

void SoundSimilarity::take() {
    if (closed_) return;
    const intelligence::SoundIndex::Update update = index_->take();
    if (update.result) Q_EMIT found(SimilarSounds(update.result));
    const auto& s = update.status;
    const bool changed = s.analysing != status_.analysing || s.library != status_.library ||
                         s.analysed != status_.analysed || s.failed != status_.failed;
    status_ = s;
    if (changed) Q_EMIT progressChanged();
}

bool SoundSimilarity::waitIdle(double seconds) { return index_->waitIdle(seconds); }

void SoundSimilarity::close() {
    if (closed_) return;
    closed_ = true;
    if (libraryUpdates_) disconnect(libraryUpdates_);
    index_->setLibrarySource(nullptr);
    index_->close();
}

}  // namespace sub::app
