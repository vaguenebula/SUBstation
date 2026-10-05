// The browser's backend (browser/src, sub_browser): that it indexes, matches
// and orders exactly as the Python code before it did (the reference in
// support/BrowserReference.h), keeps its index up to date incrementally, and
// runs its searches off the caller's thread, latest first.
//
// Python's own text rules (str.lower, casefold, split, the regex's word starts,
// the match quality) are checked against values Python 3.12 (Unicode 15.0.0,
// as the tables) computed over every character and over every string of a few
// characters from pools of awkward ones: FNV-1a hashes of the results in order.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "Browser.h"
#include "BrowserReference.h"
#include "Platform.h"
#include "Search.h"
#include "Text.h"
#include "browser/BrowserSearch.h"
#include "browser/FileIndex.h"
#include "browser/ItemListModel.h"
#include "browser/Library.h"
#include "browser/PathKeys.h"

using namespace sub::app;
namespace backend = sub::browser;

namespace {

// The reference: the browser's index and search as they were in Python (support/BrowserReference.h).
namespace reference = sub::app::test::reference;
using reference::fsPath;
using reference::lower;

constexpr double kDay = 86400.0;
constexpr uint32_t kMaxFiles = 300000;
constexpr uint32_t kMaxDepth = 16;

struct Fnv {
    uint64_t hash = 0xcbf29ce484222325ull;
    void add(std::string_view data) {
        for (const char c : data) hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    }
};

// One code point as WTF-8 (surrogates too, as Python's surrogatepass).
std::string utf8(uint32_t c) {
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

// Every string of `minLength` to `maxLength` items of the pool, in the order
// itertools.product makes them (the last position changing fastest).
void eachProduct(const std::vector<uint32_t>& pool, int minLength, int maxLength,
                 const std::function<void(const std::string&)>& visit) {
    for (int length = minLength; length <= maxLength; ++length) {
        std::vector<size_t> at(static_cast<size_t>(length), 0);
        for (;;) {
            std::string s;
            for (const size_t i : at) s += utf8(pool[i]);
            visit(s);
            int position = length - 1;
            while (position >= 0 && ++at[static_cast<size_t>(position)] == pool.size()) at[static_cast<size_t>(position--)] = 0;
            if (position < 0) break;
        }
    }
}

void touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
}

QStringList describe(const std::vector<BrowserItem>& items) {
    QStringList out;
    for (const BrowserItem& item : items)
        out << QStringLiteral("%1 | %2 | %3 | %4").arg(item.name, item.path, kindName(item.kind), item.detail);
    return out;
}

QStringList names(const std::vector<BrowserItem>& items) {
    QStringList out;
    for (const BrowserItem& item : items) out << item.name;
    return out;
}

// --- Random things, as the Python tests made them ------------------------------------------

class Random {
public:
    explicit Random(unsigned seed) : engine_(seed) {}
    double real() { return std::uniform_real_distribution<double>(0.0, 1.0)(engine_); }
    int between(int low, int high) { return std::uniform_int_distribution<int>(low, high)(engine_); }
    template <typename T>
    const T& choice(const std::vector<T>& from) {
        return from[static_cast<size_t>(between(0, static_cast<int>(from.size()) - 1))];
    }
    template <typename T>
    std::vector<T> sample(const std::vector<T>& from, size_t n) {
        std::vector<T> copy = from;
        std::shuffle(copy.begin(), copy.end(), engine_);
        copy.resize(std::min(n, copy.size()));
        return copy;
    }

private:
    std::mt19937 engine_;
};

const std::vector<QString> kWords{
    QStringLiteral("Kick"), QStringLiteral("kick"),        QStringLiteral("KICK"),         QStringLiteral("Snare"),
    QStringLiteral("808"),  QStringLiteral("Bass"),        QStringLiteral("Big"),          QStringLiteral("Deep"),
    QStringLiteral("Hat"),  QStringLiteral("Open"),        QStringLiteral("Loop"),         QStringLiteral("e"),
    QStringLiteral("\u03a3"), QStringLiteral("\u0391\u03a3"), QStringLiteral("\u0130"), QStringLiteral("Stra\u00dfe"),
    QStringLiteral("Caf\u00e9"), QStringLiteral("\u03a9"),  QStringLiteral("a"),            QStringLiteral("x")};
const std::vector<QString> kSeparators{QStringLiteral(" "), QStringLiteral("_"), QStringLiteral("-"), QStringLiteral("."),
                                       QStringLiteral("("), QStringLiteral(")"), QStringLiteral("["), QStringLiteral("]"),
                                       QString(),           QStringLiteral("  ")};
const std::vector<QString> kQueries{
    QStringLiteral("kick"),   QStringLiteral("Kick"),       QStringLiteral("k"),        QStringLiteral("e"),
    QStringLiteral("808"),    QStringLiteral("bass"),       QStringLiteral("big kick"), QStringLiteral("\u03c3"),
    QStringLiteral("\u03b1\u03c2"), QStringLiteral("i\u0307"), QStringLiteral("stra\u00dfe"), QStringLiteral("caf\u00e9"),
    QStringLiteral("\u03c9"), QStringLiteral("a"),          QStringLiteral("deep_"),    QStringLiteral("("),
    QString(),                QStringLiteral(" "),          QStringLiteral("hat open"), QStringLiteral("zz"),
    QStringLiteral(".wav"),   QStringLiteral("kick kick"),  QStringLiteral("ss")};

QString randomName(Random& rng) {
    QString name = rng.choice(kWords);
    const int parts = rng.between(0, 3);
    for (int i = 0; i < parts; ++i) name += rng.choice(kSeparators) + rng.choice(kWords);
    return name;
}

QString randomQuery(Random& rng) {
    if (rng.real() < 0.7) return rng.choice(kQueries);
    return rng.choice(kQueries) + QLatin1Char(' ') + rng.choice(kQueries);
}

// Folders nested a few deep, many names shared between folders (and in
// different case), files that are not audio, and names the browser skips.
void buildTree(const QString& root, Random& rng, int folders = 25, int files = 30) {
    std::vector<QString> dirs{root};
    QDir().mkpath(root);
    const std::vector<QString> folderNames{QStringLiteral("Kicks"), QStringLiteral("Loops"), QStringLiteral("Pack"),
                                           QStringLiteral("\u03a3\u0391"), QStringLiteral("\u0130"),
                                           QStringLiteral("Stra\u00dfe"), QStringLiteral("Caf\u00e9"),
                                           QStringLiteral("a b"), QStringLiteral("x.y")};
    for (int i = 0; i < folders; ++i) {
        const QString parent = rng.choice(dirs);
        dirs.push_back(parent + QLatin1Char('/') + rng.choice(folderNames) + QStringLiteral(" %1").arg(i));
        QDir().mkpath(dirs.back());
    }
    const std::vector<QString> extensions{QStringLiteral(".wav"),  QStringLiteral(".WAV"), QStringLiteral(".mp3"),
                                          QStringLiteral(".flac"), QStringLiteral(".wave"), QStringLiteral(".txt"),
                                          QStringLiteral(".asd"),  QStringLiteral(".wav.asd"), QString()};
    for (const QString& folder : dirs) {
        QSet<QString> used;
        const int count = rng.between(0, files);
        for (int i = 0; i < count; ++i) {
            const QString name = randomName(rng) + rng.choice(extensions);
            const QString key = QString::fromStdString(lower(name));
            const bool trimmed = !name.startsWith(QLatin1Char('.')) && !name.startsWith(QLatin1Char(' ')) &&
                                 !name.endsWith(QLatin1Char('.')) && !name.endsWith(QLatin1Char(' '));
            if (used.contains(key) || !trimmed) continue;
            used.insert(key);
            touch(folder + QLatin1Char('/') + name);
        }
    }
    touch(root + QStringLiteral("/.hidden.wav"));
    touch(root + QStringLiteral("/$recycled.wav"));
    touch(root + QStringLiteral("/.git/inside.wav"));
    touch(root + QStringLiteral("/$RECYCLE.BIN/gone.wav"));
}

// --- The backend, driven synchronously ---------------------------------------------------

backend::Limits limits(uint32_t maxFiles = kMaxFiles, uint32_t maxDepth = kMaxDepth) {
    backend::Limits l;
    l.maxFiles = maxFiles;
    l.maxDepth = maxDepth;
    for (const QString& ext : FileIndex::audioExtensions()) l.extensions.push_back(ext.toStdString());
    return l;
}

class Backend {
public:
    explicit Backend(const QStringList& places, const QString& store = {}, uint32_t maxFiles = kMaxFiles,
                     uint32_t maxDepth = kMaxDepth)
        : native(store.isEmpty() ? std::string() : toBackendPath(store), limits(maxFiles, maxDepth)) {
        if (!places.isEmpty()) setPlaces(places);
    }

