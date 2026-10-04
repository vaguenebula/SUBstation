// Plug-in scanning (app/src/plugins): files are read in child processes, so a
// plug-in that crashes or hangs costs only itself; results are cached per file;
// the index scans in the background, one scan at a time, over the standard
// folders and the user's own.
//
// Most of it is tested without plug-ins: this test executable is also a fake
// scanner. Started with SUB_FAKE_SCANNER in its environment (as the scans here
// start it), it speaks the scanner's protocol before Qt starts, and answers for
// each file by what the file says: "plugins:<JSON list>", "error:<reason>",
// "crash" (it dies), "hang" (it never answers), "noise..." (stray lines first).
// SUB_FAKE_SCANNER_LOG names a file it appends "<process id> <path>" to for
// each file it reads. The real scanner (substation-scan) reads junk files, and
// the test plug-ins when they are built (SUBSTATION_TEST_PLUGINS_BUNDLE).

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "browser/PathKeys.h"
#include "plugins/PluginIndex.h"
#include "plugins/PluginPaths.h"
#include "plugins/PluginScanner.h"
#include "plugins/PluginSettings.h"

using namespace sub::app;

// --- The fake scanner ---------------------------------------------------------------------

namespace fake {

// (Qt's strings, JSON and files work before a QCoreApplication is made.)
int run(const QByteArray& mode) {
    if (mode == "exit") return 3;
    std::cout << "a plug-in saying hello (not JSON: skipped)" << std::endl;
    if (mode != "silent") std::cout << "{\"ready\": true}" << std::endl;
    const QString log = qEnvironmentVariable("SUB_FAKE_SCANNER_LOG");
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        // One JSON string a line.
        const QJsonArray wrapped = QJsonDocument::fromJson("[" + QByteArray::fromStdString(line) + "]").array();
        if (wrapped.isEmpty() || !wrapped[0].isString()) continue;
        const QString path = wrapped[0].toString();
        if (!log.isEmpty()) {
            QFile file(log);
            if (file.open(QIODevice::Append))
                file.write(QByteArray::number(QCoreApplication::applicationPid()) + ' ' + path.toUtf8() + '\n');
        }
        if (mode == "silent") continue;
        QFile file(path);
        QByteArray text = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        if (text.startsWith("crash")) std::_Exit(3);
        if (text.startsWith("hang"))
            for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
        if (text.startsWith("noise")) {
            std::cout << "[1, 2]\n{\"path\": \"somewhere else\", \"plugins\": []}\ngarbage {\n" << std::flush;
            text = text.mid(5);
        }
        QJsonObject reply{{QStringLiteral("path"), path}};
        if (text.startsWith("plugins:"))
            reply.insert(QStringLiteral("plugins"), QJsonDocument::fromJson(text.mid(8)).array());
        else if (text.startsWith("error:"))
            reply.insert(QStringLiteral("error"), QString::fromUtf8(text.mid(6)));
        else
            reply.insert(QStringLiteral("error"), QStringLiteral("not a plug-in"));
        std::cout << QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString() << std::endl;
    }
    return 0;
}

}  // namespace fake

namespace {

[[maybe_unused]] const bool kFakeScanner = [] {
    const QByteArray mode = qgetenv("SUB_FAKE_SCANNER");
    if (!mode.isEmpty()) std::exit(fake::run(mode));
    return false;
}();

QString self() { return QCoreApplication::applicationFilePath(); }

void writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path));
    file.write(content);
}

QByteArray plugin(const QString& name, const QString& vendor, const QString& category, bool instrument,
                  const QString& uid) {
    const QJsonObject p{{QStringLiteral("uid"), uid},           {QStringLiteral("name"), name},
                        {QStringLiteral("vendor"), vendor},     {QStringLiteral("version"), QStringLiteral("1.0.0")},
                        {QStringLiteral("category"), category}, {QStringLiteral("instrument"), instrument}};
    return QJsonDocument(p).toJson(QJsonDocument::Compact);
}

QByteArray plugins(const QList<QByteArray>& list) { return "plugins:[" + list.join(", ") + "]"; }

void setMtime(const QString& path, int secondsLater) {
#ifdef _WIN32
    const std::filesystem::path file(path.toStdWString());
#else
    const std::filesystem::path file(QFile::encodeName(path).toStdString());
#endif
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(secondsLater));
}

// The (process, path) pairs the fake scanner logged.
std::vector<std::pair<QString, QString>> readLog(const QString& log) {
    std::vector<std::pair<QString, QString>> out;
    QFile file(log);
    if (!file.open(QIODevice::ReadOnly)) return out;
    for (const QByteArray& line : file.readAll().split('\n')) {
        const qsizetype space = line.indexOf(' ');
        if (space > 0) out.push_back({QString::fromUtf8(line.left(space)), QString::fromUtf8(line.mid(space + 1))});
    }
    return out;
}

QStringList pathsRead(const QString& log) {
    QStringList out;
    for (const auto& [_, path] : readLog(log)) out << path;
    return out;
}

std::map<QString, QString> reasons(const ScanResult& result) {
    std::map<QString, QString> out;
    for (const ScanFailure& f : result.failures) out[QFileInfo(f.path).fileName()] = f.reason;
    return out;
}

