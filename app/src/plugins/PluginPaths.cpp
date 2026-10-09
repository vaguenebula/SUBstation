#include "plugins/PluginPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "Text.h"
#include "browser/PathKeys.h"

#ifndef _WIN32
#include "plugins/Vst3Format.h"
#endif

namespace sub::app {

namespace {

std::filesystem::path fsPath(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).toStdString());
#endif
}

// Python's str.lower(), for sorting as it sorted.
std::string lowerForSort(const QString& s) { return browser::pyLower(s.toStdString()); }

}  // namespace

QStringList standardPluginFolders() {
    if (qEnvironmentVariableIsSet("SUBSTATION_VST3_PATH")) {
        QStringList folders;
        for (const QString& folder : qEnvironmentVariable("SUBSTATION_VST3_PATH").split(QDir::listSeparator()))
            if (!folder.isEmpty()) folders << normalPath(folder);
        return folders;
    }
#ifdef _WIN32
    const QString common = qEnvironmentVariable("CommonProgramFiles", QStringLiteral("C:\\Program Files\\Common Files"));
    const QString local = qEnvironmentVariable("LOCALAPPDATA", QDir::home().filePath(QStringLiteral("AppData/Local")));
    return {normalPath(QDir::fromNativeSeparators(common) + QStringLiteral("/VST3")),
            normalPath(QDir::fromNativeSeparators(local) + QStringLiteral("/Programs/Common/VST3"))};
#else
    QStringList folders;
    for (const std::string& folder : vst3::Vst3Format::instance().defaultSearchPaths())
        folders << normalPath(QFile::decodeName(QByteArray::fromStdString(folder)));
    return folders;
#endif
}

QStringList pluginSearchFolders(const QStringList& custom) {
    // In order (Python kept them in a dict, which keeps insertion order).
    QStringList folders;
    QSet<QString> seen;
    QStringList all = standardPluginFolders();
    for (const QString& folder : custom)
        if (!folder.isEmpty()) all << folder;
    for (const QString& folder : all) {
        const QString key = pathKey(folder);
        if (seen.contains(key)) continue;
        seen.insert(key);
        folders << normalPath(folder);
    }
    return folders;
}

QStringList findPluginFiles(const QStringList& roots) {
    std::vector<QString> found;  // in the order found, one per key (the first)
    QSet<QString> foundKeys;
    QSet<QString> seen;  // real paths of the folders already listed
    auto add = [&](const QString& path) {
        if (!foundKeys.contains(caseKey(path))) {
            foundKeys.insert(caseKey(path));
            found.push_back(path);
        }
    };
    for (const QString& root : roots) {
        const QFileInfo rootInfo(root);
        if (root.toLower().endsWith(QStringLiteral(".vst3")) && rootInfo.exists()) {
            add(root);
            continue;
        }
        if (!rootInfo.isDir()) continue;
        std::vector<QString> stack{root};
        while (!stack.empty()) {
            const QString folder = stack.back();
            stack.pop_back();
            const QFileInfo info(folder);
            const QString canonical = info.canonicalFilePath();
            const QString real = caseKey(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
            if (seen.contains(real)) continue;
            seen.insert(real);
            const QStringList names = QDir(folder).entryList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::NoSort);
            for (const QString& name : names) {
                const QString path = folder.endsWith(QLatin1Char('/')) ? folder + name : folder + QLatin1Char('/') + name;
                if (name.toLower().endsWith(QStringLiteral(".vst3")))
                    add(path);
                else if (QFileInfo(path).isDir())  // following links
                    stack.push_back(path);
            }
        }
    }
    std::vector<std::pair<std::string, QString>> sorted;
    sorted.reserve(found.size());
    for (const QString& path : found) sorted.emplace_back(lowerForSort(path), path);
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QStringList files;
    for (auto& [_, path] : sorted) files << path;
    return files;
}

QStringList findPluginFiles() { return findPluginFiles(pluginSearchFolders()); }

QString pluginCachePath() { return localDataFile("SUBSTATION_PLUGIN_CACHE", QStringLiteral("vst3-cache.json")); }

QString pluginBinary(const QString& path) {
    const QFileInfo info(path);
#if defined(_WIN32)
    const QString inner = path + QStringLiteral("/Contents/x86_64-win/") + info.fileName();
#elif defined(__APPLE__)
    const QString inner = path + QStringLiteral("/Contents/MacOS/") + info.completeBaseName();
#elif defined(__aarch64__)
    const QString inner = path + QStringLiteral("/Contents/aarch64-linux/") + info.completeBaseName() + QStringLiteral(".so");
#else
    const QString inner = path + QStringLiteral("/Contents/x86_64-linux/") + info.completeBaseName() + QStringLiteral(".so");
#endif
    return info.isDir() && QFileInfo::exists(inner) ? inner : path;
}

std::optional<std::array<int64_t, 2>> pluginSignature(const QString& path) {
    std::error_code error;
    const std::filesystem::path file = fsPath(pluginBinary(path));
    const auto status = std::filesystem::status(file, error);
    if (error || !std::filesystem::exists(status)) return std::nullopt;
    const auto written = std::filesystem::last_write_time(file, error);
    if (error) return std::nullopt;
    // A folder has no size of its own (as Windows reports it).
    const uintmax_t size = std::filesystem::is_directory(status) ? 0 : std::filesystem::file_size(file, error);
    if (error) return std::nullopt;
    // Nanoseconds since 1970, as Python's st_mtime_ns (caches it wrote stay valid).
#ifdef _MSC_VER
    const auto sinceEpoch = std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch();
#else
    const auto sinceEpoch = std::chrono::file_clock::to_sys(written).time_since_epoch();
#endif
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count();
    return std::array<int64_t, 2>{ns, static_cast<int64_t>(size)};
}

QString friendlyScanReason(const QString& input) {
    QStringList words;
    for (const std::string& word : browser::pySplit(input.toStdString())) words << QString::fromStdString(word);
    const QString reason = words.join(QLatin1Char(' '));
    static const QRegularExpression number(QStringLiteral("LoadLibraryW failed with error number: (\\d+)"));
    const QRegularExpressionMatch match = number.match(reason);
    if (match.hasMatch()) {
        const int code = match.captured(1).toInt();
        QString what;
        switch (code) {
            case 126: what = QStringLiteral("a file it needs is missing"); break;
            case 193: what = QStringLiteral("it is not a 64-bit Windows plug-in"); break;
            case 1114: what = QStringLiteral("it failed to start"); break;
            default: what = QStringLiteral("error %1").arg(code);
        }
        return QStringLiteral("Windows could not load it: %1.").arg(what);
    }
    const qsizetype cut = reason.lastIndexOf(QStringLiteral(": "));
    if (reason.startsWith(QStringLiteral("LoadLibraryW failed for path")) && cut >= 0)
        return QStringLiteral("Windows could not load it: ") + reason.mid(cut + 2);
    return reason;
}

}  // namespace sub::app