    void setPlaces(const QStringList& places) {
        std::vector<backend::PlaceSpec> specs;
        for (const QString& place : places) specs.push_back(placeSpec(place));
        native.setPlaces(std::move(specs));
    }
    bool wait() { return native.waitIdle(60); }

    // The latest search's result (null if it didn't come, or wasn't this one).
    std::shared_ptr<const backend::Result> search(const QString& text = {}, const QString& sort = QStringLiteral("rank"),
                                                  double now = 0.0, std::vector<int> groups = {kAudioGroup},
                                                  const QString& tag = {}, const std::string& prefix = {}) {
        backend::Query query;
        query.text = text.toStdString();
        query.sort = sort == QStringLiteral("name") ? backend::Sort::Name : backend::Sort::Rank;
        query.now = now;
        query.groups = std::move(groups);
        query.tag = tag.toStdString();
        query.placePrefix = prefix;
        const uint64_t generation = native.search(std::move(query));
        if (!wait()) return nullptr;
        auto result = native.take().result;
        return result && result->generation == generation ? result : nullptr;
    }

    std::vector<BrowserItem> items(const QString& text = {}, const QString& sort = QStringLiteral("rank"), double now = 0.0,
                                   const std::string& prefix = {}) {
        const SearchResult result(search(text, sort, now, {kAudioGroup}, {}, prefix), {});
        return result.items(0, result.total());
    }

