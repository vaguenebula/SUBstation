#pragma once
// The browser's index and search as they were in Python, before the native
// backend (browser/src) replaced them: the Python suite's reference, ported. It
// is the reference the native backend has to agree with, item for item and in
// the same order (test_browser_native.cpp), and what the browser benchmark
// checks every query against (benchmarks/browser_backend_bench.cpp).
//
// Python's text rules come from the backend's own ports of them (pyLower,
// pyCasefold, pySplit, wordStarts in browser/src/Text.h), which the tests
// check against values Python computed.

#include <QString>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "browser/BrowserItem.h"
#include "browser/FileIndex.h"
#include "browser/Library.h"

namespace sub::app::test::reference {

// Python's str.lower() of the text (UTF-8).
std::string lower(const QString& s);

// A path in Qt's form as std::filesystem takes it, and back.
std::filesystem::path fsPath(const QString& path);
QString fromFs(const std::filesystem::path& path);

// Names the walk skips: those starting with '.' or '$'.
bool hiddenName(const QString& name);

// DirEntry.is_dir(follow_symlinks=False): on Windows junctions are folders too.
bool isFolderNotLink(const std::filesystem::directory_entry& entry);

// The audio files under a folder, as the Python walk found them: depth first,
// the last folder first, at most `maxDepth` folders deep, no further folders
// once `maxFiles` files were found. Each file's detail is its folder's name.
std::vector<BrowserItem> walkAudio(const QString& root, uint32_t maxFiles, uint32_t maxDepth);

// What the index thread made of the places: their files, once each, by name.
std::vector<BrowserItem> indexPlaces(const QStringList& places, uint32_t maxFiles = FileIndex::kMaxFiles,
                                     uint32_t maxDepth = FileIndex::kMaxDepth);

// The query's words, in lower case.
std::vector<std::string> termsOf(const QString& query);
// Every term is in the item's name or detail.
bool matches(const BrowserItem& item, const std::vector<std::string>& terms);
// How well the terms match the item's name: at its start, at a word's start, inside it.
int matchQuality(const BrowserItem& item, const std::vector<std::string>& terms);

// The items that match the query, in the order `sort` ("rank" or "name") puts them.
std::vector<BrowserItem> find(std::vector<BrowserItem> items, const QString& query, const Library& library,
                              const QString& sort = QStringLiteral("rank"));

// The Places filter of the panel (in lower case where file names ignore case).
std::vector<BrowserItem> placeItems(const std::vector<BrowserItem>& items, const QString& place);

}  // namespace sub::app::test::reference
