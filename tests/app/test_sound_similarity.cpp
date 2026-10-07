// Find Similar: the sound similarity (SoundSimilarity, the intelligence
// module's index) analysing the files the browser lists, and the browser's
// list of the sounds most like one (BrowserController::findSimilar): its sort,
// filtering it, ending it, a clip's part of a file, a file outside the
// library, files found later, and a sound that can't be analysed.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "TestSupport.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "browser/BrowserController.h"
#include "browser/ItemListModel.h"
#include "browser/PathKeys.h"
#include "intelligence/SoundSimilarity.h"

using namespace sub::app;

namespace {

constexpr int kRate = test::kSampleRate;

// The same noise everywhere (std's distributions aren't).
struct Noise {
    uint32_t state;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 8388608.f - 1.f;
    }
};

std::vector<float> kick(double startHz, double endHz, double decay) {
    std::vector<float> s(size_t(0.6 * kRate));
    double phase = 0.0;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = double(i) / kRate;
        phase += 2.0 * std::numbers::pi * (endHz + (startHz - endHz) * std::exp(-t / 0.03)) / kRate;
        s[i] = float(0.9 * std::sin(phase) * std::exp(-t / decay));
    }
    return s;
}

std::vector<float> hat(double decay, uint32_t seed) {
    Noise noise{seed};
    std::vector<float> s(size_t(0.4 * kRate));
    float a = 0.f, b = 0.f;
    for (size_t i = 0; i < s.size(); ++i) {
        const float n = noise.next();
        s[i] = float(0.3 * (n - 2.f * a + b) * std::exp(-double(i) / kRate / decay));
        b = a;
        a = n;
    }
    return s;
}

std::vector<float> snare(double toneHz, double decay, uint32_t seed) {
    Noise noise{seed};
    std::vector<float> s(size_t(0.5 * kRate));
    float previous = 0.f;
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = double(i) / kRate;
        const float n = noise.next();
        const double tone = std::sin(2.0 * std::numbers::pi * toneHz * t) * std::exp(-t / 0.05);
        s[i] = float(0.6 * (0.3 * tone + 0.7 * (n - 0.6 * previous) * std::exp(-t / decay)));
        previous = n;
    }
    return s;
}

std::vector<float> join(std::vector<float> a, const std::vector<float>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<float> padded(std::vector<float> s, double seconds) {
    s.resize(size_t(seconds * kRate), 0.f);
    return s;
}

QStringList names(const ItemListModel& model) {
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row) out << model.item(row)->name;
    return out;
}

QString kindOf(const QString& name) {
    for (const char* kind : {"Kick", "Hat", "Snare", "Loop"})
        if (name.startsWith(QLatin1String(kind))) return QString::fromLatin1(kind);
    return {};
}

}  // namespace

