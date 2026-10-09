# Plug-in scanner

Finds the VST3 plug-ins installed on this computer and what each file contains, without letting a broken plug-in take
the program down. It lives in [app/src/plugins](../../app/src/plugins): the scanner
([PluginScanner.h](../../app/src/plugins/PluginScanner.h)), where it looks and its cache
([PluginPaths.h](../../app/src/plugins/PluginPaths.h)), the index the browser and the preferences show
([PluginIndex.h](../../app/src/plugins/PluginIndex.h)), the user's own plug-in folders
([PluginSettings.h](../../app/src/plugins/PluginSettings.h)); and in [tools/scanner](../../tools/scanner/main.cpp),
`substation-scan`, the child process that reads plug-in files.

How the user sees it (the browser's *Plug-ins* category, *Rescan Plug-ins*, the folders in the preferences) is in
[guide/plugins.md](../guide/plugins.md). Loading and hosting a plug-in once found is the engine's: see
[engine/plugins.md](../engine/plugins.md).

## Overview

- Reading a plug-in file means running its code, and a broken plug-in can crash or hang the process that loads it. So
  files are read in a child process, `substation-scan`, several per process: if one takes the process down or doesn't
  answer in time, it is marked as failed and the rest carry on in a new process.
- Results are cached per file in `vst3-cache.json` (in `%LOCALAPPDATA%\SUBstation` on Windows,
  `~/.local/share/SUBstation` on Linux), so a file is read again only when it changes (or on a rescan). The scan runs
  in the background at start-up and reads only new or changed files.
- Plug-ins are looked for in the standard VST3 folders (on Windows `C:\Program Files\Common Files\VST3` and
  `%LOCALAPPDATA%\Programs\Common\VST3`; on Linux `~/.vst3`, `/usr/lib/vst3`, `/usr/local/lib/vst3`) and in folders of
  the user's own.
- `PluginScanner` needs no event loop and touches no UI: the plug-in index runs it on a thread of its own.
- `substation-scan` links the engine's VST3 host (`sub_engine`'s `Vst3Format`) and nothing of Qt.

## Files

| File | What it holds |
|---|---|
| [PluginInfo.h](../../app/src/plugins/PluginInfo.h) | `PluginInfo`, `ScanFailure`, `ScanResult`; `pluginToolTip` |
| [PluginPaths.h](../../app/src/plugins/PluginPaths.h) | `standardPluginFolders()`, `pluginSearchFolders()`, `findPluginFiles()`, `pluginCachePath()`, `pluginBinary()`, `pluginSignature()`, `friendlyScanReason()`; `kPluginCacheVersion`, `kPluginScanTimeout` |
| [PluginScanner.h](../../app/src/plugins/PluginScanner.h) | `PluginScanner`: the cache, the child processes, `scan()`; its header comment is the protocol and the cache's format |
| [PluginSettings.h](../../app/src/plugins/PluginSettings.h) | The user's VST3 folders in `QSettings` (`plugins/vst3_folders`): `customPluginFolders()`, `setCustomPluginFolders()`, `pluginFolders()` |
| [PluginIndex.h](../../app/src/plugins/PluginIndex.h) | `PluginIndex` (`Session.plugins`): runs scans in the background, one at a time; holds the plug-ins and failures; the folders; Preferences › Plug-ins binds to it |
| [PluginListModel.h](../../app/src/plugins/PluginListModel.h), [PluginFolderModel.h](../../app/src/plugins/PluginFolderModel.h) | The plug-ins and the folders as list models, for QML |
| [tools/scanner/main.cpp](../../tools/scanner/main.cpp) | `substation-scan`: answers one JSON line per plug-in file, using the engine's `Vst3Format::scanFile()` |
| [browser/BrowserController.h](../../app/src/browser/BrowserController.h) | the browser's *Plug-ins* category, *Rescan Plug-ins*, the failures in the *Plug-ins* tooltip and the status line |

## Key types

