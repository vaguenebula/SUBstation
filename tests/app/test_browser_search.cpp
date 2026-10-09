// What the browser remembers about its items and how it asks for its lists:
// keys, use counts decaying and persisting (library.json), unknown fields
// kept, a bad file ignored, the use records the backend gets, match quality
// preferring name starts, rank putting used items first, and the sidebar
// entries' queries.

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <memory>
#include <vector>

#include "Browser.h"
#include "Platform.h"
#include "Search.h"
#include "Text.h"
#include "browser/BrowserItem.h"
#include "browser/BrowserSearch.h"
#include "browser/FileIndex.h"
#include "browser/Library.h"
#include "browser/PathKeys.h"

using namespace sub::app;
namespace backend = sub::browser;

namespace {

constexpr double kDay = 86400.0;

BrowserItem audio(const QString& name, const QString& folder = QStringLiteral("Drums")) {
    return {name, QStringLiteral("/Samples/%1/%2").arg(folder, name), ItemKind::Audio, folder, std::nullopt, {}};
}

QStringList names(const std::vector<BrowserItem>& items) {
    QStringList out;
    for (const BrowserItem& item : items) out << item.name;
    return out;
}

// Searches a list with the backend, as the browser does its lists.
std::vector<BrowserItem> find(const std::vector<BrowserItem>& items, const QString& query, const Library& library,
                              const QString& sort = QStringLiteral("rank")) {
    backend::Limits limits;
    for (const QString& ext : FileIndex::audioExtensions()) limits.extensions.push_back(ext.toStdString());
    backend::Browser browser("", limits);
    std::vector<backend::ExternalItem> external;
    for (const BrowserItem& item : items) {
        backend::ExternalItem e;
        e.kind = static_cast<backend::Kind>(item.kind);
        e.name = item.name.toStdString();
        e.path = item.path.toStdString();
        e.detail = item.detail.toStdString();
        e.key = item.key().toStdString();
        external.push_back(std::move(e));
    }
    browser.setExternal(1, std::move(external));
    browser.setUsage(usageRecords(library), Library::kHalfLifeDays);
    backend::Query q;
    q.text = query.toStdString();
    q.sort = sort == QStringLiteral("name") ? backend::Sort::Name : backend::Sort::Rank;
    q.now = library.now();
    q.groups = {1};
    browser.search(q);
    if (!browser.waitIdle(10)) return {};
    const auto result = browser.take().result;
    std::vector<BrowserItem> out;
    if (!result) return out;
    for (const backend::Hit& hit : result->hits) out.push_back(items[hit.index]);
    return out;
}

int matchQuality(const BrowserItem& item, const QString& query) {
    const auto terms = backend::pySplit(backend::pyLower(query.toStdString()));
    std::string joined;
    for (const auto& t : terms) joined += (joined.empty() ? "" : " ") + t;
    std::vector<uint32_t> scratch;
    return backend::matchQuality(backend::pyLower(item.name.toStdString()), item.kind == ItemKind::Audio, terms, joined,
                                 scratch);
}

}  // namespace