    backend::Browser native;
};

std::vector<std::string> keysOf(const backend::Result& result) {
    std::vector<std::string> keys;
    for (const backend::Hit& hit : result.hits) keys.push_back(result.groups.at(static_cast<int>(hit.group))->items[hit.index].key);
    return keys;
}

bool waitFor(const std::function<bool()>& predicate, double seconds = 10.0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

std::vector<BrowserItem> randomItems(Random& rng, int n) {
    const std::vector<QString> details{QStringLiteral("Drums"),       QStringLiteral("Vendor"), QStringLiteral("Audio Effects"),
                                       QStringLiteral("Instruments"), QStringLiteral("Kicks"),  QStringLiteral("\u03a3\u0391"),
                                       QString()};
    const std::vector<QString> extensions{QStringLiteral(".wav"), QStringLiteral(".mp3"), QStringLiteral(".WAV")};
    std::vector<BrowserItem> items;
    for (int i = 0; i < n; ++i) {
        QString name = randomName(rng);
        const int kind = rng.between(0, 2);
        const QString detail = rng.choice(details);
        if (kind == 0) {
            name += rng.choice(extensions);
            items.push_back({name, QStringLiteral("/Lib/%1/%2/%3").arg(detail).arg(i).arg(name), ItemKind::Audio, detail,
                             std::nullopt, {}});
        } else if (kind == 1) {
            PluginInfo plugin;
            plugin.format = QStringLiteral("VST3");
            plugin.uid = QStringLiteral("uid%1").arg(i);
            plugin.name = name;
            plugin.vendor = detail;
            plugin.path = QStringLiteral("/P/%1.vst3").arg(i);
            plugin.instrument = rng.real() < 0.5;
            items.push_back({name, plugin.path, ItemKind::Plugin, detail, plugin, {}});
        } else {
            items.push_back({name, QStringLiteral("dev%1").arg(i), ItemKind::Device, detail, std::nullopt, {}});
        }
    }
    return items;
}

QString tagOf(const BrowserItem& item) { return item.detail.left(3); }

}  // namespace

class TestBrowserNative : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init() {
        tmp_ = std::make_unique<QTemporaryDir>();
        QVERIFY(tmp_->isValid());
    }
    void cleanup() { tmp_.reset(); }

    // --- Text: Python's own rules ---

    void lowerAndCasefoldOfEveryCharacter() {
        if (std::string_view(backend::unicodeVersion()) != "15.0.0") QSKIP("the tables were made with another Unicode version");
        Fnv lowered, folded;
        for (uint32_t c = 0; c < 0x110000; ++c) {
            const std::string s = utf8(c);
            lowered.add(backend::pyLower(s));
            lowered.add("\xff");
            folded.add(backend::pyCasefold(s));
            folded.add("\xff");
        }
        QCOMPARE(lowered.hash, 0x806E0A816BADEB82ull);
        QCOMPARE(folded.hash, 0xC0B542B380906D48ull);
        // A few, by name.
        QCOMPARE(backend::pyLower(utf8(0x130)), utf8('i') + utf8(0x307));  // İ
        QCOMPARE(backend::pyCasefold(utf8(0xDF)), std::string("ss"));      // ß
        QCOMPARE(backend::pyLower(utf8(0x391) + utf8(0x3A3)), utf8(0x3B1) + utf8(0x3C2));  // ΑΣ: final sigma
        QCOMPARE(backend::pyCasefold(utf8(0x391) + utf8(0x3A3)), utf8(0x3B1) + utf8(0x3C3));
    }

    void lowerFollowsFinalSigma() {
        if (std::string_view(backend::unicodeVersion()) != "15.0.0") QSKIP("the tables were made with another Unicode version");
        const std::vector<uint32_t> pool{0x3A3, 0x3C3, 0x3C2, 0x391, 'a', '\'', '.', 0x301, 0xAD,
                                         ' ',   '1',   0x130, 0x1C5, '_', 0x3A9, 0x345, 0x2B0};
        Fnv hash;
        int count = 0;
        eachProduct(pool, 1, 4, [&](const std::string& s) {
            hash.add(backend::pyLower(s));
            hash.add("\xff");
            ++count;
        });
        QCOMPARE(count, 88740);
        QCOMPARE(hash.hash, 0x3632B8ECEC2BC627ull);
    }

    void splitAndWordStarts() {
        if (std::string_view(backend::unicodeVersion()) != "15.0.0") QSKIP("the tables were made with another Unicode version");
        const std::vector<uint32_t> pool{'a', 'B', '7', '_', '-', '.', '(', ')', '[', ']', ' ', '\t', 0xA0, 0x2003, 0x1C,
                                         0xE9, 0xDF, 0x301, 0x663, 0xB7, '!', 0x6F22, 0x1F600};
        Fnv split, starts;
        int count = 0;
        std::vector<uint32_t> offsets;
        eachProduct(pool, 0, 4, [&](const std::string& s) {
            for (const std::string& part : backend::pySplit(s)) {
                split.add(part);
                split.add(std::string_view("\0", 1));
            }
            split.add("\xfe");
            backend::wordStarts(s, offsets);
            for (const uint32_t offset : offsets) {  // as code point indexes
                size_t index = 0;
                for (size_t b = 0; b < offset; ++b)
                    if ((static_cast<unsigned char>(s[b]) & 0xC0) != 0x80) ++index;
                starts.add(std::to_string(index) + ",");
            }
            starts.add("\xfd");
            ++count;
        });
        QCOMPARE(count, 292561);
        QCOMPARE(split.hash, 0x874AFF4904BDAEE9ull);
        QCOMPARE(starts.hash, 0xE97CA9EB61BC8C90ull);
    }

