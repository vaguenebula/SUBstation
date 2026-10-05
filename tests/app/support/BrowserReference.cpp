#include "BrowserReference.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <string_view>
#include <system_error>
#include <utility>

#include "Platform.h"
#include "Text.h"
#include "browser/PathKeys.h"

namespace sub::app::test::reference {

namespace backend = sub::browser;

namespace {

const QStringList kExtensions = FileIndex::audioExtensions();

}  // namespace

std::string lower(const QString& s) { return backend::pyLower(s.toStdString()); }

std::filesystem::path fsPath(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).toStdString());
#endif
}

QString fromFs(const std::filesystem::path& path) {
#ifdef _WIN32
    return QDir::fromNativeSeparators(QString::fromStdWString(path.wstring()));
#else
    return QFile::decodeName(QByteArray::fromStdString(path.string()));
#endif
}

bool hiddenName(const QString& name) { return name.startsWith(QLatin1Char('.')) || name.startsWith(QLatin1Char('$')); }

bool isFolderNotLink(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    const auto type = entry.symlink_status(error).type();
#ifdef _MSC_VER
    if (type == std::filesystem::file_type::junction) return true;
#endif
    return !error && type == std::filesystem::file_type::directory;
}

std::vector<BrowserItem> walkAudio(const QString& root, uint32_t maxFiles, uint32_t maxDepth) {
    std::vector<BrowserItem> items;
    std::vector<std::pair<QString, uint32_t>> stack{{root, 0}};
    while (!stack.empty() && items.size() < maxFiles) {
        const auto [folder, depth] = stack.back();
        stack.pop_back();
        std::vector<std::filesystem::directory_entry> entries;
        std::error_code error;
        for (std::filesystem::directory_iterator it(fsPath(folder), error), end; !error && it != end; it.increment(error))
            entries.push_back(*it);
        if (error) continue;
        for (const auto& entry : entries) {
            const QString name = fromFs(entry.path().filename());
            if (hiddenName(name)) continue;
            const QString path = folder + QLatin1Char('/') + name;
            if (isFolderNotLink(entry)) {
                if (depth < maxDepth) stack.push_back({path, depth + 1});
            } else if (std::any_of(kExtensions.begin(), kExtensions.end(), [&](const QString& ext) {
                           return QString::fromStdString(lower(name)).endsWith(ext);
                       })) {
                items.push_back({name, path, ItemKind::Audio, QFileInfo(folder).fileName(), std::nullopt, {}});
            }
        }
    }
    return items;
}

std::vector<BrowserItem> indexPlaces(const QStringList& places, uint32_t maxFiles, uint32_t maxDepth) {
    std::vector<BrowserItem> audio;
    QSet<QString> seen;
    for (const QString& place : places) {
        if (!QFileInfo(place).isDir()) continue;
        for (BrowserItem& item : walkAudio(place, maxFiles, maxDepth)) {
            const QString key = caseKey(item.path);
            if (seen.contains(key)) continue;
            seen.insert(key);
            audio.push_back(std::move(item));
        }
    }
    std::stable_sort(audio.begin(), audio.end(),
                     [](const BrowserItem& a, const BrowserItem& b) { return lower(a.name) < lower(b.name); });
    return audio;
}

std::vector<std::string> termsOf(const QString& query) { return backend::pySplit(lower(query)); }

bool matches(const BrowserItem& item, const std::vector<std::string>& terms) {
    const std::string haystack = lower(item.name + QLatin1Char(' ') + item.detail);
    return std::all_of(terms.begin(), terms.end(), [&](const std::string& t) { return haystack.find(t) != std::string::npos; });
}

int matchQuality(const BrowserItem& item, const std::vector<std::string>& terms) {
    const std::string name = lower(item.name);
    std::string stem = name;
    if (item.kind == ItemKind::Audio && name.rfind('.') != std::string::npos) stem = name.substr(0, name.rfind('.'));
    std::string joined;
    for (const std::string& term : terms) joined += (joined.empty() ? "" : " ") + term;
    int quality = stem == joined ? 8 : 0;
    std::vector<uint32_t> starts;
    backend::wordStarts(name, starts);
    for (const std::string& term : terms) {
        if (name.starts_with(term))
            quality += 3;
        else if (std::any_of(starts.begin(), starts.end(), [&](uint32_t at) { return std::string_view(name).substr(at).starts_with(term); }))
            quality += 2;
        else if (name.find(term) != std::string::npos)
            quality += 1;
    }
    return quality;
}

std::vector<BrowserItem> find(std::vector<BrowserItem> items, const QString& query, const Library& library,
                              const QString& sort) {
    const std::vector<std::string> terms = termsOf(query);
    if (!terms.empty()) {
        std::erase_if(items, [&](const BrowserItem& item) { return !matches(item, terms); });
    }
    if (sort == QStringLiteral("name")) {
        std::stable_sort(items.begin(), items.end(), [](const BrowserItem& a, const BrowserItem& b) {
            return backend::pyCasefold(a.name.toStdString()) < backend::pyCasefold(b.name.toStdString());
        });
        return items;
    }
    const double now = library.now();
    if (terms.empty()) {  // only the used items move; the rest keep their order
        const bool used = std::any_of(items.begin(), items.end(), [&](const BrowserItem& i) { return library.records().contains(i.key()); });
        if (!used) return items;
        std::stable_sort(items.begin(), items.end(), [&](const BrowserItem& a, const BrowserItem& b) {
            return -library.rank(a.key(), now) < -library.rank(b.key(), now);
        });
        return items;
    }
    std::vector<std::pair<std::pair<double, int>, BrowserItem>> keyed;
    for (BrowserItem& item : items) keyed.push_back({{-library.rank(item.key(), now), -matchQuality(item, terms)}, std::move(item)});
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    items.clear();
    for (auto& [_, item] : keyed) items.push_back(std::move(item));
    return items;
}

std::vector<BrowserItem> placeItems(const std::vector<BrowserItem>& items, const QString& place) {
    std::vector<BrowserItem> out;
    for (const BrowserItem& item : items) {
        const bool under = backend::platform::kCaseSensitivePaths
                               ? item.path.startsWith(place + QLatin1Char('/'))
                               : lower(item.path).starts_with(lower(place) + "/");
        if (under) out.push_back(item);
    }
    return out;
}

}  // namespace sub::app::test::reference