class TestSoundSimilarity : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void init() {
        tmp_ = std::make_unique<QTemporaryDir>();
        QVERIFY(tmp_->isValid());
        QSettings().remove(QStringLiteral("browser"));
        qputenv("SUBSTATION_LIBRARY", path(QStringLiteral("library.json")).toUtf8());
        qputenv("SUBSTATION_PLUGIN_CACHE", path(QStringLiteral("vst3-cache.json")).toUtf8());
        qputenv("SUBSTATION_VST3_PATH", path(QStringLiteral("VST3")).toUtf8());
        lib_ = path(QStringLiteral("lib"));
        const double kicks[][3] = {{150, 50, 0.25}, {170, 45, 0.3}, {130, 55, 0.2}};
        for (int i = 0; i < 3; ++i) {
            write(QStringLiteral("Kicks/Kick %1.wav").arg(i + 1), kick(kicks[i][0], kicks[i][1], kicks[i][2]));
            write(QStringLiteral("Hats/Hat %1.wav").arg(i + 1), hat(0.02 + 0.01 * i, 10 + i));
            write(QStringLiteral("Snares/Snare %1.wav").arg(i + 1), snare(180 + 20 * i, 0.1 + 0.02 * i, 20 + i));
        }
        // A hat, then a kick, a second apart.
        write(QStringLiteral("Loops/Loop.wav"), join(padded(hat(0.03, 30), 1.0), kick(150, 50, 0.25)));
    }

    void cleanup() {
        browser_.reset();
        similarity_.reset();
        QSettings().remove(QStringLiteral("browser"));
        tmp_.reset();
    }

    void theBrowsersFilesAreAnalysedInTheBackground() {
        make();
        QVERIFY(settle());
        QCOMPARE(similarity_->libraryFiles(), 10);
        QCOMPARE(similarity_->analysedFiles(), 10);
        QCOMPARE(similarity_->failedFiles(), 0);
        QVERIFY(!similarity_->analysing());
        QVERIFY(browser_->canFindSimilar());
    }

    void findSimilarListsTheMostSimilarFirst() {
        make();
        QVERIFY(settle());
        browser_->setSort(QStringLiteral("name"));
        browser_->setSearchText(QStringLiteral("kick"));
        QVERIFY(settle());
        QSignalSpy sortChanged(browser_.get(), &BrowserController::sortChanged);
        QSignalSpy similarChanged(browser_.get(), &BrowserController::similarChanged);
        const QString from = lib_ + QStringLiteral("/Kicks/Kick 1.wav");
        browser_->findSimilar(from);
        QCOMPARE(browser_->similarTo(), from);
        QCOMPARE(browser_->similarName(), QStringLiteral("Kick 1.wav"));
        QCOMPARE(browser_->sort(), kSimilarSort);
        QCOMPARE(browser_->sorts().first().toMap().value(QStringLiteral("value")).toString(), kSimilarSort);
        QCOMPARE(browser_->sorts().first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Similarity"));
        QCOMPARE(browser_->searchText(), QString());  // (cleared)
        QVERIFY(sortChanged.count() >= 1);
        QCOMPARE(similarChanged.count(), 1);
        QVERIFY(browser_->searching());
        QVERIFY(settle());
        // Every sound, the sound itself first, then the other kicks.
        const QStringList shown = names(*browser_->results());
        QCOMPARE(shown.size(), 10);
        QCOMPARE(shown.first(), QStringLiteral("Kick 1.wav"));
        QCOMPARE(kindOf(shown[1]), QStringLiteral("Kick"));
        QCOMPARE(kindOf(shown[2]), QStringLiteral("Kick"));
        QCOMPARE(browser_->statusText(), QStringLiteral("10 sounds like Kick 1.wav"));
        // The settings keep the sort chosen before.
        QCOMPARE(QSettings().value(QStringLiteral("browser/sort")).toString(), QStringLiteral("name"));
    }

    void searchTextAndPlacesFilterIt() {
        make();
        QVERIFY(settle());
        browser_->findSimilar(lib_ + QStringLiteral("/Hats/Hat 1.wav"));
        QVERIFY(settle());
        browser_->setSearchText(QStringLiteral("snare"));
        QVERIFY(settle());
        QCOMPARE(names(*browser_->results()).size(), 3);
        for (const QString& name : names(*browser_->results())) QCOMPARE(kindOf(name), QStringLiteral("Snare"));
        QCOMPARE(browser_->sort(), kSimilarSort);
        // A place lists its files (not its folder tree) by similarity.
        browser_->setSearchText(QString());
        browser_->setScope({QStringLiteral("place"), lib_});
        QVERIFY(settle());
        QVERIFY(!browser_->showingTree());
        QCOMPARE(browser_->sort(), kSimilarSort);
        QCOMPARE(names(*browser_->results()).first(), QStringLiteral("Hat 1.wav"));
        QCOMPARE(kindOf(names(*browser_->results())[1]), QStringLiteral("Hat"));
    }

    void anotherSortOrListOrClearingEndsIt() {
        make();
        QVERIFY(settle());
        const QString from = lib_ + QStringLiteral("/Kicks/Kick 1.wav");
        // Another sort.
        browser_->findSimilar(from);
        QVERIFY(settle());
        browser_->setSort(QStringLiteral("name"));
        QVERIFY(settle());
        QCOMPARE(browser_->similarTo(), QString());
        QCOMPARE(browser_->sort(), QStringLiteral("name"));
        QCOMPARE(browser_->sorts().size(), 2);
        QCOMPARE(names(*browser_->results()).first(), QStringLiteral("Hat 1.wav"));
        // A list that isn't files.
        browser_->findSimilar(from);
        QVERIFY(settle());
        browser_->setScope({QStringLiteral("plugins")});
        QCOMPARE(browser_->similarTo(), QString());
        QCOMPARE(browser_->sort(), QStringLiteral("name"));
        // Clearing: the sort before comes back.
        browser_->setScope({QStringLiteral("samples")});
        browser_->findSimilar(from);
        QVERIFY(settle());
        QSignalSpy similarChanged(browser_.get(), &BrowserController::similarChanged);
        browser_->clearSimilar();
        QCOMPARE(similarChanged.count(), 1);
        QCOMPARE(browser_->sort(), QStringLiteral("name"));
        QVERIFY(settle());
        QCOMPARE(names(*browser_->results()).first(), QStringLiteral("Hat 1.wav"));
        QCOMPARE(browser_->statusText(), QStringLiteral("10 items"));
        // Ctrl+F (all items) ends it too.
        browser_->findSimilar(from);
        browser_->focusSearch();
        QCOMPARE(browser_->similarTo(), QString());
    }

    void aClipsPartOfAFileFindsItsKind() {
        make();
        QVERIFY(settle());
        auto firstOtherThanLoop = [this] {
            for (const QString& name : names(*browser_->results()))
                if (kindOf(name) != QStringLiteral("Loop")) return kindOf(name);
            return QString();
        };
        const QString loop = lib_ + QStringLiteral("/Loops/Loop.wav");
        browser_->findSimilar(loop, 0.0, 0.5);  // the hat
        QVERIFY(settle());
        QCOMPARE(firstOtherThanLoop(), QStringLiteral("Hat"));
        browser_->findSimilar(loop, 1.0, 0.6);  // the kick
        QVERIFY(settle());
        QCOMPARE(firstOtherThanLoop(), QStringLiteral("Kick"));
    }

    void aSoundOutsideTheLibrary() {
        make();
        QVERIFY(settle());
        const QString outside = path(QStringLiteral("project/Recorded.wav"));
        QDir().mkpath(QFileInfo(outside).absolutePath());
        test::writeWav(outside, kick(160, 48, 0.28));
        QSignalSpy found(similarity_.get(), &SoundSimilarity::found);
        browser_->findSimilar(outside);
        QVERIFY(settle());
        QCOMPARE(names(*browser_->results()).size(), 10);  // (not itself: it isn't in the library)
        QCOMPARE(kindOf(names(*browser_->results()).first()), QStringLiteral("Kick"));
        // What the search found, for the sampler and the drum rack to step through.
        QCOMPARE(found.count(), 1);
        const SimilarSounds result = found.at(0).at(0).value<SimilarSounds>();
        QCOMPARE(result.path(), outside);
        QVERIFY(result.error().isEmpty());
        QCOMPARE(result.count(), 10);
        const auto best = result.best(3);
        QCOMPARE(best.size(), 3);
        for (const auto& [file, similarity] : best) {
            QCOMPARE(kindOf(QFileInfo(file).fileName()), QStringLiteral("Kick"));
            QVERIFY(similarity > 0.0 && similarity <= 1.0);
        }
        QVERIFY(best[0].second >= best[1].second);
        QVERIFY(result.similarity(best[0].first) > result.similarity(lib_ + QStringLiteral("/Hats/Hat 1.wav")));
        QVERIFY(std::isnan(result.similarity(outside)));
    }

    void filesFoundLaterAreAnalysedToo() {
        make();
        QVERIFY(settle());
        write(QStringLiteral("Kicks/Kick 4.wav"), kick(140, 52, 0.26));
        browser_->rescan();
        QVERIFY(QTest::qWaitFor([this] { return similarity_->analysedFiles() == 11 && !similarity_->analysing(); }, 20000));
        browser_->findSimilar(lib_ + QStringLiteral("/Kicks/Kick 1.wav"));
        QVERIFY(settle());
        QVERIFY(names(*browser_->results()).mid(0, 4).contains(QStringLiteral("Kick 4.wav")));
    }

    void aSoundThatCantBeAnalysed() {
        make();
        QVERIFY(settle());
        const QString junk = path(QStringLiteral("junk.wav"));
        QFile file(junk);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a sound");
        file.close();
        QSignalSpy messages(browser_.get(), &BrowserController::statusMessage);
        browser_->findSimilar(junk);
        QVERIFY(settle());
        QCOMPARE(browser_->results()->total(), 0);
        QCOMPARE(browser_->statusText(), QStringLiteral("Could not analyse junk.wav"));
        QCOMPARE(messages.count(), 1);
        QVERIFY(messages.at(0).at(0).toString().startsWith(QStringLiteral("Could not analyse junk.wav: ")));
    }

    void audioFilesOfferFindSimilar() {
        make();
        QVERIFY(settle());
        auto actions = [this](int row) {
            QStringList ids;
            for (const QVariant& a : browser_->resultActions(row)) ids << a.toMap().value(QStringLiteral("action")).toString();
            return ids;
        };
        QCOMPARE(actions(0), QStringList({QStringLiteral("findSimilar"), QString(), QStringLiteral("showInFolder")}));
        // Without a sound similarity, not offered.
        browser_.reset();
        QSettings().setValue(QStringLiteral("browser/places"), QStringList{lib_});
        BrowserController::Options options;
        options.scanPlugins = false;
        options.indexPath = QString();
        browser_ = std::make_unique<BrowserController>(nullptr, options);
        QVERIFY(settle());
        QVERIFY(!browser_->canFindSimilar());
        QCOMPARE(actions(0), QStringList{QStringLiteral("showInFolder")});
        browser_->findSimilar(lib_ + QStringLiteral("/Kicks/Kick 1.wav"));
        QCOMPARE(browser_->similarTo(), QString());
    }

private:
    QString path(const QString& relative) const { return tmp_->filePath(relative); }

    void write(const QString& relative, const std::vector<float>& samples) {
        const QString file = lib_ + QLatin1Char('/') + relative;
        QDir().mkpath(QFileInfo(file).absolutePath());
        test::writeWav(file, samples);
    }

    void make() {
        QSettings().setValue(QStringLiteral("browser/places"), QStringList{lib_});
        SoundSimilarity::Options s;
        s.storePath = QString();
        s.threads = 2;
        s.background = false;
        s.refreshSeconds = 0.0;
        similarity_ = std::make_unique<SoundSimilarity>(s);
        BrowserController::Options options;
        options.scanPlugins = false;
        options.indexPath = QString();
        options.similarity = similarity_.get();
        browser_ = std::make_unique<BrowserController>(nullptr, options);
    }

    // Until nothing is on its way: indexing, analysing, searching.
    bool settle() {
        auto quiet = [this] {
            return !browser_->searching() && !browser_->indexing() && (!similarity_ || !similarity_->analysing());
        };
        for (int i = 0; i < 2; ++i) {
            if (!QTest::qWaitFor(quiet, 20000)) return false;
            QTest::qWait(50);  // (what the backends say next: their wakes are queued)
        }
        return quiet();
    }

    std::unique_ptr<QTemporaryDir> tmp_;
    QString lib_;
    std::unique_ptr<SoundSimilarity> similarity_;
    std::unique_ptr<BrowserController> browser_;
};

QTEST_GUILESS_MAIN(TestSoundSimilarity)
#include "test_sound_similarity.moc"