    void keysAreNormcase() {
#ifdef _WIN32
        // os.path.normcase: backslashes and Windows' own lower case (no final
        // sigma, no ß expanded).
        QCOMPARE(QString::fromStdString(backend::platform::pathKey("C:\\Samples\\Kick.WAV")), QStringLiteral("c:\\samples\\kick.wav"));
        QCOMPARE(QString::fromStdString(backend::platform::pathKey(QStringLiteral("D:/\u00c0\u03a3/X.wav").toStdString())),
                 QStringLiteral("d:\\\u00e0\u03c3\\x.wav"));
        QCOMPARE(QString::fromStdString(backend::platform::pathKey(QStringLiteral("E:\\Stra\u00dfe\\\u0391\u03a3.flac").toStdString())),
                 QStringLiteral("e:\\stra\u00dfe\\\u03b1\u03c3.flac"));
        QCOMPARE(audioKey(QStringLiteral("C:/x/y/../Kick.WAV")), QStringLiteral("audio:c:\\x\\kick.wav"));
#else
        // Elsewhere names keep their case: "Kick.wav" and "kick.wav" are two files.
        QCOMPARE(backend::platform::pathKey("/Samples/Kick.WAV"), std::string("/Samples/Kick.WAV"));
        QCOMPARE(backend::platform::nameKey("Kick.WAV"), std::string("Kick.WAV"));
        QCOMPARE(audioKey(QStringLiteral("/x/y/../Kick.WAV")), QStringLiteral("audio:/x/Kick.WAV"));
        QVERIFY(audioKey(QStringLiteral("/x/Kick.wav")) != audioKey(QStringLiteral("/x/kick.wav")));
#endif
    }

    void matchQualityAsPython() {
        if (std::string_view(backend::unicodeVersion()) != "15.0.0") QSKIP("the tables were made with another Unicode version");
        const std::vector<QString> extensions{QStringLiteral(".wav"), QStringLiteral(".WAV"), QString(), QStringLiteral(".flac")};
        std::vector<QString> nameList;
        int counter = 0;
        for (const QString& first : kWords) {
            nameList.push_back(first + extensions[static_cast<size_t>(counter++ % 4)]);
            for (const QString& separator : kSeparators)
                for (const QString& second : kWords)
                    nameList.push_back(first + separator + second + extensions[static_cast<size_t>(counter++ % 4)]);
        }
        QCOMPARE(nameList.size(), size_t(4020));
        Fnv hash;
        int count = 0;
        long total = 0;
        std::vector<uint32_t> scratch;
        for (const QString& name : nameList) {
            const std::string lowered = lower(name);
            for (const QString& query : kQueries) {
                const auto terms = backend::pySplit(lower(query));
                if (terms.empty()) continue;
                std::string joined;
                for (const auto& t : terms) joined += (joined.empty() ? "" : " ") + t;
                for (const bool audio : {true, false}) {
                    const int quality = backend::matchQuality(lowered, audio, terms, joined, scratch);
                    hash.add(std::to_string(quality) + ",");
                    total += quality;
                    ++count;
                }
            }
        }
        QCOMPARE(count, 168840);
        QCOMPARE(total, 73606L);
        QCOMPARE(hash.hash, 0x6CDF196752F2FCBEull);
        // And the reference here agrees.
        Random rng(3);
        for (int i = 0; i < 2000; ++i) {
            const QString name = randomName(rng) + rng.choice(extensions);
            const QString query = randomQuery(rng);
            const auto terms = reference::termsOf(query);
            if (terms.empty()) continue;
            std::string joined;
            for (const auto& t : terms) joined += (joined.empty() ? "" : " ") + t;
            for (const ItemKind kind : {ItemKind::Audio, ItemKind::Plugin}) {
                const BrowserItem item{name, QStringLiteral("/x/") + name, kind, QStringLiteral("Folder"), std::nullopt, {}};
                QCOMPARE(backend::matchQuality(lower(name), kind == ItemKind::Audio, terms, joined, scratch),
                         reference::matchQuality(item, terms));
            }
        }
    }

    // --- Searching: as the Python find() did ---

    void searchOrdersAsPython_data() {
        QTest::addColumn<unsigned>("seed");
        for (unsigned seed = 0; seed < 6; ++seed) QTest::addRow("seed %u", seed) << seed;
    }