```cpp
struct PluginInfo {        // one plug-in (a VST3 class) in a file
    QString name;
    QString format;        // "VST3"
    QString path;          // the .vst3 bundle or file
    QString uid;           // VST3 class id (32 hex digits)
    QString vendor;
    QString version;
    QString category;      // VST3 sub-categories, e.g. "Instrument|Synth" or "Fx|EQ"
    bool instrument = false;
    QVariantMap toRef() const;  // what a device needs to load it (PluginRef's fields), for QML
};

struct ScanFailure {
    QString path;
    QString reason;        // for the user
};

struct ScanResult {
    std::vector<PluginInfo> plugins;
    std::vector<ScanFailure> failures;
};
```

A module with several plug-ins (an instrument and its FX version) gives one `PluginInfo` each. The plug-ins are sorted
by name, then vendor, ignoring case (their full Unicode lower case, compared code point by code point). A plug-in with no name is named after its
file.

## Where it looks

- `standardPluginFolders()`: on Windows `%CommonProgramFiles%\VST3` and `%LOCALAPPDATA%\Programs\Common\VST3` (with
  fallbacks if the variables are missing); elsewhere the engine's list (`Vst3Format::defaultSearchPaths()`: `~/.vst3`,
  `/usr/lib/vst3`, `/usr/local/lib/vst3` on Linux). If `SUBSTATION_VST3_PATH` is set, those folders instead
  (separated by the system's list separator, `;` on Windows and `:` elsewhere; empty for none). The tests set it, so
  they never see the installed plug-ins.
- `pluginSearchFolders(custom)`: the standard folders, then the user's own, each once (compared by `pathKey()`: the
  path cleaned up (`QDir::cleanPath`), and on Windows with backslashes and in lower case; elsewhere case counts).
- `pluginFolders()`: `pluginSearchFolders(customPluginFolders())`, what a scan looks in.
- `findPluginFiles(roots)`: every `.vst3` bundle (a folder) or file under the roots. Nothing inside a bundle is listed.
  Linked folders (symbolic links, junctions) are followed, each real folder once (by its canonical path), so a link
  back up can't loop. A root that is itself a `.vst3` is listed as it is. Unreadable folders are skipped. Sorted
  ignoring case, one entry per `caseKey()` path.

## How a scan works

```
 PluginScanner::scan(files = none, rescan = false, progress, cancelled)
   files = findPluginFiles(folders)
   cache = vst3-cache.json (unless rescan)
   for each file: signature [mtime ns, size] of its binary matches the cache? → reuse, else → to read
   read(to read):                                         child process (substation-scan)
     start it; every path written to its input,
     then its input closed  ────────────────────────────► {"ready": true}
                                                           for each line: Vst3Format::scanFile(path)
     its output read line by line  ◄───────────────────── {"path", "plugins": [...]} or {"path", "error"}
     per file: wait up to `timeout` for its answer
       no answer, process ended → "The plug-in crashed while loading."  \  note the failure,
       no answer in time        → "The plug-in timed out."               /  start a new process for the rest
   save the cache: entries read now + reused ones + cached files not in this scan that still exist
   ScanResult (plugins sorted, failures with friendly reasons)
```

### The signature

A file is read again when its *signature* changes: the modification time (nanoseconds since
1970) and size of the file its code is in (`pluginSignature()`). For a bundle
that is `Contents/x86_64-win/<name>.vst3` inside it on Windows, `Contents/x86_64-linux/<name>.so` on Linux
(`aarch64-linux` on ARM) (`pluginBinary()`), else the path itself. A file whose signature can't be read gets none,
which never matches.

### The child process

`PluginScanner(program, cacheFile, timeout, folders, arguments)`: `program` defaults to `defaultProgram()`,
`substation-scan` next to the application's executable (`substation-scan.exe` on Windows), or `SUBSTATION_SCANNER` if
set. It is started with `QProcess`, its error output discarded, and on Windows with `CREATE_NO_WINDOW` (no console
flashing up).

`substation-scan` ([tools/scanner/main.cpp](../../tools/scanner/main.cpp)):

1. Keeps its output for its answers: it duplicates its standard output for itself, then points standard output and
   standard error at the null device, so whatever plug-ins print goes nowhere.
2. On Windows sets the error mode (`SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX`): no
   "program stopped working" or "insert a disk" dialogs if a plug-in misbehaves.
