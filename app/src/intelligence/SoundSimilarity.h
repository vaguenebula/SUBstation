#pragma once
// Sound similarity (the intelligence module, sub_intelligence) on the
// application thread's side: Session.similarity.
//
// The module's SoundIndex fingerprints every file the browser's index lists,
// in the background (at background priority, on a quarter of the cores), keeps
// the fingerprints in sound-index.bin next to the browser's index, and finds
// the sounds most like a given one: a file, or part of one (an audio clip's).
// Nothing needs indexing by hand: a file the browser finds is analysed soon
// after, and one that changes is analysed again. A search from a sound with no
// fingerprint yet (a recording, a clip's part of a loop) analyses it then.
//
// Neither the index's threads nor the browser's call into the application:
// the index calls its wake callback, and SoundSimilarity takes what it has on
// its own thread (a queued call), emitting `found` and `progressChanged`.
//
// The browser shows a search's result as its "similar" sort
// (BrowserController::findSimilar). The sampler and the drum rack can step
// through a result's best() to swap in similar sounds.

#include <QList>
#include <QMetaObject>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "similarity/SoundIndex.h"

namespace sub::app {

class FileIndex;

// What a search for similar sounds found. Cheap to copy.
class SimilarSounds {
public:
    SimilarSounds() = default;
    explicit SimilarSounds(std::shared_ptr<const intelligence::SimilarityResult> result);

    bool isNull() const { return !result_; }
    uint64_t generation() const;
    QString path() const;    // the sound searched from (Qt's form)
    double start() const;    // its part: seconds into the file
    double length() const;   // seconds (< 0: to the end)
    QString error() const;   // why it couldn't be analysed ("" if it was)
    int count() const;       // the library's files with a similarity
    double searchMs() const;

    // A file's similarity (0..1; NaN if it has none: not in the library, or not analysed yet).
    double similarity(const QString& path) const;
    // The most similar files, best first, not the sound itself: (path, similarity).
    QList<QPair<QString, double>> best(int count = 20) const;
    // For the browser's "similar" sort: a file's similarity by its path in the
    // backend's form (sub::browser::Query::score). Keeps the result alive.
    std::function<double(std::string_view)> scorer() const;

private:
    std::shared_ptr<const intelligence::SimilarityResult> result_;
};

class SoundSimilarity : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool analysing READ analysing NOTIFY progressChanged)  // files wait to be analysed or checked
    Q_PROPERTY(int libraryFiles READ libraryFiles NOTIFY progressChanged)
    Q_PROPERTY(int analysedFiles READ analysedFiles NOTIFY progressChanged)
    Q_PROPERTY(int failedFiles READ failedFiles NOTIFY progressChanged)

public:
    // sound-index.bin in localDataDir(), or SUBSTATION_SOUND_INDEX if set.
    static QString defaultStorePath();

    struct Options {
        std::optional<QString> storePath;  // none: defaultStorePath(); empty: not saved
        unsigned threads = 0;              // analysers; 0: a quarter of the cores, one to four
        bool analyse = true;               // analyse the library (off: only the sounds searched from; tests)
        bool background = true;            // at background priority
        double refreshSeconds = 1.0;       // the least time between takings of the library
    };

    explicit SoundSimilarity(QObject* parent = nullptr);
    SoundSimilarity(Options options, QObject* parent = nullptr);
    ~SoundSimilarity() override;

    // Analyse the files this browser index lists (null: none), taken again
    // whenever it changes. Call with null before the index closes: once this
    // returns, the previous index is never read again.
    void setLibrary(FileIndex* index);

    // Starts a search for the sounds most like the file at `path` (Qt's form),
    // or its part from `start` seconds in, `length` seconds long (< 0: to its
    // end). The result comes as `found`; returns its generation.
    uint64_t find(const QString& path, double start = 0.0, double length = -1.0);

    bool analysing() const { return status_.analysing; }
    int libraryFiles() const { return static_cast<int>(status_.library); }
    int analysedFiles() const { return static_cast<int>(status_.analysed); }
    int failedFiles() const { return static_cast<int>(status_.failed); }

    // Takes what the index has now (what the wake callback leads to; tests call it).
    void take();
    // Blocks until the library is analysed and no search runs (tests).
    bool waitIdle(double seconds = 30.0);
    // Stops the index's threads (saving). Idempotent.
    void close();

    intelligence::SoundIndex& backend() { return *index_; }

Q_SIGNALS:
    void found(const sub::app::SimilarSounds& result);  // of the latest find()
    void progressChanged();

private:
    std::unique_ptr<intelligence::SoundIndex> index_;
    QPointer<FileIndex> library_;
    QMetaObject::Connection libraryUpdates_;
    intelligence::SoundIndexStatus status_;
    bool closed_ = false;
};

}  // namespace sub::app

Q_DECLARE_METATYPE(sub::app::SimilarSounds)