    void searchOrdersAsPython() {
        QFETCH(unsigned, seed);
        Random rng(seed);
        double clock = 1'000'000'000.0;
        Library library(tmp_->filePath(QStringLiteral("library-%1.json").arg(seed)), [&] { return clock; });
        const std::vector<BrowserItem> items = randomItems(rng, 400);
        Backend b({});
        std::vector<backend::ExternalItem> external;
        for (const BrowserItem& item : items) {
            backend::ExternalItem e;
            e.kind = static_cast<backend::Kind>(item.kind);
            e.name = item.name.toStdString();
            e.path = item.path.toStdString();
            e.detail = item.detail.toStdString();
            e.key = item.key().toStdString();
            e.tag = tagOf(item).toStdString();
            external.push_back(std::move(e));
        }
        b.native.setExternal(5, std::move(external));
        const std::vector<QString> tags{QString(), QString(), QStringLiteral("Dru"), QStringLiteral("Ven"), QStringLiteral("Aud")};
        for (int round = 0; round < 60; ++round) {
            if (rng.real() < 0.3) {  // some uses, at various times; some with odd records
                clock += std::vector<double>{0.0, 3600.0, kDay * 40}[static_cast<size_t>(rng.between(0, 2))];
                QStringList keys;
                const int uses = rng.between(1, 4);
                for (int i = 0; i < uses; ++i) keys << rng.choice(items).key();
                library.recordUse(keys);
                if (rng.real() < 0.2) library.setRecord(rng.choice(items).key(), {{QStringLiteral("hidden"), true}});
            }
            b.native.setUsage(usageRecords(library), Library::kHalfLifeDays);
            const QString query = randomQuery(rng);
            const QString sort = rng.real() < 0.5 ? QStringLiteral("rank") : QStringLiteral("name");
            const QString tag = rng.choice(tags);
            std::vector<BrowserItem> tagged;
            for (const BrowserItem& item : items)
                if (tag.isEmpty() || tagOf(item) == tag) tagged.push_back(item);
            const auto expected = reference::find(tagged, query, library, sort);
            const auto result = b.search(query, sort, library.now(), {5}, tag);
            QVERIFY(result);
            std::vector<std::string> keys;
            for (const BrowserItem& item : expected) keys.push_back(item.key().toStdString());
            QVERIFY2(keysOf(*result) == keys, qPrintable(QStringLiteral("round %1, \"%2\", %3, %4").arg(round).arg(query, sort, tag)));
        }
    }

    // --- Indexing: as the Python walk did ---

    void indexListsAsPython() {
        Random rng(7);
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        buildTree(lib, rng);
        Backend b({lib});
        QVERIFY(b.wait());
        const auto expected = reference::indexPlaces({lib});
        QVERIFY(expected.size() > 100);
        QCOMPARE(describe(b.items()), describe(expected));  // same files, details and paths, in the same order
        QCOMPARE(b.native.status().files, uint32_t(expected.size()));
    }

    void indexLimitsAsPython() {
        Random rng(8);
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        buildTree(lib, rng, 40, 12);
        QString deep = lib;
        for (int i = 0; i < 8; ++i) {  // deeper than the limits below
            deep += QStringLiteral("/level%1").arg(i);
            touch(deep + QStringLiteral("/deep%1.wav").arg(i));
        }
        for (const auto& [maxFiles, maxDepth] : std::vector<std::pair<uint32_t, uint32_t>>{{50, 16}, {1000000, 3}, {7, 2}, {1, 16}}) {
            Backend b({lib}, {}, maxFiles, maxDepth);
            QVERIFY(b.wait());
            QCOMPARE(describe(b.items()), describe(reference::indexPlaces({lib}, maxFiles, maxDepth)));
        }
    }

// (Q_OS_WIN, not _WIN32: moc doesn't see the compiler's predefined macros.)
#ifdef Q_OS_WIN
    void indexWalksJunctionsNotSymlinks() {
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        const QString target = tmp_->filePath(QStringLiteral("elsewhere"));
        touch(target + QStringLiteral("/Linked.wav"));
        touch(lib + QStringLiteral("/Real.wav"));
        auto mklink = [](const QStringList& args) {
            return QProcess::execute(QStringLiteral("cmd"), QStringList{QStringLiteral("/c"), QStringLiteral("mklink")} + args);
        };
        QCOMPARE(mklink({QStringLiteral("/J"), QDir::toNativeSeparators(lib + QStringLiteral("/junction")), QDir::toNativeSeparators(target)}), 0);
        const int made = mklink({QStringLiteral("/D"), QDir::toNativeSeparators(lib + QStringLiteral("/dirlink")),
                                 QDir::toNativeSeparators(target)});  // needs developer mode
        mklink({QDir::toNativeSeparators(lib + QStringLiteral("/filelink.wav")), QDir::toNativeSeparators(lib + QStringLiteral("/Real.wav"))});
        Backend b({lib});
        QVERIFY(b.wait());
        const QStringList found = names(b.items());
        QVERIFY(found.contains(QStringLiteral("Linked.wav")));  // through the junction
        if (made == 0) QCOMPARE(found.count(QStringLiteral("Linked.wav")), 1);  // not through the symbolic link
        QCOMPARE(describe(b.items()), describe(reference::indexPlaces({lib})));
    }
#else
    void indexDoesNotWalkSymlinks() {
        // On POSIX a folder to walk into is a real directory: links to folders
        // are not followed (as DirEntry.is_dir(follow_symlinks=False)); a link
        // to a file is listed as a file.
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        const QString target = tmp_->filePath(QStringLiteral("elsewhere"));
        touch(target + QStringLiteral("/Linked.wav"));
        touch(lib + QStringLiteral("/Real.wav"));
        QVERIFY(QFile::link(target, lib + QStringLiteral("/dirlink")));
        QVERIFY(QFile::link(lib + QStringLiteral("/Real.wav"), lib + QStringLiteral("/filelink.wav")));
        Backend b({lib});
        QVERIFY(b.wait());
        QCOMPARE(names(b.items()), QStringList({QStringLiteral("filelink.wav"), QStringLiteral("Real.wav")}));
        QCOMPARE(describe(b.items()), describe(reference::indexPlaces({lib})));
    }
#endif

