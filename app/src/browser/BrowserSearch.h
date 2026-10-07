#pragma once
// Filtering and ordering the browser's lists: what to ask the search for.
//
// The backend does the searching (browser/src/Search.cpp), on its own thread;
// scopeQuery() turns a sidebar entry into what it searches. An item matches
// when every word of the query is in its name or detail (folder, vendor,
// category), ignoring case. That is also where later filters (items hidden
// from search) and orders (similar sounds) go.
//
// Sort orders:
//   "rank"  what you use most (and most recently) first, then the best name
//           matches, then the list's own order. With nothing used yet, a search
//           lists the names that start with what you typed first.
//   "name"  alphabetical.
//   "similar"  the sounds most like one, the most similar first (only while
//           the list shows them: BrowserController::findSimilar; files only).
//
// The list's own order: built-in devices, plug-ins (as the scan found them),
// presets (by the device they are for, then by name), then samples by name.

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <string>
#include <vector>

#include "browser/BrowserItem.h"

namespace sub::app {

// The groups of items searched, as the backend numbers them.
inline constexpr int kAudioGroup = 0;  // the index's files (sub::browser::kAudioGroup)
inline constexpr int kBuiltinGroup = 1;
inline constexpr int kPluginsGroup = 2;
inline constexpr int kPresetsGroup = 3;

// The sort orders as (value, label): "rank" (Rank), "name" (Name).
QVariantList sortOrders();
// Whether a user can choose it: "rank" or "name" ("similar" comes with Find Similar).
bool isSortOrder(const QString& sort);
inline const QString kSimilarSort = QStringLiteral("similar");

// A sidebar entry: what a list shows.
//   ("all")                     everything: built-in devices, plug-ins, presets, samples
//   ("samples")                 the index's audio files
//   ("builtin"[, category])     built-in devices (of a category)
//   ("plugins"[, category])     plug-ins ("Instruments" or "Audio Effects")
//   ("presets"[, device])       presets (for a device)
//   ("place", path)             a place's files (or, with no search text, its folder tree)
//   ("add")                     the "Add Folder…" entry (not a list)
struct Scope {
    QString kind = QStringLiteral("samples");
    QString sub;

    bool operator==(const Scope&) const = default;
    QStringList toList() const;  // as QML sees it: [kind] or [kind, sub]
    static Scope fromList(const QStringList& list);
};

struct ScopeQuery {
    std::vector<int> groups;  // in order
    QString tag;              // external items with this tag only ('' for all)
    std::string placePrefix;  // files under this place only (the backend's form; '' for all)
};

// What a sidebar entry lists.
ScopeQuery scopeQuery(const Scope& scope);

// Files under a place have paths starting with this, in the backend's form
// (sub::browser::placePrefix(): lower case where file names ignore case).
std::string placePrefix(const QString& place);

// The Plug-ins category an item is listed under: "Instruments" or "Audio Effects".
QString pluginTag(const BrowserItem& item);

}  // namespace sub::app