struct Progress {
    std::vector<std::tuple<int, int, QString>> calls;
    PluginScanner::Progress callback() {
        return [this](int done, int total, const QString& path) { calls.emplace_back(done, total, path); };
    }
};

// An environment variable set for one test, and put back after it.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const QByteArray& value) : name_(name), had_(qEnvironmentVariableIsSet(name)), old_(qgetenv(name)) {
        qputenv(name, value);
    }
    ~ScopedEnv() {
        if (had_)
            qputenv(name_, old_);
        else
            qunsetenv(name_);
    }

private:
    const char* name_;
    bool had_;
    QByteArray old_;
};

}  // namespace

class TestPluginIndex : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        QCoreApplication::setOrganizationName(QStringLiteral("SUBstation Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("SUBstation Tests"));
        qputenv("SUB_FAKE_SCANNER", "1");  // the child processes started from here are fake scanners
    }

    void init() {
        tmp_ = std::make_unique<QTemporaryDir>();
        QVERIFY(tmp_->isValid());
        log_ = tmp_->filePath(QStringLiteral("scanner.log"));
        qputenv("SUB_FAKE_SCANNER", "1");
        qputenv("SUB_FAKE_SCANNER_LOG", log_.toUtf8());
        qputenv("SUBSTATION_PLUGIN_CACHE", tmp_->filePath(QStringLiteral("cache/vst3-cache.json")).toUtf8());
        qputenv("SUBSTATION_VST3_PATH", tmp_->filePath(QStringLiteral("VST3")).toUtf8());
        QSettings().remove(QString::fromLatin1(kPluginFoldersKey));
    }

    void cleanup() {
        QSettings().remove(QString::fromLatin1(kPluginFoldersKey));
        tmp_.reset();
    }

    // --- Scanning, with the fake scanner ---

    void scanReadsFilesInChildProcesses() {
        const QString a = path(QStringLiteral("VST3/Alpha.vst3"));
        const QString b = path(QStringLiteral("VST3/Broken.vst3"));
        const QString c = path(QStringLiteral("VST3/Nameless.vst3"));
        writeFile(a, plugins({plugin(QStringLiteral("Zeta Synth"), QStringLiteral("V"), QStringLiteral("Instrument|Synth"), true,
                                     QStringLiteral("A1")),
                              plugin(QStringLiteral("alpha FX"), QStringLiteral("V"), QStringLiteral("Fx|Delay"), false,
                                     QStringLiteral("A2"))}));
        writeFile(b, "error:LoadLibraryW failed with error number: 193 for path x\r\n");
        writeFile(c, plugins({plugin(QString(), QStringLiteral("W"), QStringLiteral("Fx"), false, QStringLiteral("C1"))}));
        PluginScanner scanner(self(), cacheFile());
        Progress progress;
        const ScanResult result = scanner.scan(QStringList{a, b, c}, false, progress.callback());
        QCOMPARE(progress.calls, (std::vector<std::tuple<int, int, QString>>{{0, 3, a}, {1, 3, b}, {2, 3, c}}));
        // By name, then vendor, ignoring case; a plug-in with no name is named after its file.
        QStringList names;
        for (const PluginInfo& p : result.plugins) names << p.name;
        QCOMPARE(names, QStringList({QStringLiteral("alpha FX"), QStringLiteral("Nameless"), QStringLiteral("Zeta Synth")}));
        const PluginInfo& synth = result.plugins[2];
        QCOMPARE(synth.path, a);
        QCOMPARE(synth.format, QStringLiteral("VST3"));
        QCOMPARE(synth.uid, QStringLiteral("A1"));
        QCOMPARE(synth.vendor, QStringLiteral("V"));
        QCOMPARE(synth.version, QStringLiteral("1.0.0"));
        QCOMPARE(synth.category, QStringLiteral("Instrument|Synth"));
        QVERIFY(synth.instrument);
        QCOMPARE(result.failures.size(), size_t(1));
        QCOMPARE(result.failures[0].path, b);
        QCOMPARE(result.failures[0].reason, QStringLiteral("Windows could not load it: it is not a 64-bit Windows plug-in."));
        // All in one process.
        const auto log = readLog(log_);
        QCOMPARE(log.size(), size_t(3));
        QCOMPARE(log[0].first, log[2].first);
    }

    void unchangedFilesAreNotReadAgain() {
        const QString a = path(QStringLiteral("VST3/A.vst3"));
        const QString b = path(QStringLiteral("VST3/B.vst3"));
        writeFile(a, plugins({plugin(QStringLiteral("A"), QStringLiteral("V"), QStringLiteral("Fx"), false, QStringLiteral("A1"))}));
        writeFile(b, "error:not a plug-in");
        PluginScanner scanner(self(), cacheFile());
        const ScanResult first = scanner.scan(QStringList{a, b});
        // The cache: version 1, keyed by path, with signatures as integers.
        QFile cache(cacheFile());
        QVERIFY(cache.open(QIODevice::ReadOnly));
        const QJsonObject data = QJsonDocument::fromJson(cache.readAll()).object();
        QCOMPARE(data.value(QStringLiteral("version")).toInt(), kPluginCacheVersion);
        const QJsonObject entry = data.value(QStringLiteral("files")).toObject().value(caseKey(a)).toObject();
        QCOMPARE(entry.value(QStringLiteral("path")).toString(), a);
        QCOMPARE(entry.value(QStringLiteral("signature")).toArray().size(), 2);
        QCOMPARE(entry.value(QStringLiteral("signature")).toArray()[1].toInteger(), QFileInfo(a).size());
        QVERIFY(entry.value(QStringLiteral("error")).isNull());

        // Unchanged files are not read again (failures neither)...
        Progress progress;
        QFile::remove(log_);
        const ScanResult again = scanner.scan(QStringList{a, b}, false, progress.callback());
        QVERIFY(progress.calls.empty());
        QVERIFY(pathsRead(log_).isEmpty());
        QCOMPARE(again.plugins, first.plugins);
        QCOMPARE(again.failures, first.failures);
        // ...changed ones are...
        setMtime(a, 10);
        scanner.scan(QStringList{a, b}, false, progress.callback());
        QCOMPARE(pathsRead(log_), QStringList{a});
        // ...and a rescan reads everything.
        QFile::remove(log_);
        scanner.scan(QStringList{a, b}, true);
        QCOMPARE(pathsRead(log_), QStringList({a, b}));
    }

    void theCacheKeepsWhatItKnows() {
        const QString a = path(QStringLiteral("VST3/A.vst3"));
        const QString b = path(QStringLiteral("More/B.vst3"));
        writeFile(a, plugins({plugin(QStringLiteral("A"), QStringLiteral("V"), QStringLiteral("Fx"), false, QStringLiteral("A1"))}));
        writeFile(b, plugins({plugin(QStringLiteral("B"), QStringLiteral("V"), QStringLiteral("Fx"), false, QStringLiteral("B1"))}));
        PluginScanner scanner(self(), cacheFile());
        scanner.scan(QStringList{a, b});
        // A scan without B (its folder removed): B is not in the result, but kept in the cache...
        const ScanResult without = scanner.scan(QStringList{a});
        QCOMPARE(without.plugins.size(), size_t(1));
        QFile::remove(log_);
        const ScanResult back = scanner.scan(QStringList{a, b});  // ...so adding it back is quick
        QCOMPARE(back.plugins.size(), size_t(2));
        QVERIFY(pathsRead(log_).isEmpty());
        // ...while it exists.
        QVERIFY(QFile::remove(b));
        scanner.scan(QStringList{a});
        writeFile(b, plugins({plugin(QStringLiteral("B"), QStringLiteral("V"), QStringLiteral("Fx"), false, QStringLiteral("B1"))}));
        QFile::remove(log_);
        scanner.scan(QStringList{a, b});
        QCOMPARE(pathsRead(log_), QStringList{b});

        // Another version, or a file that isn't JSON, counts as an empty cache.
        for (const QByteArray& damaged : {QByteArray(R"({"version": 2, "files": {}})"), QByteArray("{not json")}) {
            writeFile(cacheFile(), damaged);
            QFile::remove(log_);
            scanner.scan(QStringList{a});
            QCOMPARE(pathsRead(log_), QStringList{a});
        }
    }

    void aCrashingPluginCostsOnlyItself() {
        const QString junk = path(QStringLiteral("VST3/Junk.vst3"));
        const QString crash = path(QStringLiteral("VST3/Crash.vst3"));
        const QString more = path(QStringLiteral("VST3/More Junk.vst3"));
        writeFile(junk, "error:LoadLibraryW failed with error number: 193 for path x");
        writeFile(crash, "crash");
        writeFile(more, "error:LoadLibraryW failed with error number: 126 for path y");
        PluginScanner scanner(self(), cacheFile());
        const ScanResult result = scanner.scan(QStringList{junk, crash, more});
        QVERIFY(result.plugins.empty());
        const auto why = reasons(result);
        QCOMPARE(why.at(QStringLiteral("Crash.vst3")), QStringLiteral("The plug-in crashed while loading."));
        // The files before and after it were read, the latter by a new process.
        QCOMPARE(why.at(QStringLiteral("Junk.vst3")), QStringLiteral("Windows could not load it: it is not a 64-bit Windows plug-in."));
        QCOMPARE(why.at(QStringLiteral("More Junk.vst3")), QStringLiteral("Windows could not load it: a file it needs is missing."));
        const auto log = readLog(log_);
        QCOMPARE(log.size(), size_t(3));
        QCOMPARE(log[0].first, log[1].first);
        QVERIFY(log[2].first != log[1].first);
        // The failure is cached: not read again until it changes.
        QFile::remove(log_);
        scanner.scan(QStringList{junk, crash, more});
        QVERIFY(pathsRead(log_).isEmpty());
    }

    void aHangingPluginTimesOut() {
        const QString hang = path(QStringLiteral("VST3/Hang.vst3"));
        const QString junk = path(QStringLiteral("VST3/Junk.vst3"));
        writeFile(hang, "hang");
        writeFile(junk, "error:LoadLibraryW failed with error number: 193 for path x");
        QElapsedTimer timer;
        timer.start();
        const ScanResult result = PluginScanner(self(), cacheFile(), 2.0).scan(QStringList{hang, junk});
        QVERIFY(timer.elapsed() < 10000);
        QCOMPARE(reasons(result), (std::map<QString, QString>{
                                      {QStringLiteral("Hang.vst3"), QStringLiteral("The plug-in timed out.")},
                                      {QStringLiteral("Junk.vst3"), QStringLiteral("Windows could not load it: it is not a 64-bit Windows plug-in.")}}));
        const auto log = readLog(log_);
        QCOMPARE(log.size(), size_t(2));
        QVERIFY(log[0].first != log[1].first);
    }

    void strayLinesAreNotAnswers() {
        const QString a = path(QStringLiteral("VST3/Noisy.vst3"));
        writeFile(a, "noise" + plugins({plugin(QStringLiteral("Noisy"), QStringLiteral("V"), QStringLiteral("Fx"), false,
                                               QStringLiteral("N1"))}));
        const ScanResult result = PluginScanner(self(), cacheFile()).scan(QStringList{a});
        QCOMPARE(result.plugins.size(), size_t(1));
        QCOMPARE(result.plugins[0].name, QStringLiteral("Noisy"));
        QVERIFY(result.failures.empty());
    }

    void aScannerThatDoesNotStartFails_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("missing");
        QTest::newRow("it ends at once") << QStringLiteral("exit") << false;
        QTest::newRow("it never says it is ready") << QStringLiteral("silent") << false;
        QTest::newRow("there is none") << QStringLiteral("1") << true;
    }

    void aScannerThatDoesNotStartFails() {
        QFETCH(QString, mode);
        QFETCH(bool, missing);
        const ScopedEnv env("SUB_FAKE_SCANNER", mode.toUtf8());
        const QString a = path(QStringLiteral("VST3/A.vst3"));
        writeFile(a, "error:x");
        PluginScanner scanner(missing ? path(QStringLiteral("no-such-scanner")) : self(), cacheFile(), 1.0);
        try {
            scanner.scan(QStringList{a});
            QFAIL("no error");
        } catch (const std::runtime_error& error) {
            QCOMPARE(QString::fromUtf8(error.what()), QStringLiteral("The plug-in scanner could not start."));
        }
    }

    void aCancelledScanReturnsWhatItHas() {
        const QString a = path(QStringLiteral("VST3/A.vst3"));
        const QString b = path(QStringLiteral("VST3/B.vst3"));
        const QString c = path(QStringLiteral("VST3/C.vst3"));
        for (const QString& p : {a, b, c})
            writeFile(p, plugins({plugin(QFileInfo(p).completeBaseName(), QStringLiteral("V"), QStringLiteral("Fx"), false,
                                         QFileInfo(p).completeBaseName())}));
        PluginScanner scanner(self(), cacheFile());
        scanner.scan(QStringList{c});  // C is in the cache already
        writeFile(b, "hang");
        // Cancelled while B is read (it hangs): that doesn't wait for its time-out.
        bool cancel = false;
        const auto progress = [&](int done, int, const QString&) { cancel = done == 1; };
        QElapsedTimer timer;
        timer.start();
        const ScanResult result = scanner.scan(QStringList{a, b, c}, false, progress, [&] { return cancel; });
        QVERIFY(timer.elapsed() < 10000);
        QStringList names;
        for (const PluginInfo& p : result.plugins) names << p.name;
        QCOMPARE(names, QStringList({QStringLiteral("A"), QStringLiteral("C")}));  // B was not read; C came from the cache
        QVERIFY(result.failures.empty());
        // B was left out of the cache, A and C are in it.
        QFile::remove(log_);
        writeFile(b, "error:x");
        scanner.scan(QStringList{a, b, c});
        QCOMPARE(pathsRead(log_), QStringList{b});
    }

    void findingPluginFiles() {
        const QString root = tmp_->path();
        writeFile(root + QStringLiteral("/A/Vendor/Single.vst3"), {});
        writeFile(root + QStringLiteral("/A/Bundle.vst3/Contents/x86_64-win/Bundle.vst3"), {});  // inside the bundle: not listed on its own
        writeFile(root + QStringLiteral("/A/readme.txt"), "hello");
        QDir().mkpath(root + QStringLiteral("/B"));
        QCOMPARE(findPluginFiles({root + QStringLiteral("/A"), root + QStringLiteral("/B"), root + QStringLiteral("/Missing")}),
                 QStringList({root + QStringLiteral("/A/Bundle.vst3"), root + QStringLiteral("/A/Vendor/Single.vst3")}));
        // A root that is itself a .vst3 is listed as it is.
        QCOMPARE(findPluginFiles({root + QStringLiteral("/A/Vendor/Single.vst3")}),
                 QStringList{root + QStringLiteral("/A/Vendor/Single.vst3")});
        // A vendor folder that is a link to somewhere else (as some installers
        // make) is looked in; a link back up to the root doesn't loop.
        writeFile(root + QStringLiteral("/Elsewhere/Linked.vst3"), {});
        const bool linked = QFile::link(root + QStringLiteral("/Elsewhere"), root + QStringLiteral("/B/Vendor")) &&
                            QFile::link(root + QStringLiteral("/B"), root + QStringLiteral("/Elsewhere/Back"));
#ifdef _WIN32
        // (QFile::link makes shortcuts on Windows: junctions need mklink.)
        Q_UNUSED(linked);
#else
        QVERIFY(linked);
        QCOMPARE(findPluginFiles({root + QStringLiteral("/B")}), QStringList{root + QStringLiteral("/B/Vendor/Linked.vst3")});
#endif
        // Sorted ignoring case, as Python's str.lower.
        writeFile(root + QStringLiteral("/C/b.vst3"), {});
        writeFile(root + QStringLiteral("/C/A.vst3"), {});
        writeFile(root + QStringLiteral("/C/c.VST3"), {});
        QCOMPARE(findPluginFiles({root + QStringLiteral("/C")}),
                 QStringList({root + QStringLiteral("/C/A.vst3"), root + QStringLiteral("/C/b.vst3"), root + QStringLiteral("/C/c.VST3")}));
    }

    void searchPaths() {
        const QString root = tmp_->path();
        const QString a = root + QStringLiteral("/A"), b = root + QStringLiteral("/B"), c = root + QStringLiteral("/C");
        const ScopedEnv env("SUBSTATION_VST3_PATH", (a + QDir::listSeparator() + b).toUtf8());
        QCOMPARE(standardPluginFolders(), QStringList({a, b}));
        QCOMPARE(pluginSearchFolders(), QStringList({a, b}));
        // The user's own folders come after the standard ones, each folder once.
        QCOMPARE(pluginSearchFolders({c, a + QLatin1Char('/')}), QStringList({a, b, c}));
        QCOMPARE(pluginSearchFolders({c}), QStringList({a, b, c}));
#ifdef _WIN32
        QCOMPARE(pluginSearchFolders({c, root + QStringLiteral("/a\\")}), QStringList({a, b, c}));  // the same folder there
#else
        QCOMPARE(pluginSearchFolders({c, root + QStringLiteral("/a/")}), QStringList({a, b, c, root + QStringLiteral("/a")}));
#endif
        qputenv("SUBSTATION_VST3_PATH", "");
        QVERIFY(standardPluginFolders().isEmpty());
        qunsetenv("SUBSTATION_VST3_PATH");
        QVERIFY(!standardPluginFolders().isEmpty());  // the system's
    }

    void friendlyMessages() {
        QCOMPARE(friendlyScanReason(QStringLiteral("LoadLibraryW failed for path C:\\x\\A.vst3: A DLL initialization routine failed.\r\n\r\n")),
                 QStringLiteral("Windows could not load it: A DLL initialization routine failed."));
        QCOMPARE(friendlyScanReason(QStringLiteral("LoadLibraryW failed with error number: 126 for path C:\\a b\\B.vst3")),
                 QStringLiteral("Windows could not load it: a file it needs is missing."));
        QCOMPARE(friendlyScanReason(QStringLiteral("LoadLibraryW failed with error number: 5 for path x")),
                 QStringLiteral("Windows could not load it: error 5."));
        QCOMPARE(friendlyScanReason(QStringLiteral("The plug-in crashed while loading.")),
                 QStringLiteral("The plug-in crashed while loading."));
        QCOMPARE(friendlyScanReason(QStringLiteral("  two\t\nlines  ")), QStringLiteral("two lines"));
    }

    void theBundlesBinarySignsIt() {
#ifdef _WIN32
        const QString inner = QStringLiteral("/Contents/x86_64-win/Thing.vst3");
#elif defined(__aarch64__)
        const QString inner = QStringLiteral("/Contents/aarch64-linux/Thing.so");
#else
        const QString inner = QStringLiteral("/Contents/x86_64-linux/Thing.so");
#endif
        const QString bundle = path(QStringLiteral("VST3/Thing.vst3"));
        QDir().mkpath(bundle);
        QCOMPARE(pluginBinary(bundle), bundle);  // no code inside: the bundle itself
        writeFile(bundle + inner, "code");
        QCOMPARE(pluginBinary(bundle), bundle + inner);
        const auto signature = pluginSignature(bundle);
        QVERIFY(signature);
        QCOMPARE((*signature)[1], int64_t(4));
        setMtime(bundle + inner, 10);
        QVERIFY(pluginSignature(bundle) != signature);
        QVERIFY(!pluginSignature(path(QStringLiteral("VST3/Missing.vst3"))));
    }

    // --- The real scanner ---

    void theRealScannerReadsJunk() {
        const QString junk = path(QStringLiteral("VST3/Junk.vst3"));
        writeFile(junk, "not a plug-in");
        const QString missing = path(QStringLiteral("VST3/Missing.vst3"));
        const ScanResult result = PluginScanner(QStringLiteral(SUBSTATION_SCANNER), cacheFile()).scan(QStringList{junk, missing});
        QVERIFY(result.plugins.empty());
        QCOMPARE(result.failures.size(), size_t(2));
        QCOMPARE(result.failures[0].path, junk);
        QVERIFY(!result.failures[0].reason.isEmpty());
        QVERIFY(!result.failures[1].reason.isEmpty());
#ifdef _WIN32
        QCOMPARE(result.failures[0].reason, QStringLiteral("Windows could not load it: it is not a 64-bit Windows plug-in."));
#endif
    }

    void theRealScannerReadsTheTestPlugins() {
#ifndef SUBSTATION_TEST_PLUGINS_BUNDLE
        QSKIP("the test plug-ins are not built");
#else
        const QString bundle = path(QStringLiteral("VST3/SUBTestPlugins.vst3"));
        copyTree(QStringLiteral(SUBSTATION_TEST_PLUGINS_BUNDLE), bundle);
        const QString junk = path(QStringLiteral("VST3/Junk.vst3"));
        writeFile(junk, "not a plug-in");
        PluginScanner scanner(QStringLiteral(SUBSTATION_SCANNER), cacheFile());
        Progress progress;
        const ScanResult result = scanner.scan(QStringList{bundle, junk}, false, progress.callback());
        QCOMPARE(progress.calls, (std::vector<std::tuple<int, int, QString>>{{0, 2, bundle}, {1, 2, junk}}));
        QStringList found;
        for (const PluginInfo& p : result.plugins) {
            found << QStringLiteral("%1|%2|%3|%4").arg(p.name, p.vendor, p.category).arg(p.instrument);
            QCOMPARE(p.path, bundle);
            QCOMPARE(p.format, QStringLiteral("VST3"));
            QCOMPARE(p.uid.size(), 32);
        }
        QCOMPARE(found, QStringList({QStringLiteral("SUB Test Effect|SUBstation|Fx|Delay|0"),
                                     QStringLiteral("SUB Test Mono|SUBstation|Fx|0"),
                                     QStringLiteral("SUB Test Sidechain|SUBstation|Fx|Dynamics|0"),
                                     QStringLiteral("SUB Test Synth|SUBstation|Instrument|Synth|1")}));
        QCOMPARE(result.failures.size(), size_t(1));
        QCOMPARE(result.failures[0].path, junk);
#ifdef _WIN32
        QCOMPARE(result.failures[0].reason, QStringLiteral("Windows could not load it: it is not a 64-bit Windows plug-in."));
#endif
        // Unchanged files are not read again...
        Progress again;
        const ScanResult cached = scanner.scan(QStringList{bundle, junk}, false, again.callback());
        QVERIFY(again.calls.empty());
        QCOMPARE(cached.plugins, result.plugins);
        QCOMPARE(cached.failures, result.failures);
        // ...changed ones are...
        setMtime(pluginBinary(bundle), 10);
        scanner.scan(QStringList{bundle, junk}, false, again.callback());
        QCOMPARE(again.calls.size(), size_t(1));
        QCOMPARE(std::get<2>(again.calls[0]), bundle);
        // ...and a rescan reads everything.
        Progress all;
        scanner.scan(QStringList{bundle, junk}, true, all.callback());
        QCOMPARE(all.calls.size(), size_t(2));
#endif
    }

    void aCrashingTestPluginCostsOnlyItself() {
#ifndef SUBSTATION_TEST_PLUGINS_BUNDLE
        QSKIP("the test plug-ins are not built");
#else
        const QString bundle = path(QStringLiteral("VST3/SUBTestPlugins.vst3"));
        copyTree(QStringLiteral(SUBSTATION_TEST_PLUGINS_BUNDLE), bundle);
        const QString junk = path(QStringLiteral("VST3/Junk.vst3"));
        const QString more = path(QStringLiteral("VST3/More Junk.vst3"));
        writeFile(junk, "not a plug-in");
        writeFile(more, "nor this");
        const ScopedEnv crash("SUB_TEST_PLUGIN_CRASH", "1");  // the test plug-ins kill the process as they load
        const ScanResult result = PluginScanner(QStringLiteral(SUBSTATION_SCANNER), cacheFile()).scan(QStringList{junk, bundle, more});
        QVERIFY(result.plugins.empty());
        const auto why = reasons(result);
        QCOMPARE(why.at(QStringLiteral("SUBTestPlugins.vst3")), QStringLiteral("The plug-in crashed while loading."));
        // The files before and after it were read, the latter by a new process.
        QVERIFY(!why.at(QStringLiteral("Junk.vst3")).isEmpty());
        QVERIFY(!why.at(QStringLiteral("More Junk.vst3")).isEmpty());
        QVERIFY(why.at(QStringLiteral("More Junk.vst3")) != QStringLiteral("The plug-in crashed while loading."));
#endif
    }

    void aHangingTestPluginTimesOut() {
#ifndef SUBSTATION_TEST_PLUGINS_BUNDLE
        QSKIP("the test plug-ins are not built");
#else
        const QString bundle = path(QStringLiteral("VST3/SUBTestPlugins.vst3"));
        copyTree(QStringLiteral(SUBSTATION_TEST_PLUGINS_BUNDLE), bundle);
        const QString junk = path(QStringLiteral("VST3/Junk.vst3"));
        writeFile(junk, "not a plug-in");
        const ScopedEnv hang("SUB_TEST_PLUGIN_HANG", "1");
        QElapsedTimer timer;
        timer.start();
        const ScanResult result = PluginScanner(QStringLiteral(SUBSTATION_SCANNER), cacheFile(), 2.0).scan(QStringList{bundle, junk});
        QVERIFY(timer.elapsed() < 10000);
        const auto why = reasons(result);
        QCOMPARE(why.at(QStringLiteral("SUBTestPlugins.vst3")), QStringLiteral("The plug-in timed out."));
        QVERIFY(!why.at(QStringLiteral("Junk.vst3")).isEmpty());
#endif
    }

    // --- The index ---

    void theIndexScansInTheBackground() {
        writeStandardPlugins();
        PluginIndex index(nullptr, self());
        QSignalSpy updated(&index, &PluginIndex::updated);
        QSignalSpy progress(&index, &PluginIndex::progress);
        QSignalSpy status(&index, &PluginIndex::statusTextChanged);
        QVERIFY(!index.scanning());
        index.scan();
        QVERIFY(index.scanning());
        QCOMPARE(updated.count(), 1);  // scanning started
        QCOMPARE(index.statusText(), QStringLiteral("Scanning plug-ins…"));
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 20000);
        QCOMPARE(updated.count(), 2);  // and stopped
        QCOMPARE(progress.count(), 3);
        QCOMPARE(progress.at(0).at(0).toInt(), 0);
        QCOMPARE(progress.at(0).at(1).toInt(), 3);
        QVERIFY(status.count() >= 3);
        QCOMPARE(index.pluginCount(), 2);
        QCOMPARE(index.failureCount(), 1);
        QCOMPARE(index.statusText(), QStringLiteral("2 plug-ins found · 1 file could not be read"));
        QCOMPARE(index.failuresText(), QStringLiteral("Broken.vst3: not a plug-in"));
        QCOMPARE(index.failureList().size(), 1);
        QCOMPARE(index.failureList()[0].toMap().value(QStringLiteral("reason")).toString(), QStringLiteral("not a plug-in"));
        // The plug-ins as a model.
        PluginListModel* model = index.pluginModel();
        QCOMPARE(model->rowCount(), 2);
        QCOMPARE(model->get(0).value(QStringLiteral("name")).toString(), QStringLiteral("Delay"));
        QCOMPARE(model->get(1).value(QStringLiteral("name")).toString(), QStringLiteral("Synth"));
        QCOMPARE(model->get(1).value(QStringLiteral("instrument")).toBool(), true);
        QCOMPARE(model->get(1).value(QStringLiteral("toolTip")).toString(),
                 QStringLiteral("Synth (VST3 Instrument)\nV\nInstrument, Synth\n") + path(QStringLiteral("VST3/Synth.vst3")));
        const QVariantMap ref = model->get(1).value(QStringLiteral("ref")).toMap();
        QCOMPARE(ref.value(QStringLiteral("uid")).toString(), QStringLiteral("S1"));
        QCOMPARE(ref.value(QStringLiteral("format")).toString(), QStringLiteral("VST3"));
        QCOMPARE(model->roleNames().value(PluginListModel::NameRole), QByteArray("name"));
    }

    void oneScanAtATime() {
        writeStandardPlugins();
        PluginIndex index(nullptr, self());
        QSignalSpy updated(&index, &PluginIndex::updated);
        index.scan();
        index.scan(true);  // remembered: a rescan when this one is done
        index.scan();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 20000);
        // The first read every file (an empty cache), the second (a rescan) every file again.
        QCOMPARE(pathsRead(log_).size(), 6);
        QCOMPARE(updated.count(), 3);  // started, started again, stopped
        QCOMPARE(index.pluginCount(), 2);
        // Another scan reads nothing (all in the cache).
        QFile::remove(log_);
        index.scan();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 20000);
        QVERIFY(pathsRead(log_).isEmpty());
        QCOMPARE(index.pluginCount(), 2);
        index.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 20000);
        QCOMPARE(pathsRead(log_).size(), 3);
    }

    void aScannerThatCannotRunSaysSo() {
        writeStandardPlugins();
        PluginIndex index(nullptr, path(QStringLiteral("no-such-scanner")));
        QSignalSpy messages(&index, &PluginIndex::statusMessage);
        index.scan();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 20000);
        QCOMPARE(messages.count(), 1);
        QCOMPARE(messages.at(0).at(0).toString(), QStringLiteral("The plug-in scanner could not start."));
        QCOMPARE(index.pluginCount(), 0);
    }

    void waitingStopsAScan() {
        writeFile(path(QStringLiteral("VST3/Hang.vst3")), "hang");
        auto index = std::make_unique<PluginIndex>(nullptr, self());
        index->scan();
        QTest::qWait(300);
        QElapsedTimer timer;
        timer.start();
        index->wait();  // (also when it is destroyed)
        index.reset();
        QVERIFY(timer.elapsed() < 5000);
    }

    void theUsersOwnFolders() {
        writeStandardPlugins();
        const QString extra = path(QStringLiteral("More VST3"));
        writeFile(extra + QStringLiteral("/Vendor/Extra.vst3"),
                  plugins({plugin(QStringLiteral("Extra"), QStringLiteral("X"), QStringLiteral("Fx"), false, QStringLiteral("E1"))}));
        writeFile(extra + QStringLiteral("/Broken Too.vst3"), "error:not a plug-in either");

        // A one-item list can come back from QSettings as a string.
        QSettings().setValue(QString::fromLatin1(kPluginFoldersKey), extra);
        QCOMPARE(customPluginFolders(), QStringList{extra});
        QSettings().remove(QString::fromLatin1(kPluginFoldersKey));
        QVERIFY(customPluginFolders().isEmpty());

        PluginIndex index(nullptr, self());
        index.scan();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 20000);
        QCOMPARE(index.pluginCount(), 2);
        PluginFolderModel* folders = index.folderModel();
        QCOMPARE(folders->rowCount(), 1);
        const QModelIndex standard = folders->index(0);
        QVERIFY(standard.data(PluginFolderModel::StandardRole).toBool());
        QVERIFY(!standard.data(PluginFolderModel::RemovableRole).toBool());
        QVERIFY(standard.data(PluginFolderModel::DisplayRole).toString().endsWith(QStringLiteral("  (standard)")));
        QCOMPARE(standard.data(PluginFolderModel::ToolTipRole).toString(), QStringLiteral("A standard VST3 folder: always searched."));

        // An added folder is kept, and its plug-ins (in folders inside it too) are found.
        QSignalSpy foldersChanged(&index, &PluginIndex::foldersChanged);
        QVERIFY(index.addFolder(extra));
        QVERIFY(!index.addFolder(extra + QLatin1Char('/')));  // the same folder again: nothing changes
        QVERIFY(!index.addFolder(path(QStringLiteral("VST3"))));  // a standard one
        QCOMPARE(foldersChanged.count(), 1);
        QCOMPARE(customPluginFolders(), QStringList{extra});
        QCOMPARE(index.customFolders(), QStringList{extra});
        QCOMPARE(pluginFolders(), QStringList({path(QStringLiteral("VST3")), extra}));
        QCOMPARE(folders->rowCount(), 2);
        QVERIFY(folders->index(1).data(PluginFolderModel::RemovableRole).toBool());
        QVERIFY(folders->index(1).data(PluginFolderModel::ExistsRole).toBool());
        QCOMPARE(folders->find(extra + QLatin1Char('/')), 1);
        QVERIFY(index.scanning());
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 20000);
        QCOMPARE(index.pluginCount(), 3);
        QCOMPARE(index.failureCount(), 2);
        QCOMPARE(index.statusText(), QStringLiteral("3 plug-ins found · 2 files could not be read"));

        // A folder that went is shown as missing.
        const QString gone = path(QStringLiteral("Gone"));
        QVERIFY(index.addFolder(gone));
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 20000);
        QVERIFY(!folders->index(2).data(PluginFolderModel::ExistsRole).toBool());
        QVERIFY(folders->index(2).data(PluginFolderModel::ToolTipRole).toString().endsWith(
            QStringLiteral("\nThis folder doesn't exist (any more).")));

        // A removed folder's plug-ins go.
        index.removeFolder(gone);
        index.removeFolder(extra + QLatin1Char('/'));
        QCOMPARE(customPluginFolders(), QStringList());
        QCOMPARE(folders->rowCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 20000);
        QCOMPARE(index.pluginCount(), 2);
        QCOMPARE(index.failureCount(), 1);
        QCOMPARE(index.statusText(), QStringLiteral("2 plug-ins found · 1 file could not be read"));
    }