class TestBrowserSearch : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init() {
        tmp_ = std::make_unique<QTemporaryDir>();
        QVERIFY(tmp_->isValid());
        now_ = 1'000'000'000.0;
    }

    void keysIdentifyItems() {
#ifdef _WIN32
        // os.path.normcase(os.path.normpath()): one key whatever the case or the separators.
        QCOMPARE(BrowserItem({QStringLiteral("Kick.wav"), QStringLiteral("C:/Samples/Drums/Kick.wav"), ItemKind::Audio}).key(),
                 BrowserItem({QStringLiteral("Kick.wav"), QStringLiteral("c:\\samples\\drums\\KICK.WAV"), ItemKind::Audio}).key());
        QCOMPARE(audioKey(QStringLiteral("C:/Samples/Drums/Kick.wav")), QStringLiteral("audio:c:\\samples\\drums\\kick.wav"));
        QCOMPARE(BrowserItem({QStringLiteral("P"), QStringLiteral("C:/Presets/EQ/P.gilpreset"), ItemKind::Preset}).key(),
                 QStringLiteral("preset:C:\\Presets\\EQ\\P.gilpreset"));
#else
        // Normalised, but case kept: names that differ in case are different files here.
        QCOMPARE(audio(QStringLiteral("Kick.wav")).key(),
                 BrowserItem({QStringLiteral("Kick.wav"), QStringLiteral("/Samples//Drums/../Drums/Kick.wav"), ItemKind::Audio}).key());
        QCOMPARE(audio(QStringLiteral("Kick.wav")).key(), QStringLiteral("audio:/Samples/Drums/Kick.wav"));
        QVERIFY(audio(QStringLiteral("Kick.wav")).key() != audio(QStringLiteral("KICK.WAV")).key());
        QCOMPARE(BrowserItem({QStringLiteral("P"), QStringLiteral("/Presets/EQ/P.gilpreset"), ItemKind::Preset}).key(),
                 QStringLiteral("preset:/Presets/EQ/P.gilpreset"));
#endif
        PluginInfo ref;
        ref.format = QStringLiteral("VST3");
        ref.uid = QStringLiteral("abc");
        ref.name = QStringLiteral("Synth");
        ref.vendor = QStringLiteral("V");
        ref.path = QStringLiteral("/a.vst3");
        QCOMPARE(BrowserItem({QStringLiteral("Synth"), QStringLiteral("/a.vst3"), ItemKind::Plugin, QStringLiteral("V"), ref}).key(),
                 QStringLiteral("plugin:VST3:abc"));
        QCOMPARE(pluginItem(ref).key(), QStringLiteral("plugin:VST3:abc"));
        QCOMPARE(BrowserItem({QStringLiteral("Utility"), QStringLiteral("utility"), ItemKind::Device}).key(),
                 QStringLiteral("device:utility"));
    }

    void usesDecayAndPersist() {
        Library library(path(), clock());
        library.recordUse({QStringLiteral("audio:a")});
        library.recordUse({QStringLiteral("audio:a")});
        QCOMPARE(library.uses(QStringLiteral("audio:a")), 2);
        QCOMPARE(library.rank(QStringLiteral("audio:a")), 2.0);
        now_ += Library::kHalfLifeDays * kDay;
        QVERIFY(std::abs(library.rank(QStringLiteral("audio:a")) - 1.0) < 1e-9);
        library.recordUse({QStringLiteral("audio:a")});
        QVERIFY(std::abs(library.rank(QStringLiteral("audio:a")) - 2.0) < 1e-9);
        QCOMPARE(library.rank(QStringLiteral("audio:never")), 0.0);
        const Library again(path(), clock());
        QCOMPARE(again.uses(QStringLiteral("audio:a")), 3);
        QVERIFY(std::abs(again.rank(QStringLiteral("audio:a")) - 2.0) < 1e-9);
        // The same key twice at once: two uses.
        library.recordUse({QStringLiteral("device:x"), QStringLiteral("device:x")});
        QCOMPARE(library.uses(QStringLiteral("device:x")), 2);
        QCOMPARE(library.rank(QStringLiteral("device:x")), 2.0);
    }

    void unknownFieldsAreKept() {
        write(R"({"version": 1, "items": {"audio:a": {"hidden": true}}})");
        Library library(path(), clock());
        QCOMPARE(library.rank(QStringLiteral("audio:a")), 0.0);  // a record without uses
        library.recordUse({QStringLiteral("audio:a")});
        const QJsonObject items = read().value(QStringLiteral("items")).toObject();
        QCOMPARE(items.value(QStringLiteral("audio:a")).toObject().value(QStringLiteral("hidden")).toBool(), true);
        QCOMPARE(items.value(QStringLiteral("audio:a")).toObject().value(QStringLiteral("uses")).toInt(), 1);
        QCOMPARE(read().value(QStringLiteral("version")).toInt(), 1);
    }

    void badFileIsIgnored() {
        write("{not json");
        QVERIFY(Library(path(), clock()).records().isEmpty());
        write(R"({"version": 2, "items": {"audio:a": {"uses": 1}}})");
        QVERIFY(Library(path(), clock()).records().isEmpty());
        write(R"([{"version": 1}])");  // (not an object)
        QVERIFY(Library(path(), clock()).records().isEmpty());
        write(R"({"version": 1, "items": [{"uses": 1}]})");
        QVERIFY(Library(path(), clock()).records().isEmpty());
        write(R"({"version": 1, "items": {"audio:a": {"uses": 1}, "audio:b": 3}})");
        QCOMPARE(Library(path(), clock()).records().keys(), QStringList{QStringLiteral("audio:a")});
        // The environment says where it is.
        qputenv("SUBSTATION_LIBRARY", path().toUtf8());
        QCOMPARE(Library::defaultPath(), path());
        qunsetenv("SUBSTATION_LIBRARY");
    }

    void savingMakesItsFolder() {
        const QString nested = tmp_->filePath(QStringLiteral("a/b/library.json"));
        Library library(nested, clock());
        library.recordUse({QStringLiteral("audio:a")});
        QCOMPARE(Library(nested, clock()).uses(QStringLiteral("audio:a")), 1);
    }

    void usageRecordsForTheBackend() {
        write(R"({"version": 1, "items": {
            "a": {"uses": 2, "score": 1.5, "last_used": 1000},
            "b": {"hidden": true},
            "c": {"score": 2},
            "d": {"score": "x"},
            "e": {"score": 1, "last_used": null}}})");
        const auto records = usageRecords(Library(path(), clock()));
        std::map<std::string, std::pair<double, double>> got;
        for (const auto& r : records) got[r.key] = {r.score, r.lastUsed};
        QCOMPARE(got.size(), size_t(4));  // "d": not a number, left out
        QCOMPARE(got.at("a").first, 1.5);
        QCOMPARE(got.at("a").second, 1000.0);
        QCOMPARE(got.at("b").first, 0.0);
        QVERIFY(std::isnan(got.at("b").second));
        QCOMPARE(got.at("c").first, 2.0);
        QVERIFY(std::isnan(got.at("c").second));
        QVERIFY(std::isnan(got.at("e").second));
    }

    void matchQualityPrefersNameStarts() {
        const QString query = QStringLiteral("kick");
        QVERIFY(matchQuality(audio(QStringLiteral("kick.wav")), query) > matchQuality(audio(QStringLiteral("Kick 01.wav")), query));
        QVERIFY(matchQuality(audio(QStringLiteral("Kick 01.wav")), query) > matchQuality(audio(QStringLiteral("Big_Kick.wav")), query));
        QVERIFY(matchQuality(audio(QStringLiteral("Big_Kick.wav")), query) > matchQuality(audio(QStringLiteral("Bigkick.wav")), query));
        QVERIFY(matchQuality(audio(QStringLiteral("Bigkick.wav")), query) >
                matchQuality(audio(QStringLiteral("Snare.wav"), QStringLiteral("Kicks")), query));
    }

    void rankPutsUsedItemsFirst() {
        Library library(path(), clock());
        const std::vector<BrowserItem> items{audio(QStringLiteral("Kick A.wav")), audio(QStringLiteral("Big Kick.wav")),
                                             audio(QStringLiteral("Kick B.wav")), audio(QStringLiteral("Snare.wav"))};
        QCOMPARE(names(find(items, QStringLiteral("kick"), library)),
                 QStringList({QStringLiteral("Kick A.wav"), QStringLiteral("Kick B.wav"), QStringLiteral("Big Kick.wav")}));
        library.recordUse({items[2].key()});
        now_ += kDay;
        library.recordUse({items[1].key(), items[1].key()});
        QCOMPARE(names(find(items, QStringLiteral("kick"), library)),
                 QStringList({QStringLiteral("Big Kick.wav"), QStringLiteral("Kick B.wav"), QStringLiteral("Kick A.wav")}));
        // Without a search, only the used items move.
        QCOMPARE(names(find(items, QString(), library)),
                 QStringList({QStringLiteral("Big Kick.wav"), QStringLiteral("Kick B.wav"), QStringLiteral("Kick A.wav"),
                              QStringLiteral("Snare.wav")}));
        QCOMPARE(names(find(items, QStringLiteral("kick"), library, QStringLiteral("name"))),
                 QStringList({QStringLiteral("Big Kick.wav"), QStringLiteral("Kick A.wav"), QStringLiteral("Kick B.wav")}));
    }

    void whatEachSidebarEntryLists() {
        auto query = [](const QStringList& scope) { return scopeQuery(Scope::fromList(scope)); };
        QCOMPARE(query({QStringLiteral("all")}).groups, (std::vector<int>{kBuiltinGroup, kPluginsGroup, kPresetsGroup, kAudioGroup}));
        QCOMPARE(query({QStringLiteral("samples")}).groups, std::vector<int>{kAudioGroup});
        QCOMPARE(query({QStringLiteral("builtin"), QStringLiteral("Instruments")}).groups, std::vector<int>{kBuiltinGroup});
        QCOMPARE(query({QStringLiteral("builtin"), QStringLiteral("Instruments")}).tag, QStringLiteral("Instruments"));
        QCOMPARE(query({QStringLiteral("plugins")}).groups, std::vector<int>{kPluginsGroup});
        QCOMPARE(query({QStringLiteral("plugins")}).tag, QString());
        QCOMPARE(query({QStringLiteral("presets"), QStringLiteral("EQ Eight")}).groups, std::vector<int>{kPresetsGroup});
        QCOMPARE(query({QStringLiteral("presets"), QStringLiteral("EQ Eight")}).tag, QStringLiteral("EQ Eight"));
        const ScopeQuery place = query({QStringLiteral("place"), QStringLiteral("/Music/Drums/")});
        QCOMPARE(place.groups, std::vector<int>{kAudioGroup});
#ifdef _WIN32
        QCOMPARE(place.placePrefix, std::string("\\music\\drums\\"));
        QCOMPARE(placePrefix(QStringLiteral("C:/")), std::string("c:\\"));
#else
        QCOMPARE(place.placePrefix, std::string("/Music/Drums/"));  // case kept here
        QCOMPARE(placePrefix(QStringLiteral("/")), std::string("/"));
#endif
        QCOMPARE(query({QStringLiteral("add")}).groups, std::vector<int>{kAudioGroup});
        QCOMPARE(Scope::fromList({QStringLiteral("place"), QStringLiteral("/x")}).toList(),
                 QStringList({QStringLiteral("place"), QStringLiteral("/x")}));
        QCOMPARE(Scope::fromList({QStringLiteral("all")}).toList(), QStringList{QStringLiteral("all")});
        QCOMPARE(Scope::fromList({}).kind, QStringLiteral("samples"));

        PluginInfo synth;
        synth.instrument = true;
        QCOMPARE(pluginTag(pluginItem(synth)), QStringLiteral("Instruments"));
        QCOMPARE(pluginTag(pluginItem(PluginInfo())), QStringLiteral("Audio Effects"));
        QCOMPARE(pluginTag(audio(QStringLiteral("x.wav"))), QStringLiteral("Audio Effects"));

        QCOMPARE(sortOrders().size(), 2);
        QCOMPARE(sortOrders()[0].toMap().value(QStringLiteral("value")).toString(), QStringLiteral("rank"));
        QCOMPARE(sortOrders()[1].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Name"));
        QVERIFY(isSortOrder(QStringLiteral("name")) && !isSortOrder(QStringLiteral("size")));
    }

    void builtinDevicesAreItems() {
        // As the engine lists them: by category (instruments first), named, with their category as detail.
        const std::vector<BrowserItem> all = builtinItems();
        QStringList categories = builtinCategoryNames();
        size_t total = 0;
        for (const QString& category : categories) {
            const auto items = builtinItems(category);
            total += items.size();
            for (const BrowserItem& item : items) {
                QCOMPARE(item.kind, ItemKind::Device);
                QCOMPARE(item.detail, category);
                QVERIFY(!item.path.isEmpty() && !item.name.isEmpty());
                QCOMPARE(item.key(), QStringLiteral("device:") + item.path);
            }
        }
        QCOMPARE(total, all.size());
        if (categories.size() == 2) QCOMPARE(categories, QStringList({QStringLiteral("Instruments"), QStringLiteral("Audio Effects")}));
    }

    void audioFiles() {
        QVERIFY(FileIndex::isAudioFile(QStringLiteral("/x/Kick.WAV")));
        QVERIFY(FileIndex::isAudioFile(QStringLiteral("/x/a.wave")));
        QVERIFY(FileIndex::isAudioFile(QStringLiteral("/x/a.flac")));
        QVERIFY(FileIndex::isAudioFile(QStringLiteral("/x/a.mp3")));
        QVERIFY(!FileIndex::isAudioFile(QStringLiteral("/x/a.wav.asd")));
        qputenv("SUBSTATION_BROWSER_INDEX", path().toUtf8());
        QCOMPARE(FileIndex::defaultIndexPath(), path());
        qunsetenv("SUBSTATION_BROWSER_INDEX");
        QVERIFY(FileIndex::defaultIndexPath().endsWith(QStringLiteral("/SUBstation/browser-index.bin")));
    }

private:
    QString path() const { return tmp_->filePath(QStringLiteral("library.json")); }
    Library::Clock clock() { return [this] { return now_; }; }
    void write(const QByteArray& data) {
        QFile file(path());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(data);
    }
    QJsonObject read() const {
        QFile file(path());
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    std::unique_ptr<QTemporaryDir> tmp_;
    double now_ = 0.0;
};

QTEST_GUILESS_MAIN(TestBrowserSearch)
#include "test_browser_search.moc"