    void placesOverlappingOrMissing() {
        Random rng(9);
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        buildTree(lib, rng);
        QStringList folders = QDir(lib).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        folders.removeIf([](const QString& name) { return reference::hiddenName(name); });
        QVERIFY(!folders.isEmpty());
        const QString inner = lib + QLatin1Char('/') + folders.first();
        const QString missing = tmp_->filePath(QStringLiteral("missing"));
        for (const QStringList& places : {QStringList{lib, inner}, QStringList{inner, lib}, QStringList{missing, lib}, QStringList{inner}}) {
            Backend b(places);
            QVERIFY(b.wait());
            QCOMPARE(describe(b.items()), describe(reference::indexPlaces(places)));
        }
    }

    void placeAndAllScopes() {
        Random rng(10);
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        buildTree(lib, rng);
        Backend b({lib});
        QVERIFY(b.wait());
        const auto items = reference::indexPlaces({lib});
        Library library(tmp_->filePath(QStringLiteral("library.json")), [] { return 1e9; });
        QStringList used;
        for (const BrowserItem& item : rng.sample(items, 10)) used << item.key();
        library.recordUse(used);
        b.native.setUsage(usageRecords(library), Library::kHalfLifeDays);
        QSet<QString> folderSet;
        for (const BrowserItem& item : items) folderSet.insert(QFileInfo(item.path).path());
        std::vector<QString> folders(folderSet.begin(), folderSet.end());
        std::sort(folders.begin(), folders.end());
        for (const QString& query : {QString(), QStringLiteral("kick"), QStringLiteral("e"), QStringLiteral("808 bass"),
                                     QStringLiteral("\u03c3"), QStringLiteral("stra\u00dfe"), QStringLiteral("zz")}) {
            for (const QString& sort : {QStringLiteral("rank"), QStringLiteral("name")}) {
                QCOMPARE(describe(b.items(query, sort, 1e9)), describe(reference::find(items, query, library, sort)));
                const QString place = rng.choice(folders);
                QCOMPARE(describe(b.items(query, sort, 1e9, placePrefix(place))),
                         describe(reference::find(reference::placeItems(items, place), query, library, sort)));
            }
        }
        // The used files rank first (their keys found through their folders').
        const auto ranked = b.items(QString(), QStringLiteral("rank"), 1e9);
        QSet<QString> first;
        for (size_t i = 0; i < 10 && i < ranked.size(); ++i) first.insert(ranked[i].key());
        QCOMPARE(first, QSet<QString>(used.begin(), used.end()));
    }

    // --- Keeping up to date ---

