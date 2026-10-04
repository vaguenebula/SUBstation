#pragma once
// Where VST3 plug-ins are looked for and the files found there, the scan
// cache's place, and how a file's errors read for the user (the free functions
// of the old scanner.py).

#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <optional>

namespace sub::app {

inline constexpr int kPluginCacheVersion = 1;
inline constexpr double kPluginScanTimeout = 60.0;  // seconds one file may take (some copy-protected plug-ins are slow)

// The standard VST3 folders, or those in SUBSTATION_VST3_PATH when it is set
// (separated by the system's list separator; empty for none: the tests point
// it at the test plug-ins, so they never see the installed ones). On Windows
// %CommonProgramFiles%\VST3 and %LOCALAPPDATA%\Programs\Common\VST3; elsewhere
// the engine's (Vst3Format::defaultSearchPaths(): ~/.vst3, /usr/lib/vst3,
// /usr/local/lib/vst3 on Linux).
QStringList standardPluginFolders();

// The standard folders, then the user's own, each once (compared by pathKey()).
QStringList pluginSearchFolders(const QStringList& custom = {});

// Every .vst3 bundle (a folder) or file under the roots; nothing inside a
// bundle is listed. Linked folders (symbolic links, junctions) are followed,
// each real folder once, so a link back up can't loop. A root that is itself a
// .vst3 is listed as it is. Unreadable folders are skipped. Sorted ignoring
// case (as Python's str.lower), one entry per path key.
QStringList findPluginFiles(const QStringList& roots);
QStringList findPluginFiles();  // under pluginSearchFolders()

// vst3-cache.json in localDataDir(), or SUBSTATION_PLUGIN_CACHE if set.
QString pluginCachePath();

// The file a VST3 bundle's code is in (the path itself for a single file, or a
// bundle without it): Contents/x86_64-win/<name>.vst3 on Windows,
// Contents/<arch>-linux/<name>.so on Linux.
QString pluginBinary(const QString& path);

// When the file a plug-in's code is in changed, and its size: [modification
// time in ns since 1970, size], as Python's [st_mtime_ns, st_size]. A file is
// read again when it changes. Nothing if it can't be read.
std::optional<std::array<int64_t, 2>> pluginSignature(const QString& path);

// The loader's error as a line for the user: whitespace collapsed; Windows'
// LoadLibraryW error numbers named ("Windows could not load it: ..."), or only
// Windows' message kept. Other reasons pass as they are.
QString friendlyScanReason(const QString& reason);

}  // namespace sub::app