3. Says `{"ready": true}`.
4. For each JSON-encoded path on its input (one a line; a line that isn't a JSON string is skipped), answers one JSON
   line: `{"path": ..., "plugins": [{"uid", "name", "vendor", "version", "category", "instrument"}, ...]}` from
   `Vst3Format::scanFile(path)`, or `{"path": ..., "error": ...}` if it throws. Loading the module is the engine's (see
   [engine/plugins.md](../engine/plugins.md#vst3format)).
5. Ends when its input ends.

The parent (`PluginScanner::read`):

- writes every path at once (a JSON string a line, anything outside printable ASCII escaped as `\uXXXX`), then
  closes the child's input: `QProcess` buffers what the child hasn't read yet, so a plug-in that hangs (and stops
  the child reading) can't stop the parent;
- reads the child's output as it comes, with `QProcess`'s waiting functions (a tenth of a second at a time, checking
  `cancelled` between): no event loop is needed, but a thread Qt knows;
- waits for the child's first JSON object (it says it is ready); if the child ends or doesn't say it within the
  timeout, throws `std::runtime_error("The plug-in scanner could not start.")`;
- for each path, waits up to `timeout` (`kPluginScanTimeout`, 60 s: some copy-protected plug-ins are slow) for the
  answer with that path; lines that aren't JSON objects, or answer another path, are skipped as not its own;
- on a crash or a time-out, records the failure (`"The plug-in crashed while loading."` or `"The plug-in timed out."`)
  and starts a new process for the remaining paths;
- when the batch is done, waits up to 5 s for the child to end by itself (none if it failed), then kills it.

`progress(done, total, path)` is called before each file that is read; `cancelled()` is checked between files, between
child processes and while waiting. A cancelled scan returns what it has; files not reached are left out of the result
but keep their cache entries.

### The cache file

`pluginCachePath()`: `vst3-cache.json` in `localDataDir()` (`%LOCALAPPDATA%\SUBstation` on Windows, the system's place
for application data elsewhere: `~/.local/share/SUBstation`), or `SUBSTATION_PLUGIN_CACHE` if set (the tests point it at
a temporary file). Caches written by earlier versions of SUBstation are read as they are.

```json
{
 "version": 1,
 "files": {
  "c:\\program files\\common files\\vst3\\foo.vst3": {
   "path": "C:\\Program Files\\Common Files\\VST3\\Foo.vst3",
   "signature": [1712345678901234567, 1048576],
   "plugins": [{"uid": "...", "name": "Foo", "vendor": "...", "version": "1.0.0",
                "category": "Fx|EQ", "instrument": false}],
   "error": null
  }
 }
}
```

- Keys are `caseKey(path)`: on Windows the path in lower case with backslashes; elsewhere the path as it is.
- A file version other than `kPluginCacheVersion` (1), or a file that can't be read or parsed, counts as an empty cache.
- It is written with `QSaveFile` (a temporary file renamed into place), so a crash never leaves half a cache. A cache
  that can't be written only costs time. (Reading and writing it is `readVersionedObject`/`writeVersionedObject`,
  [io/Json.h](../../app/src/io/Json.h), as the browser's `library.json` is.)
- Failures are cached too: a file that crashed or timed out is not read again until it changes, or on a rescan.
- Entries of files not in this scan (a folder removed from the list) are kept while the file exists, so adding the
  folder back is quick; they don't appear in the result.

### Friendly reasons

`friendlyScanReason(reason)` turns the loader's error into a line for the user: whitespace collapsed; Windows'
`LoadLibraryW` error numbers named (126: "a file it needs is missing", 193: "it is not a 64-bit Windows plug-in", 1114:
"it failed to start", others "error N"), as "Windows could not load it: ..."; a
`LoadLibraryW failed for path ...: <message>` keeps only Windows' message. Other reasons pass through as they are.

## In the application

- **Start-up.** The `BrowserController` the session makes calls `PluginIndex::scan()` (unless the session's options
  say not to: the tests). A `QThread` (low priority) runs `PluginScanner(scanner, {}, kPluginScanTimeout,
  pluginFolders()).scan(...)` and hands back the plug-ins and failures when it finishes; the index's models update
  then, and the session passes the plug-ins to the bridge (`EngineBridge::setKnownPlugins`), which tries devices whose
  plug-in wasn't found again. Only new or changed files are read.
- **One scan at a time.** A scan asked for while one runs is remembered (as a rescan if either asked for one) and runs
  when it finishes. Closing stops a scan that runs (`PluginIndex::wait()`: it is interrupted between files) and waits
  for its thread.
- **Rescan.** *Rescan Plug-ins* (the browser's menu) and the button in *Preferences › Plug-ins* call `scan(true)`
  (`rescan()`): every file is read again, also those that failed before.
- **Failures.** Hover over *Plug-ins* in the browser for the list and the reasons (the first 30); the status line says
  how many files could not be read. The preferences show the count and the reasons (`failuresText`, `failureList`). A
  scanner that can't start goes to the status line (`PluginIndex::statusMessage`).
- **User folders.** *Preferences › Plug-ins* lists the standard folders (always searched) and the user's own
  (`standardFolders`, `customFolders`, the `folders` model). `addFolder` stores it (unless it is already listed: a
  standard folder or one of theirs) and scans at once: only the new folder's files are read, the rest come from the
  cache. `removeFolder` takes it out of the list and scans again, so its plug-ins leave the browser. Folders are kept
  in `QSettings` under `plugins/vst3_folders` (a one-item list comes back from `QSettings` as a string; it is read
  either way).
- **Progress.** The browser's status shows "Scanning plug-ins 3/40: Name" while the *Plug-ins* category is shown; the
  preferences show "Scanning 3/40: Name" (`statusText`), and "12 plug-ins found · 1 file could not be read" after.
- The browser lists the plug-ins under *Plug-ins › Instruments / Audio Effects* with their vendor; a project's plug-in
  devices are matched to them by class id, so a plug-in that moved is found again (see
  [engine-bridge.md](engine-bridge.md#plug-ins) and [browser.md](../browser.md)).

## Invariants

- Plug-in code never runs in the application's process during a scan. (Loading a plug-in onto a track does run it in
  the application, on the GUI thread: that is hosting, not scanning.)
- The child's output carries only its JSON answers.
- One answer per path, in order; the parent matches answers by path, so a stray line can't be taken for an answer.

## Extending it

- **Another format** (CLAP): `substation-scan` would call that format's scanner by file extension and answer with a
  format field; `PluginInfo::format` already exists, and `findPluginFiles()` would look for the format's extension.
  Bump `kPluginCacheVersion` if the cache's shape changes. See
  [engine/plugins.md](../engine/plugins.md#clap-not-implemented).
- **More metadata**: add it to the scanner's answer, to `PluginInfo`, and to the reading in `PluginScanner::scan()`.
  Old cache entries lack it; bump `kPluginCacheVersion` to read every file again.

## Gotchas

- A changed plug-in is only noticed by its binary's modification time and size.
- The timeout is per file; a slow but working plug-in over 60 s is marked as timed out until a rescan.
- `substation-scan` must be beside the application (the build puts both in `build/bin`; `windeployqt` doesn't move it):
  without it the scan can't start, and says so.
- `PluginScanner::scan()` blocks: run it on a thread of its own (`PluginIndex` does), never on the GUI thread.

## Tests

- [test_plugin_index.cpp](../../tests/app/test_plugin_index.cpp): reading the test bundle (four plug-ins, names,
  vendors, categories, class ids) and a junk file with the real scanner; with itself as a fake scanner
  ([testing.md](../testing.md#the-fake-scanner)): caching (unchanged files not read again, changed ones are, a rescan
  reads everything), a plug-in that crashes while loading costing only itself (the files after it read by a new
  process), a hanging plug-in timing out, stray lines, cancelling; finding files (links, nested folders,
  `SUBSTATION_VST3_PATH`); the friendly messages; the index (one scan at a time, folders added and removed). The test
  plug-ins crash or hang on purpose when `SUB_TEST_PLUGIN_CRASH` or `SUB_TEST_PLUGIN_HANG` is set.
- [test_browser_controller.cpp](../../tests/app/test_browser_controller.cpp): plug-ins and their failures in the
  browser. [test_ui_dialogs.cpp](../../tests/app/test_ui_dialogs.cpp): plug-in folders in the preferences.
- `TestSupport`'s `prepareApplication()` sets `SUBSTATION_PLUGIN_CACHE` for every test program; the tests that scan set
  `SUBSTATION_VST3_PATH`.

See [testing.md](../testing.md).