    void savedIndexIsCheckedNotListedAgain() {
        Random rng(11);
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        buildTree(lib, rng);
        const QString store = tmp_->filePath(QStringLiteral("state/index.bin"));
        std::vector<BrowserItem> before;
        {
            Backend first({lib}, store);
            QVERIFY(first.wait());
            before = first.items();
        }
        QVERIFY(QFileInfo::exists(store));

        {  // Nothing changed: what was saved is shown, and nothing is published again.
            Backend again({lib}, store);
            QVERIFY(again.wait());
            QCOMPARE(describe(again.items()), describe(before));
            QCOMPARE(again.native.status().version, uint64_t(1));
        }

        // Changes while closed: files added and removed, a folder renamed, one made.
        QStringList folders;
        for (QDirIterator it(lib, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories); it.hasNext();) {
            const QString folder = it.next();
            if (!reference::hiddenName(QFileInfo(folder).fileName())) folders << folder;
        }
        folders.sort();
        touch(folders.first() + QStringLiteral("/Added Kick.wav"));
        for (QDirIterator it(lib, {QStringLiteral("*.wav")}, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
            const QString file = it.next();
            if (file.contains(QStringLiteral("/.")) || file.contains(QStringLiteral("/$"))) continue;
            QVERIFY(QFile::remove(file));
            break;
        }
        const QString last = folders.last();
        QVERIFY(QDir().rename(last, QFileInfo(last).path() + QStringLiteral("/Renamed Folder")));
        touch(lib + QStringLiteral("/New Pack/Deep/New Snare.wav"));
        Backend changed({lib}, store);
        QVERIFY(changed.wait());
        QCOMPARE(describe(changed.items()), describe(reference::indexPlaces({lib})));
    }

    void rescanListsEveryFolderAgain() {
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        touch(lib + QStringLiteral("/Drums/Kick.wav"));
        const QString store = tmp_->filePath(QStringLiteral("index.bin"));
        {
            Backend first({lib}, store);
            QVERIFY(first.wait());
        }
        // A file the folder's time doesn't tell about (the time is put back).
        const auto drums = fsPath(lib + QStringLiteral("/Drums"));
        const auto time = std::filesystem::last_write_time(drums);
        touch(lib + QStringLiteral("/Drums/Snare.wav"));
        std::filesystem::last_write_time(drums, time);
        Backend b({lib}, store);
        QVERIFY(b.wait());
        QCOMPARE(names(b.items()), QStringList{QStringLiteral("Kick.wav")});  // checked by time only
        b.native.rescan();
        QVERIFY(b.wait());
        QCOMPARE(names(b.items()), QStringList({QStringLiteral("Kick.wav"), QStringLiteral("Snare.wav")}));
    }

    void changesInPlacesAreSeen() {
#if !defined(_WIN32) && !defined(__linux__)
        QSKIP("places are watched on Windows and Linux only");
#endif
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        touch(lib + QStringLiteral("/Drums/Kick.wav"));
        Backend b({lib});
        QVERIFY(b.wait());
        auto current = [&] { return names(b.items()); };

        touch(lib + QStringLiteral("/Drums/Snare.wav"));
        QVERIFY(waitFor([&] { return current() == QStringList({QStringLiteral("Kick.wav"), QStringLiteral("Snare.wav")}); }));
        touch(lib + QStringLiteral("/Pack/Sub/Deeper/Hat.wav"));  // new folders, several deep
        QVERIFY(waitFor([&] { return current().contains(QStringLiteral("Hat.wav")); }));
        QVERIFY(QFile::rename(lib + QStringLiteral("/Drums/Kick.wav"), lib + QStringLiteral("/Drums/Big Kick.wav")));
        QVERIFY(waitFor([&] {
            return current() == QStringList({QStringLiteral("Big Kick.wav"), QStringLiteral("Hat.wav"), QStringLiteral("Snare.wav")});
        }));
        QVERIFY(QDir().rename(lib + QStringLiteral("/Pack"), lib + QStringLiteral("/Moved")));
        QVERIFY(waitFor([&] { return describe(b.items()) == describe(reference::indexPlaces({lib})); }));
        touch(lib + QStringLiteral("/Moved/Sub/Later.wav"));  // in a folder that moved: still watched
        QVERIFY(waitFor([&] { return current().contains(QStringLiteral("Later.wav")); }));
        touch(lib + QStringLiteral("/.git/objects/x.wav"));  // never listed
        QVERIFY(waitFor([&] { return describe(b.items()) == describe(reference::indexPlaces({lib})); }));
    }

    void placesChangeIncrementally() {
        const QString a = tmp_->filePath(QStringLiteral("a"));
        const QString c = tmp_->filePath(QStringLiteral("b"));
        touch(a + QStringLiteral("/One.wav"));
        touch(c + QStringLiteral("/Two.wav"));
        Backend b({a});
        QVERIFY(b.wait());
        b.setPlaces({a, c});
        QVERIFY(b.wait());
        QCOMPARE(describe(b.items()), describe(reference::indexPlaces({a, c})));
        b.setPlaces({c});
        QVERIFY(b.wait());
        QCOMPARE(names(b.items()), QStringList{QStringLiteral("Two.wav")});
    }

    void damagedIndexIsIgnored_data() {
        QTest::addColumn<QString>("damage");
        QTest::newRow("truncate") << QStringLiteral("truncate");
        QTest::newRow("garbage") << QStringLiteral("garbage");
        QTest::newRow("flip") << QStringLiteral("flip");
    }

    void damagedIndexIsIgnored() {
        QFETCH(QString, damage);
        const QString lib = tmp_->filePath(QStringLiteral("lib-") + damage);
        touch(lib + QStringLiteral("/Kick.wav"));
        const QString store = tmp_->filePath(QStringLiteral("index-") + damage + QStringLiteral(".bin"));
        {
            Backend first({lib}, store);
            QVERIFY(first.wait());
        }
        QFile file(store);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray data = file.readAll();
        file.close();
        QVERIFY(!data.isEmpty());
        if (damage == QStringLiteral("truncate")) {
            data.truncate(data.size() / 2);
        } else if (damage == QStringLiteral("garbage")) {
            std::mt19937 engine(4);
            for (char& c : data) c = static_cast<char>(engine() & 0xFF);
        } else {
            data[data.size() / 2] = static_cast<char>(data[data.size() / 2] ^ 0xFF);
        }
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(data);
        file.close();
        Backend b({lib}, store);
        QVERIFY(b.wait());
        QCOMPARE(names(b.items()), QStringList{QStringLiteral("Kick.wav")});
    }

    // --- Searching off the caller's thread ---

    void onlyTheLatestSearchIsHandedOut() {
        const QString lib = bigLibrary();
        Backend b({lib});
        QVERIFY(b.wait());
        std::vector<uint64_t> generations;
        for (const char* q : {"k", "ki", "kic", "kick", "e", "x"}) {
            backend::Query query;
            query.text = q;
            query.groups = {kAudioGroup};
            generations.push_back(b.native.search(std::move(query)));
        }
        QVERIFY(std::is_sorted(generations.begin(), generations.end()));
        QVERIFY(b.wait());
        const auto result = b.native.take().result;
        QVERIFY(result);
        QCOMPARE(result->generation, generations.back());
        QVERIFY(!b.native.take().result);  // handed out once
        // Results of a search replaced before they were taken are dropped.
        backend::Query kick;
        kick.text = "kick";
        kick.groups = {kAudioGroup};
        b.native.search(kick);
        QVERIFY(b.wait());
        backend::Query e;
        e.text = "e";
        e.sort = backend::Sort::Name;
        e.groups = {kAudioGroup};
        const uint64_t latest = b.native.search(e);
        const auto taken = b.native.take().result;
        QVERIFY(!taken || taken->generation == latest);
    }

    void resultsComeInPages() {
        const QString lib = bigLibrary();
        Backend b({lib});
        QVERIFY(b.wait());
        const SearchResult result(b.search(QStringLiteral("e"), QStringLiteral("name")), {});
        QVERIFY(!result.isNull());
        const auto rows = result.items(0, result.total());
        QVERIFY(result.total() > 1000);
        QCOMPARE(int(rows.size()), result.total());
        QCOMPARE(describe(result.items(100, 50)), describe(std::vector<BrowserItem>(rows.begin() + 100, rows.begin() + 150)));
        QCOMPARE(describe(result.items(result.total() - 3, 50)), describe(std::vector<BrowserItem>(rows.end() - 3, rows.end())));
        QVERIFY(result.items(result.total() + 5, 5).empty());

        ItemListModel model;
        QSignalSpy totals(&model, &ItemListModel::totalChanged);
        model.setSource(result);
        QCOMPARE(model.total(), result.total());
        QCOMPARE(totals.count(), 1);
        QCOMPARE(model.rowCount(), ItemListModel::kPage);
        QVERIFY(model.canFetchMore(QModelIndex()));
        model.fetchMore(QModelIndex());
        QCOMPARE(model.rowCount(), 2 * ItemListModel::kPage);
        model.ensureRows(result.total() + 10);
        QCOMPARE(model.rowCount(), result.total());
        QVERIFY(!model.canFetchMore(QModelIndex()));
        const BrowserItem* item = model.item(700);
        QVERIFY(item);
        QCOMPARE(result.find(*item), 700);
    }

    void theApplicationIsWokenFromTheBackendsThreads() {
        // The backend calls its wake callback from its own threads when there
        // is something to take, once until it is taken; FileIndex then takes it
        // on its own thread and says so.
        const QString lib = tmp_->filePath(QStringLiteral("lib"));
        touch(lib + QStringLiteral("/Kick.wav"));
        Backend b({});
        std::atomic<int> wakes{0};
        std::atomic<bool> elsewhere{true};
        const auto main = std::this_thread::get_id();
        b.native.setWakeCallback([&] {
            ++wakes;
            if (std::this_thread::get_id() == main) elsewhere = false;
        });
        b.setPlaces({lib});
        QVERIFY(b.wait());
        QVERIFY(waitFor([&] { return wakes > 0; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        QCOMPARE(wakes.load(), 1);  // not again until taken
        QVERIFY(elsewhere);
        b.native.take();
        backend::Query query;
        query.groups = {kAudioGroup};
        b.native.search(query);
        QVERIFY(waitFor([&] { return wakes == 2; }));
        QVERIFY(b.native.take().result);
        b.native.setWakeCallback(nullptr);

        FileIndex index(nullptr, QString());
        std::vector<SearchResult> results;
        connect(&index, &FileIndex::results, this, [&](const SearchResult& result) { results.push_back(result); });
        QSignalSpy updated(&index, &FileIndex::updated);
        index.setPlaces({lib});
        QVERIFY(index.waitIdle());
        index.search(QString(), QStringLiteral("rank"), 0.0, {kAudioGroup});
        QTRY_VERIFY_WITH_TIMEOUT(!results.empty(), 10000);
        const SearchResult& result = results.back();
        QCOMPARE(names(result.items(0, result.total())), QStringList{QStringLiteral("Kick.wav")});
        QVERIFY(updated.count() > 0);
        QVERIFY(!index.indexing());
        QCOMPARE(index.fileCount(), 1);
    }

private:
    QString bigLibrary() {
        const QString lib = tmp_->filePath(QStringLiteral("big"));
        if (QFileInfo::exists(lib)) return lib;
        Random rng(12);
        for (int f = 0; f < 60; ++f)
            for (int i = 0; i < 60; ++i)
                touch(lib + QStringLiteral("/Folder %1/").arg(f) + randomName(rng) + QStringLiteral(" %1.wav").arg(i));
        return lib;
    }

    std::unique_ptr<QTemporaryDir> tmp_;  // a new one for each test
};

QTEST_GUILESS_MAIN(TestBrowserNative)
#include "test_browser_native.moc"