private:
    QString path(const QString& relative) const { return tmp_->filePath(relative); }
    QString cacheFile() const { return tmp_->filePath(QStringLiteral("cache/vst3-cache.json")); }

    // Two plug-ins and a broken file in the standard folder (SUBSTATION_VST3_PATH).
    void writeStandardPlugins() {
        writeFile(path(QStringLiteral("VST3/Synth.vst3")),
                  plugins({plugin(QStringLiteral("Synth"), QStringLiteral("V"), QStringLiteral("Instrument|Synth"), true,
                                  QStringLiteral("S1"))}));
        writeFile(path(QStringLiteral("VST3/Effects/Delay.vst3")),
                  plugins({plugin(QStringLiteral("Delay"), QStringLiteral("V"), QStringLiteral("Fx|Delay"), false,
                                  QStringLiteral("D1"))}));
        writeFile(path(QStringLiteral("VST3/Broken.vst3")), "error:not a plug-in");
    }

    static void copyTree(const QString& from, const QString& to) {
        QDir().mkpath(QFileInfo(to).absolutePath());  // (copy() makes `to`, not the folders above it)
        std::error_code error;
#ifdef _WIN32
        std::filesystem::copy(from.toStdWString(), to.toStdWString(), std::filesystem::copy_options::recursive, error);
#else
        std::filesystem::copy(QFile::encodeName(from).toStdString(), QFile::encodeName(to).toStdString(),
                              std::filesystem::copy_options::recursive, error);
#endif
        QVERIFY2(!error, error.message().c_str());
    }

    std::unique_ptr<QTemporaryDir> tmp_;
    QString log_;
};

QTEST_GUILESS_MAIN(TestPluginIndex)
#include "test_plugin_index.moc"
