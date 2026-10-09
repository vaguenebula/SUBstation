# Browser

The browser lists built-in devices, plug-ins, presets and the audio files under the user's places, searches them as
you type, ranks what you use most first, and lists the sounds most like one (Find Similar Sounds, with the
intelligence module: [intelligence.md](intelligence.md)). It is three layers: the file index and search, a C++ backend with no Qt
in [browser/src](../browser/src) (the static library `sub_browser`, namespace `sub::browser`); the browser's logic and
models in the application layer, [app/src/browser](../app/src/browser) (`BrowserController`, `Session.browser`); and
the panel, QML in [ui/qml/browser](../ui/qml/browser). How it behaves for the user is in
[guide/browser.md](guide/browser.md).

## Overview

The backend shares nothing with the audio engine: no locks, no threads, no code. It has two threads of its own:

- the **indexer**, at background priority for CPU, disk and memory (it yields to playback and to decoding), which
  keeps a tree of the folders under the places, each with its audio files, saves it, and keeps it up to date;
- one for **searches**, which filters and orders immutable snapshots of that tree.

Neither thread calls into the application, except for the **wake callback**: when there are results, or the index
changed, the backend calls it (from one of its threads), and the application takes them on its own thread. The
backend is portable: what it needs from the operating system is the platform layer's ([platform.md](platform.md): paths
and their keys, background priority, files) and, for listing and watching folders, one header of its own,
[Platform.h](../browser/src/Platform.h), with a Win32 implementation and a POSIX one, and a folder watcher per system
(Linux watches with inotify).

The backend is held item for item to a plain, single-threaded reference kept with the tests
([tests/app/support/BrowserReference.h](../tests/app/support/BrowserReference.h)): the same files from a folder tree,
in the same order, and the same results for every query, sort, filter and use count. Searching 200 000 files takes
about 10 ms and never holds up the window or the audio; see [benchmarks/README.md](../benchmarks/README.md) for the
measurements and the profile that led to the design.

```
 application thread (Qt)                  sub::browser (C++, no Qt)
 -----------------------                  ------------------------------------------------
 BrowserController                        Browser
   | setPlaces / rebuild   ---------->      Indexer thread (background priority)
   | setItems (devices, plug-ins,  ---->      saved index  <-> browser-index.bin
   |           presets)                       FolderWatcher per place (ReadDirectoryChangesW / inotify)
   | setUsage (library.json) -------->        publishes immutable Snapshots
   | search(text, sort, ...) -------->     |
   |                                       Search thread
   |                                         runSearch(latest query, latest snapshot)
   |                                         -> Result (hits into the snapshot)
   |                                       |
   |  queued call  <------------------  wake callback (results ready / index changed)
   v
 FileIndex::take(): Browser::take() -> SearchResult -> ItemListModel (256 rows at a time)
```

## Files

### Backend (`browser/src`)

| File | What it holds |
|---|---|
| [Model.h](../browser/src/Model.h) / [Model.cpp](../browser/src/Model.cpp) | What a search reads: `FolderFiles` (one folder's file names, packed), `SnapFolder`, `Snapshot`, `ExternalItem`/`ExternalGroup` (devices, plug-ins, presets), `UsageRecord`/`Usage` (use counts and `rank()`), `Sort`, `Query` (with `score` for `Sort::Score`), `Hit`, `Result` (with `find()`), `placePrefix()`. |
| [Indexer.h](../browser/src/Indexer.h) / [Indexer.cpp](../browser/src/Indexer.cpp) | The indexer thread: the folder tree, the walk, folder times, watching, publishing snapshots, the saved index (`save()`/`load()`). Also `Limits`, `PlaceSpec`, `IndexStatus`. |
| [Search.h](../browser/src/Search.h) / [Search.cpp](../browser/src/Search.cpp) | Filtering and ordering: `runSearch()`, `matchQuality()`, `UsageCache`, `SearchInputs`. |
| [Text.h](../browser/src/Text.h) / [Text.cpp](../browser/src/Text.cpp) | Python's `str.lower()`, `str.casefold()`, `str.split()` and the regex word starts (`pyLower`, `pyCasefold`, `pySplit`, `wordStarts`), `unicodeVersion()`; on the platform layer's WTF-8 (`platform/Unicode.h`). |
| [UnicodeTables.inc](../browser/src/UnicodeTables.inc) | Tables generated from Python itself: `kLower`, `kFold`, `kSpace`, `kWord`, `kCaseIgnorable`, `kCased`, `kUnicodeVersion`. Do not edit. |
| [Browser.h](../browser/src/Browser.h) / [Browser.cpp](../browser/src/Browser.cpp) | `Browser`: owns the indexer and the search thread, the wake callback, and the hand-over of results. |
| [Platform.h](../browser/src/Platform.h) | What the index needs from the system besides the platform layer's (whose names, `NativeString`, `kSeparator`, `kCaseSensitivePaths`, `nameKey()`/`pathKey()`, `enterBackgroundMode`..., are this namespace's too: [platform.md](platform.md)): `hiddenName`, `listFolder`, `folderTime`, `Event` (auto-reset), `FolderWatcher`, `Waiter`. |
| [Platform.cpp](../browser/src/Platform.cpp) | Windows: `FindFirstFileExW`, `GetFileAttributesExW`, Win32 events, `ReadDirectoryChangesW`, `WaitForMultipleObjects`. |
| [PlatformPosix.cpp](../browser/src/PlatformPosix.cpp), [PlatformPosix.h](../browser/src/PlatformPosix.h) | Elsewhere: `opendir`/`readdir`, `stat`, a pipe as the event, `poll`. |
| [FolderWatcherInotify.cpp](../browser/src/FolderWatcherInotify.cpp), [FolderWatcherNone.cpp](../browser/src/FolderWatcherNone.cpp) | `FolderWatcher` on Linux (an inotify watch per folder), and on POSIX systems without a way to watch yet (nothing is watched). |
| [browser/tools/gen_unicode_tables.py](../browser/tools/gen_unicode_tables.py) | Writes `UnicodeTables.inc` from the running Python (a generator; not part of the build). |

The backend is built as the static library `sub_browser` ([browser/CMakeLists.txt](../browser/CMakeLists.txt)) on
`sub_platform`: the Win32 platform layer on Windows, the POSIX one elsewhere, with inotify's folder watcher on Linux.
See [building.md](building.md).

### Application layer (`app/src/browser`)

| File | What it holds |
|---|---|
| [BrowserController.h](../app/src/browser/BrowserController.h) | `BrowserController` (`Session.browser`): the sidebar's entries, search, the sort, the list shown, the folder tree of a place, preview requests, use counts, the context menus' actions, places and their settings, Find Similar. QML binds to it. |
| [FileIndex.h](../app/src/browser/FileIndex.h) | `FileIndex` (the backend on the application thread's side), `SearchResult` (a result read a page at a time), `placeSpec()`, `usageRecords()`. |
| [BrowserSearch.h](../app/src/browser/BrowserSearch.h) | What to ask the search for: the sorts (`sortOrders()`), the group numbers, `Scope`, `scopeQuery()`, `placePrefix()`, `pluginTag()`. The sort orders are documented here. |
| [BrowserItem.h](../app/src/browser/BrowserItem.h) | `BrowserItem` (and its `key()`), `ItemKind`, `builtinItems()`, `pluginItem()`. |
| [ItemListModel.h](../app/src/browser/ItemListModel.h) | The list shown: paged, draggable. |
| [SidebarModel.h](../app/src/browser/SidebarModel.h) | The sidebar as a flat list. |
| [BrowserMime.h](../app/src/browser/BrowserMime.h) | The drag formats and their readers (`pluginRefs`, `deviceKinds`, `presetPaths`, `movedDevices`). |
| [Library.h](../app/src/browser/Library.h) | `Library`: use counts kept in `library.json`, and `rank()`. |
| [PresetIndex.h](../app/src/browser/PresetIndex.h) | `PresetIndex`: the presets in the user's library as items, listed again when they change. |
| [PathKeys.h](../app/src/browser/PathKeys.h) | Paths as the browser compares them: `normalPath`, `toBackendPath`/`fromBackendPath`, `pathKey`, `caseKey`, `audioKey`, `localDataDir()`. |

The plug-ins the browser lists come from the plug-in index ([app/src/plugins](../app/src/plugins),
[app/plugin-scanner.md](app/plugin-scanner.md)).

### The panel (`ui/qml/browser`)

| File | What it holds |
|---|---|
| [BrowserPanel.qml](../ui/qml/browser/BrowserPanel.qml) | The panel: the search field and the sort, the sidebar beside the results (or a place's folder tree), the footer (preview on or off, the status); drags out of it; the context menus; Add Folder…; stopping a preview on a press outside it |
| [BrowserSidebar.qml](../ui/qml/browser/BrowserSidebar.qml) | The sidebar, over the controller's `SidebarModel` |
| [BrowserResults.qml](../ui/qml/browser/BrowserResults.qml) | The results, over the controller's `ItemListModel` through `PagedRows`; renaming a preset in place |
| [BrowserFolderTree.qml](../ui/qml/browser/BrowserFolderTree.qml) | A place's folder tree, over `FolderTreeModel` |
| [SelectionList.qml](../ui/qml/browser/SelectionList.qml), [SelectionRowArea.qml](../ui/qml/browser/SelectionRowArea.qml) | The lists' selection as `QListView`'s extended selection had it |
| [ActionMenu.qml](../ui/qml/browser/ActionMenu.qml) | A context menu from the controller's `[{action, label}]` lists |

Its C++ helpers are in [ui/src/mainwindow](../ui/src/mainwindow): `PagedRows`, `FolderTreeModel`, `OutsidePresses`.

## Key types and concepts

### Items and keys

A `BrowserItem` has a `name`, a `path`, a `kind` (`ItemKind::Audio`, `Plugin`, `Device` or `Preset`; QML sees
"audio", "plugin", "device", "preset"; for a built-in device `path` is the device kind, for a preset its file), a
`detail` (the parent folder, the plug-in's vendor, the device's category, or the device a preset is for), an optional
`PluginInfo` and a tooltip. Its `key()` says who it is, for what the browser remembers about it:

| Kind | Key |
|---|---|
| audio | `audio:` + `pathKey(path)` (`audioKey()`) |
| plugin | `plugin:<format>:<uid>` |
| device | `device:<kind>` |
| preset | `preset:<path>` (in the system's form) |

`pathKey()` is Python's `os.path.normcase(os.path.normpath(path))` (as `library.json` was written): on Windows
backslashes and Windows' own lower case (`LCMapStringEx` with the invariant locale), so names that differ only in case
have one key; **elsewhere the normalised path as it is**, since names that
differ in case are different files there ("Kick.wav" and "kick.wav" keep two keys and two use counts). The backend
makes the same keys for indexed files: a folder's key is the place's key joined with each folder name's
`platform::nameKey()`.

### Groups

Every list is a search over one or more **groups**, in order. Group 0 (`kAudioGroup`) is the index's audio files;
other numbers are external groups the controller hands over with `FileIndex::setItems(group, items)`:
`kBuiltinGroup = 1` (built-in devices), `kPluginsGroup = 2` (plug-ins) and `kPresetsGroup = 3` (presets; their native
kind is `Kind::Preset`). `scopeQuery()` turns a sidebar entry (a `Scope`: QML sees it as `[kind]` or `[kind, sub]`)
into (groups, tag, place prefix):

| Sidebar entry (scope) | Groups | Tag | Place prefix |
|---|---|---|---|
| All `["all"]` | built-in, plug-ins, presets, audio | | |
| Samples `["samples"]` | audio | | |
| Built-in, or a category `["builtin", name]` | built-in | the category | |
| Plug-ins, or *Instruments* / *Audio Effects* | plug-ins | the category (`pluginTag()`) | |
| Presets, or a device's `["presets", name]` | presets | the device's name | |
| A place `["place", path]` | audio | | `placePrefix(path)` |

An external item's tag is what `tag` filters on: a built-in device's category, a plug-in's *Instruments* or *Audio
Effects*, the device a preset is for. The place prefix (`sub::browser::placePrefix()`) is the place's path with one
trailing separator, in lower case where file names ignore case (Windows); elsewhere "Drums" and "drums" are different
folders, and the filter keeps them apart.

### Snapshots

`Snapshot` is the index at one moment, immutable and shared by `shared_ptr`, so searches never lock against the
indexer:

- `folders`: the folders that have files, in the order the walk first reached them. Each `SnapFolder` holds its
  `FolderFiles`, its path as shown (the place's root as given, then the folder names), its lower-case path (for the
  place filter where names ignore case), its key (for use counts) and its detail (the folder's name; a place's root
  shows its basename).
- `audio`: every file as (folder, file), in the list's own order: by lower-case name, then walk order (a stable sort of
  the walk by `pyLower(name)`, which keeps the walk order for equal names).
- `byFold`: positions in `audio` by casefolded name (ties in the list's own order), so ordering by name is a pass over
  it rather than a sort.
- `folderByKey`, `folderByPath`: lookups for use counts and `Result::find`.

`FolderFiles` packs a folder's names into one string with offsets: the name, its `pyLower`, its `pyCasefold` and its
key name. For ASCII names on Windows the last three share one copy.

### Results

`Result` holds its `generation`, the snapshot and external groups it points into, the `hits` (group, index) in order
and `searchMs`. The application holds it as a `SearchResult`, whose rows are only made into `BrowserItem`s when asked
for, a page at a time.

## How it works

### The indexer thread

`Indexer::run()` first enters background mode (`platform::enterBackgroundMode()`: on Windows
`THREAD_MODE_BACKGROUND_BEGIN`, CPU, I/O and memory priority; on Linux the lowest nice value and the idle I/O class,
for this thread only), then loads the saved index, then loops:

1. Take commands under the lock: new places (`setPlaces`), a rescan request, stop.
2. Apply them. `applyPlaces()` keeps what is known of a place that stays (its node and its watcher), reuses a known
   node for a new place whose key is already in the tree (a place inside another), and starts a `FolderWatcher` for
   each new place; places that went lose their watchers. A rescan marks every folder dirty.
3. If something must be looked at, run `updatePass()`: publish what is known at once if the places changed (the saved
   index shows immediately), then walk.
4. Otherwise wait with a `platform::Waiter` for the wake event, a place's watcher, or a deadline (changes settling,
   the next save; at least every 60 s).

**The walk** matches the reference's exactly, so the lists are the same: from each place, depth first, the
last folder first (a stack), at most 16 folders deep (`Limits::maxDepth`), and no further folders once 300 000 files
(`maxFiles`) were found under a place; names starting with `.` or `$` are skipped. On Windows junctions are walked
into, symbolic links to folders are not (`listFolder` reads `FILE_ATTRIBUTE_REPARSE_POINT` and
`IO_REPARSE_TAG_SYMLINK`, as `DirEntry.is_dir(follow_symlinks=False)` does); elsewhere only real directories are, not
symbolic links to them (a link to a file is listed as a file). Entries come in the order the file system lists them,
as `os.scandir` gave them, and only names ending in one of the audio extensions (`FileIndex::audioExtensions()`:
`.wav`, `.wave`, `.flac`, `.mp3`) are kept.

During a pass a folder is listed again if it was never listed, is marked dirty, or its last-write time
(`folderTime`; through junctions and links) differs from the one saved when it was listed. Each folder's time is
compared once per check round (`round_`). After the pass, folders no longer reachable from any place are dropped
(`removeUnreachable`), a snapshot is published if anything changed, and a save is scheduled for 5 s later.

The tree is shared between places: nodes are found by key (`byKey_`), so a folder under two overlapping places is one
node, and a snapshot lists it once.

**Commands interrupt passes.** `setPlaces()` and `rescan()` set an atomic `interrupt_` that the pass checks between
folders; the pass returns, the loop takes the commands, and walks again. `IndexStatus::busy` is true while commands
asked for are not finished.

**Publishing while scanning.** During a long scan `publishWhileScanning()` publishes a snapshot 30 ms after the start
if no files were shown yet, then every 150 ms or four times as long as building the last snapshot took, whichever is
more, and only when new files were found. So results show while a first scan is still going.

**Watching.** Each place has a `FolderWatcher`:

- On Windows, `ReadDirectoryChangesW` on the place's root, recursive, for file and folder names (added, removed,
  renamed), into a 64 KB buffer, overlapped.
- On Linux, an inotify watch on each folder in the tree (`IN_CREATE`, `IN_DELETE`, `IN_MOVED_FROM`, `IN_MOVED_TO`, and
  the folder itself going), except hidden ones (never listed), kept up as folders come and go: a folder created or
  moved in is watched with everything below it, one deleted or moved out is let go. Folders the system won't watch
  any more of (`fs.inotify.max_user_watches`) are left out: their changes show on a rescan or the next start. Other
  POSIX systems don't watch; changes show on a rescan or the next start.

When a watcher fires, `markChanged()` marks the folder the change was in dirty, or the nearest folder above it that is
known (new folders are found by listing that one); changes in hidden folders are ignored. Changes settle for 250 ms
after the last one (at most 1 s after the first) before a pass. If the watcher overflowed (too much changed at once:
`Changes::Overflow`), every folder's time is compared again (`++round_`). If a watcher fails (its root went), it is
dropped and `rewatch()` tries again after the next pass, once the place's root exists.

**Waiting.** `platform::Waiter` waits for any of the wake event's and the watchers' handles, or a timeout: on Windows
`WaitForMultipleObjects` on events, which takes at most 64 handles (`kMaxHandles`); elsewhere `poll` on file
descriptors (the event is a pipe, a watcher its inotify descriptor), up to 4096.

### The search thread

`Browser::search()` takes the next generation (`latest_`, an atomic counter), replaces any waiting query with it,
clears any finished result and wakes the search thread. A running search sees the counter move and stops: the filters
and sorts look at it every 4096 items (`kCheckEvery`). The thread then runs the newest query over the newest snapshot
(`indexer_.snapshot()`), the external groups and the use counts as they were when it started.

A finished result is kept only if it is still the latest; then the wake callback is called. `take()` hands it out
once, and only if its generation is still the latest, so the application never shows the results of a search it has
replaced.

### The wake callback

`Browser::setWakeCallback(std::function<void()>)` takes the place of a platform event object, so the backend needs
nothing of the platform's event loop. The callback is called from the browser's threads when there is something
to `take()` (the latest search's results, or a change of the index or its status), once until the next `take()` (as a
set event stays set). It must only hand the work over, never call back into the browser: `FileIndex` posts a queued
call to its own thread (`QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)`), which calls `take()`. Once
`setWakeCallback()` returned the previous callback is no longer called; if something waits to be taken already, the
new callback is called at once; after `close()` no calls come.

### Matching and ordering

Matching and ordering are defined by the reference's `find()` (the rules are in
[BrowserSearch.h](../app/src/browser/BrowserSearch.h)):

- **Terms**: `pySplit(pyLower(text))`, as `query.lower().split()`.
- **Match**: every term is in the item's lower-case name or its lower-case detail. For files the detail is the
  folder's, so whether a term is in it is worked out once per folder.
- **Place filter**: the folder's path (in lower case where names ignore case) plus the separator starts with the
  prefix.
- **Tag filter**: external items with that tag only.
- **Rank** (`sortByRank`): items with a use rank above 0 first, by rank, then match quality (stable); then the unused
  ones by match quality, best first, keeping the list's own order (a counting sort over quality buckets); items with a
  negative rank last. With no terms every quality is 0, so only the used items move.
- **Score** (`sortByScore`, the "similar" sort): indexed files by `Query::score(path)` (the path as shown,
  `Snapshot::join` of the folder's and the name), the highest first, stable over the list's own order; files it gives
  NaN, and items other than indexed files, are left out. The score function is the application's (a finished
  similarity result's, which it keeps alive), called on the search thread. Filtering by terms and places comes first,
  as for the other sorts.
- **Name** (`sortByName`): by casefolded name, stable over the list's own order. Each group is ordered and the groups
  are merged, which is the same as a stable sort of the whole list. For files the snapshot's `byFold` is already in
  that order.

The list's own order is the groups' order (built-in devices, plug-ins as the scan found them, presets by the device
they are for and then by name, then samples by name).

**Match quality** (`matchQuality`, as the reference's `match_quality`): 8 if the name's stem (an audio file's name
without its extension) equals the terms joined by spaces; then for each term, 3 if the name starts with it, else 2 if
a word in the name starts with it, else 1 if it is anywhere in the name. Word starts are where
`re.finditer(r"(?:^|[\s_\-.()\[\]])(\w)", name)` finds its group (`wordStarts`).

**Use rank** (`Usage::rank`, as `Library::rank`): `score * 0.5 ** (days since last use / 30)`, 0 when there is no
score. For files, `resolveAudioUsage()` matches each `audio:` record to a file through its folder's key and the file's
key name (a record for a drive's root is looked up with its trailing `\`); the result is cached in `UsageCache` for as
long as the snapshot and the use counts stay the same.

### Text: Python's rules, from Python's tables

[Text.h](../browser/src/Text.h) implements `str.lower()`, `str.casefold()`, `str.split()` and the regex classes `\s`
and `\w` from tables generated out of Python itself, so the backend lowers, folds and splits every character as Python
does:

- `kLower` and `kFold`: each character's mapping where it changes (up to three code points). Capital sigma (U+03A3) is
  lowered by its context (the Final_Sigma rule, `isFinalSigma`), which needs Python's "case-ignorable" and "cased"
  properties; Python doesn't expose them, so the generator reads them back by lowering probe strings
  (`kCaseIgnorable`, `kCased`).
- `kSpace`: `str.isspace()`, what `split()` and `\s` split on. `kWord`: `str.isalnum()` or `_`.
- ASCII is handled by a fast path and a 128-entry class table.

Strings are **WTF-8**: UTF-8 that may also hold unpaired surrogates, which Windows file names can contain, so such
names survive the trip to UTF-16 and back. Byte order is code point order, so comparing bytes compares strings as
Python did. How file names compare in keys is the platform's (`nameKey()`), not these tables'.

[browser/tools/gen_unicode_tables.py](../browser/tools/gen_unicode_tables.py) writes the tables; it is the one piece
of the browser still in Python, a generator run by hand, not part of the build. To follow a newer Unicode version,
run it with that Python:

```sh
python browser/tools/gen_unicode_tables.py
```

The header of `UnicodeTables.inc` records the Python and Unicode versions it was made with (currently Python 3.12.10,
Unicode 15.0.0); `unicodeVersion()` returns the latter.

### The saved index

The index is saved to `browser-index.bin` in `localDataDir()` (`%LOCALAPPDATA%\SUBstation` on Windows,
`~/.local/share/SUBstation` elsewhere: `FileIndex::defaultIndexPath()`; the environment variable
`SUBSTATION_BROWSER_INDEX` overrides it, as the tests do). An empty store path means nothing is saved. It is written 5 s
after a pass that changed something, and when the backend closes; it goes to `browser-index.bin.tmp` first and is
moved over the old one (`platform::writeFileAtomically`, which replaces it with `MoveFileExW` and
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` on Windows, `rename` elsewhere).

Format (little-endian, as written by the platform layer's `ByteWriter`, `platform/Bytes.h`):

```
"GILBIDX1"                     8 bytes magic
u32 format                     1 (kFormat)
u32 n, n x str                 the extensions it was made with
u32 count                      folders
count x {
  u32 parent                   index of the parent, or 0xFFFFFFFF
  str path, str name, str key, str detail
  u64 time                     last-write time when listed
  u8  listed                   0 if changes were pending: list it next time
  u32 n, n x u32               children
  u32 n, n x str               file names, in listing order
}
u64 FNV-1a                     of everything before it
str = u32 length + UTF-8 (WTF-8) bytes
```

Anything unexpected (a different magic or format, another list of extensions, a bad checksum, a truncated file,
indexes out of range) and the file is ignored: the folders are listed again. On load every folder's time is compared
with the disk, so on the next start the saved index shows at once and only folders that changed are listed. A file
added in a way that doesn't change the folder's time (on drives that don't report it) is only found by *Rescan*,
which lists every folder again.

## The application layer

### FileIndex

[FileIndex](../app/src/browser/FileIndex.h) makes the backend with `defaultIndexPath()`, `audioExtensions()`,
`kMaxFiles = 300000` and `kMaxDepth = 16`, and sets its wake callback to a queued `take()`. `take()` calls
`Browser::take()`, emits `results(SearchResult)` when there is one, and `updated` when indexing started or stopped or
the index version changed. It converts between the application's paths (Qt's form, `QString`) and the backend's
(the system's form, UTF-8): `toBackendPath()`, `fromBackendPath()`. `placeSpec(root)` gives a place as the backend
takes it (its root as given, its `pathKey()`, its basename); `usageRecords(library)` the use counts as `(key, score,
last used or NaN)`. `close()` stops the backend's threads, saving the index.

### BrowserController

[BrowserController](../app/src/browser/BrowserController.h) is what the panel binds to (`Session.browser`). Every
list is a search:

- `searchText` set (as the user types) starts a zero-interval timer, which only merges changes that come together;
  there is no typing delay, since searching doesn't hold up the UI. `refresh()` calls `FileIndex::search()` with the
  text, the `sort`, the library's clock and the scope's groups, tag and prefix. `searching` is true while results are
  on their way.
- A place (`scope` `["place", path]`) with no search text shows its folder tree instead: `showingTree`, `treeRoot`,
  and a `QFileSystemModel` (`folderModel`, made when first wanted) from `treeRootIndex`. Results that arrive after the
  tree was shown are dropped.
- When the index or the plug-ins change, the list is searched again keeping its place: the controller remembers the
  current item and the top row (`setTopRow()`) and restores them when the results come (`SearchResult::find()`,
  `positionRestored(currentRow, topRow)`), if the item is within the first 5000 rows (`kKeepWithin`).
- `selectFirstResult()` (Enter or Down in the search field) selects the first result, once the results are there if
  they are still on their way (`selectRowRequested`); `activate(row)` (Enter on a result, a double-click) adds it:
  `fileActivated(path)`, `deviceActivated(kind)`, `pluginActivated(ref)` or `presetActivated(path)`, which the session
  turns into adding it to the selected track ([app/session.md](app/session.md)).
- `focusSearch()` (Ctrl+F) shows *All* and asks the panel to focus the search field (`searchFocusRequested`).
- `statusText` is the footer: how many items the list has ("No presets yet…", "No VST3 plug-ins found"), whether
  indexing or a plug-in scan runs, the scan's progress, how many plug-in files could not be read.

**Paging.** Results cross into the model a page at a time: [ItemListModel](../app/src/browser/ItemListModel.h) shows
the first 256 rows (`kPage`) at once and more through Qt's `canFetchMore`/`fetchMore` as the view scrolls near the end;
`ensureRows()` fetches up to a row when it has to (restoring a position). A view lays out every row it has, so a list
of every file would cost the UI thread that much each time it changed. QML views fetch every page as soon as they can,
so the panel puts [PagedRows](../ui/src/mainwindow/PagedRows.h) (an identity proxy that only fetches when the view asks,
near its end) between the list and the model. `SearchResult::items()` makes rows into `BrowserItem`s: files are made
fresh, other items are the ones handed over with `setItems()`, by key.

`ItemListModel` roles: `name`, `path`, `kind`, `detail`, `key`, `display` (the name, and for a plug-in or a preset its
detail: "Name   (Vendor)"), `toolTip` (the item's tooltip or path, and how often it was used), `icon` ("waveform",
"plugin" or "preset"), `uses`, `instrument`, `plugin` (a plug-in's `PluginRef` fields). `get(row)` returns every role
of a row by name.

[SidebarModel](../app/src/browser/SidebarModel.h) is the sidebar as a flat list: *CATEGORIES* (All, Samples, Built-in
and its categories, Plug-ins with Instruments and Audio Effects, Presets with a sub-entry per device they are for) and
*PLACES* (each place, Add Folder…). Roles: `title`, `scope`, `section` (a heading), `depth` (1 for a sub-entry), `icon`,
`toolTip` (the Plug-ins entry's lists the files that could not be read), `dim` (headings, Add Folder…), `selectable`.

**Use counts.** An item counts as used when it is added to the project from the browser: double-click, Enter, or a
drag that is dropped somewhere (the panel calls `dropped(rows)`, or `droppedFiles(paths)` from the folder tree, only
when the drop was accepted). [Library](../app/src/browser/Library.h)`::recordUse()` adds 1 to `uses`, sets `score` to
the current rank plus 1 and `last_used` to now, and saves; the controller then hands the records to the backend
(`FileIndex::setUsage`). The list is not re-sorted then, so the selection stays put. `library.json` (in
`localDataDir()`, or `SUBSTATION_LIBRARY`) is `{"version": 1, "items": {key: record}}`; records are plain JSON objects
and fields this version doesn't know are kept. A file that can't be read or has another version counts as empty; a
failed save is ignored. An item's tooltip shows how often it was used.

**Drags.** `dragData(rows)` gives what a drag of these rows carries, as `{mime type: text}` for QML's `Drag.mimeData`
([BrowserMime.h](../app/src/browser/BrowserMime.h)):

| Type | What | Reader |
|---|---|---|
| `text/uri-list` | audio files, as file URLs | `QMimeData::urls()` |
| `application/x-substation-plugin` | a JSON list of plug-ins (`format`, `uid`, `name`, `vendor`, `path`, `instrument`) | `pluginRefs()` |
| `application/x-substation-device` | a JSON list of built-in device kinds | `deviceKinds()` |
| `application/x-substation-preset` | a JSON list of preset files | `presetPaths()` |
| `application/x-substation-device-move` | not the browser's: devices dragged from a track's chain in the device view, as plain text: the track's id, then the devices' ids, a line each | `movedDevices()` (written by `movedDevicesData()`) |

The arrangement and the device view read them on a drop ([ui/arrangement.md](ui/arrangement.md#drag-and-drop),
[ui/device-view.md](ui/device-view.md#dragging-and-dropping)).

**Plug-ins.** The [PluginIndex](../app/src/plugins/PluginIndex.h) scans on a thread of its own, at start-up and on
*Rescan Plug-ins*; a scan asked for while one runs is run after it. The controller lists its plug-ins in the
plug-ins group as they are found, and its failures in the Plug-ins entry's tooltip. See
[app/plugin-scanner.md](app/plugin-scanner.md).

**Presets.** [PresetIndex](../app/src/browser/PresetIndex.h) lists the user's preset library
([io/Presets.h](../app/src/io/Presets.h)) on the application thread (it is small): at start, when the device view
saves a preset (`DeviceSelection::presetSaved` → `Session::presetSaved` → `rescan()`), after a rename or delete from
the list's menu (`presetsChanged`), and when the library's folders change (a `QFileSystemWatcher` on the library and its
folders, merged by a 200 ms timer, `kSettleMs`). The session hands its items to the controller
(`setPresets(items, groups, root)`), which gives them to the backend as the presets group, tagged with the device they
are for; when the groups change the sidebar is made again (*Presets* has an entry per device). Presets straight in the
library folder are listed under "Other". A result's menu (`resultActions(row)`) offers Rename… (`renamePreset(path,
name)`: the new path, or "" with a status message), Delete (`deletePreset(path)`: the file moves to the system's trash,
`QFile::moveToTrash`) and Show in Folder.

**Find Similar.** With a sound similarity (`Options::similarity`, the session's `SoundSimilarity`), the controller
points it at its index (`setLibrary`: the sounds analysed are the index's files) and offers *Find Similar Sounds* in an
audio file's menu (`resultActions`; an audio clip's in the arrangement calls the same). `findSimilar(path, start,
length)` asks the sound similarity for the sounds most like the file, or its part; the list goes to Samples unless a
place is shown, the search text is cleared, and `sort` becomes "similar" (`sorts` lists *Similarity* first while it
lasts; the sort chosen before is what is saved, and comes back). `similarTo` and `similarName` say like what, for the
panel's bar. While the result is on its way the list is `searching`; when it comes (`SoundSimilarity::found`, the
latest search's only), the list is searched with its `scorer()` (`FileIndex::search(..., "similar", ..., score)`).
Text and places filter it as any list; a place shows its files, not its tree. While the library is still being
analysed, the sound is searched again every 2 s (`kRefineMs`) as more sounds are analysed, keeping the list's place,
and the status says how far the analysis got ("12 sounds like Kick.wav (analysing 1200 of 5091…)"). Choosing *Rank* or
*Name*, a sidebar entry that isn't files, Ctrl+F, or `clearSimilar()` ends it. A sound that can't be analysed says why
in the status line (`statusMessage`). When the controller shuts down it lets go of the sound similarity before its
index closes.

**Preview.** Setting `currentRow` to an audio file (in the list or the tree: `treeCurrentChanged(path)`) previews it
(`previewRequested(path)`, which the session sends to `EngineBridge::previewFile`) while `previewEnabled` is on;
`previewing` is true from then until it stops. An audio file the user chooses (a row clicked and let go of without a
drag, or reached with the arrow keys: the lists' `chosen(row)`, then `choose(row)` or `chooseFile(path)`) is said on
`fileChosen(path)`: a hot swap swaps it in (`HotSwap`, [app/session.md](app/session.md#the-file-manager-and-hot-swaps-filemanager-hotswap)),
and the session gives what is activated meanwhile to the hot swap instead of the arrangement. The current item
changing otherwise (a press that becomes a drag, a list searched again) previews but is no choice. The panel stops it on a press anywhere outside the browser
(`stopPreview()`). See [app/engine-bridge.md](app/engine-bridge.md).

**Places and settings.** The places and the sort are kept in `QSettings` (`browser/places`, `browser/sort`). The
first start has the user's Music folder (or the home folder) as its place. `addPlace(folder)` indexes only the new
place; `removePlace(place)` drops it; `rescan()` calls `FileIndex::rebuild()`, which sets the places and asks for a
rescan. A sidebar entry's menu (`sidebarActions(scope)`) offers Remove from Places (a place), Rescan Plug-ins
(Plug-ins), Show in Folder (Presets), then Add Folder… and Rescan. The session calls `shutdown()` when the
application ends, which stops the backend's threads (saving the index) and waits for a plug-in scan.

## The panel

[BrowserPanel.qml](../ui/qml/browser/BrowserPanel.qml) on `Session.browser`:

- **The search field and the sort.** Typing sets `searchText`; Enter or Down selects the first result; a ✕ clears it.
  The sort is a `ChoiceBox` of `sorts` (Rank, Name; Similarity while the list shows similar sounds).
- **The similar bar** (`similarBar`), under them while `similarTo` is set: "Similar to *name*" (the path as its
  tooltip) and a ✕ (`clearSimilar()`). The main window shows the browser when Find Similar starts (from a clip's menu
  with the browser hidden).
- **The hot-swap bar** (`hotSwapBar`), over the similar bar while `Session.hotSwap.active`: "Hot-Swap *name*
  (*uses*)", its Similar (`HotSwap::findSimilar`: the sounds like the one swapped in now) and a ✕ (`stop()`). Esc
  with the list (or the tree) having the keyboard ends the hot swap too; a hot swap starting gives it the keyboard.
  A press outside the panel ends it (an `OutsidePresses`, but in `hotSwapKeepers`: the main window gives the File
  Manager and the transport bar), and so does a drag starting from the panel (`startDrag`).
- **The sidebar** ([BrowserSidebar.qml](../ui/qml/browser/BrowserSidebar.qml)) in a `SplitView` beside the results: a
  click sets `scope`; Add Folder… asks for a folder (`FolderDialog`, `addPlaceRequested`); a right-click opens the
  entry's `ActionMenu`.
- **The results** ([BrowserResults.qml](../ui/qml/browser/BrowserResults.qml)) or, while `showingTree`, the folder tree
  ([BrowserFolderTree.qml](../ui/qml/browser/BrowserFolderTree.qml) over a
  [FolderTreeModel](../ui/src/mainwindow/FolderTreeModel.h): the `QFileSystemModel` under the place flattened into rows
  with their depth, folders opened with their arrow, a double-click, or Right and Left, since Qt 6.4's `TreeView` has
  no root index). Both are [SelectionList](../ui/qml/browser/SelectionList.qml)s: a click selects a row, Ctrl toggles
  one, Shift a range from the last one clicked; a press on one of several selected rows keeps them all (a drag takes
  them all) and selects only it on release; Up/Down (Shift extends), Page Up/Down and End move the current row;
  Return/Enter and a double-click activate it; a right-click selects it and asks for its menu. Keys the list doesn't
  take (Home, Delete, letters) are the window's shortcuts. The current row is the controller's (`currentRow`).
- **Drags out**: dragging further than the platform's drag distance starts a drag of the selected rows with the
  controller's `dragData()` (the tree's: its files' URLs) and the item's icon; when it ends with a drop, the panel
  counts the items as used.
- **Renaming a preset** in place (Ctrl+R with the list focused, or its menu's Rename…): a text field over its row;
  Enter applies (`renamePreset`), Esc cancels. Deleting asks first ("Move the preset to the Recycle Bin?").
- **The footer**: the headphones (`previewEnabled`) and the status.
- **Text**: the sidebar's entries, the results and the folder tree in `Theme.listFont` (10 pt, a point more than the
  rest of the UI: Qt Quick draws small text smaller than the widgets did), the sidebar's headings in
  `Theme.listHeadingFont` (8.5 pt bold); a row is its text's height and 4 px (at least 20 px, 22 in the sidebar).
- **Stopping a preview**: an [OutsidePresses](../ui/src/mainwindow/OutsidePresses.h) on the panel, enabled while
  `previewing`, sees a press anywhere outside the panel (in its window or another of the application's) and calls
  `stopPreview()`; presses while the panel's own menus are open count as inside.

What the panel is to the main window: `focusSearch()`, `startRename(path)`, `listFocused` (the results have the
keyboard: Ctrl+R renames the preset there).

## The backend's API

| Name | What it is |
|---|---|
| `Browser(store, limits)` | Starts the backend. `store`: where the index is saved (UTF-8, `""` for nowhere); `Limits`: `maxFiles` (300 000 per place), `maxDepth` (16), `extensions` (lower case, with the dot). |
| `setWakeCallback(fn)` | [Above](#the-wake-callback). |
| `setPlaces(places)` | `PlaceSpec`s: `root` (as given), `key` (`pathKey(root)`), `detail` (its basename). |
| `rescan()` | List every folder again. |
| `setExternal(group, items)` | A group of other items, `ExternalItem`s: kind, name, path, detail, key, tag. |
| `setUsage(records, halfLifeDays)` | Use counts as `UsageRecord`s (key, score, last used; NaN when unknown). |
| `search(query)` | Starts a search (a `Query`: text, sort (`Sort::Rank`, `Sort::Name` or `Sort::Score` with its `score` function), now, groups, tag, place prefix), replacing any that runs; returns its generation. |
| `snapshot()` | The index as it is now (the latest snapshot; null before the first), from any thread: what the sound similarity's library is made from. |
| `take()` | An `Update`: the `IndexStatus` (busy, version, files, folders, and timings for benchmarks: `loadMs`, `buildMs`, `passMs`, `listed`, `checked`) and the latest search's `Result` if it finished since. |
| `status()`, `searching()` | The index's status; whether a search is waiting or running. |
| `waitIdle(seconds)` | Waits until the index settled and no search runs; false on timeout (tests, benchmarks). |
| `close()` | Stops the threads, saving the index; no wake calls after it. |
| `Result::find(kind, identity)` | Row of an item (an indexed file by path, others by key), or -1. |

Item kinds are numbered alike in `sub::browser::Kind` and `sub::app::ItemKind` (audio 0, plugin 1, device 2, preset 3).

## Invariants

- The backend's threads never call into the application (but the wake callback, which only posts) or the engine, and
  never wait on the application's thread.
- Searches read only immutable, shared data: snapshots, external groups and use counts are replaced, never changed.
  `setExternal` and `setUsage` build new objects and swap them in under the lock.
- Only the latest search's results are handed out, and only once.
- The index lists what the reference's walk lists, in the same order, and the search orders as the reference's `find()`
  does, for every character. A change to either has to change the reference
  ([BrowserReference.h](../tests/app/support/BrowserReference.h)) too, or be a deliberate break of that parity.

## Extending it

- **A new sort order**: add it to `Sort` in `Model.h`, implement it in `Search.cpp`, and add it to `sortOrders()` and
  the name-to-`Sort` conversion in [BrowserSearch.h](../app/src/browser/BrowserSearch.h) /
  [FileIndex.cpp](../app/src/browser/FileIndex.cpp).
- **A new kind of item to list** (as presets are): give it a group number in `BrowserSearch.h`, hand its items over
  with `FileIndex::setItems(group, {(item, tag), ...})`, and add a scope to `scopeQuery()` and the sidebar. A new kind
  goes into `Kind` and `ItemKind` together.
- **New filters** (items hidden from search) belong in `scopeQuery()` and the native `Query`; `Library` keeps
  unknown fields so later versions can store what they need per item.
- **Another order by something worked out elsewhere** (similar sounds are one: tempo or key could be others): a
  `Query::score` function and `Sort::Score`, as the "similar" sort does; the backend needs nothing new.
- **Another audio extension**: add it to `FileIndex::audioExtensions()`. The saved index records the extensions it was
  made with, so the next start lists everything again.
- **Changing the saved format**: bump `kFormat` in `Indexer.cpp`; old files are then ignored.
- **A newer Unicode version**: re-run `gen_unicode_tables.py` with a newer Python and rebuild.
- **Another platform**: implement [Platform.h](../browser/src/Platform.h) (listing, events, waiting) and a
  `FolderWatcher` of its own (FSEvents on macOS, say) beside `FolderWatcherInotify.cpp`, and the platform layer
  ([platform.md](platform.md)); nothing else in the backend is platform-specific.

## Gotchas

- On Windows the indexer waits on its wake event and the places' watchers in one `WaitForMultipleObjects`, which takes
  at most 64 handles: only the first 63 places are watched for changes. The others are still checked by folder time on
  the next pass.
- On Linux each folder under a place takes an inotify watch; past the system's limit (`fs.inotify.max_user_watches`)
  the rest aren't watched, and their changes show on a rescan or the next start.
- Drives that don't report changes (some network drives) are only re-read by folder times or *Rescan*.
- A saved index written a moment before is sometimes slow to read on Windows (0.5–1 s), apparently antivirus scanning
  the new file; see the benchmarks' *Variance* note.
- On Windows file system calls are the wide (`W`) ones and paths cross as WTF-8; never convert through the ANSI code
  page.
- `Result::find()` for an indexed file compares paths as shown (the place's root as given), not keys.
- The wake callback runs on the browser's threads: it must not touch Qt objects of the application's thread, only post
  to them.

## Tests

- [tests/app/test_browser_native.cpp](../tests/app/test_browser_native.cpp): the backend against the reference.
  `str.lower` and `casefold` of every Unicode character, the final sigma, `split` and word starts on every short
  string from pools of awkward characters (checked against hashes Python 3.12 computed; skipped if the tables were made
  with another Unicode version), keys as `os.path.normcase` (case-sensitive off Windows), match quality; random queries,
  sorts, tags, places and use counts ordered as Python orders them; the files of a folder tree (hidden names, depth and
  file limits, junctions and symbolic links on Windows, symbolic links elsewhere, overlapping and missing places); the
  saved index (checked by folder times and not listed again, files changed while closed, rescan, damaged files
  ignored); changes seen while running (Windows and Linux); places changing incrementally; only the latest search's
  results handed out; paging; the application woken from the backend's threads.
- [tests/app/test_browser_search.cpp](../tests/app/test_browser_search.cpp): keys, use counts decaying and persisting,
  unknown fields kept, a bad `library.json` ignored, the records the backend gets, match quality preferring name starts,
  rank putting used items first, the sidebar entries' queries.
- [tests/app/test_browser_native.cpp](../tests/app/test_browser_native.cpp) also holds `Sort::Score` to the
  reference's `findScored()`: random scores (with ties and files without one), search text and places.
- [tests/app/test_sound_similarity.cpp](../tests/app/test_sound_similarity.cpp): Find Similar in the controller, with
  a real sound similarity ([intelligence.md](intelligence.md#tests)).
- [tests/app/test_browser_controller.cpp](../tests/app/test_browser_controller.cpp): the controller and its models:
  places and their settings, searching as you type, the sort, the sidebar, the folder tree, activation and drops and
  their use counts, preview requests, Enter selecting the first result, keeping the list's place when the index
  changes, plug-ins and their failures, presets, what a drag carries.
- [tests/app/test_ui_browser.cpp](../tests/app/test_ui_browser.cpp): the panel on a real session: the sidebar,
  searching, Enter and Down from the search field, previews stopped by a press outside, activating results, the
  selection and what a drag carries, keeping the list's place, paging, the sort, used items ranking first, a place's
  folder tree, the context menus, adding places, renaming and deleting presets.
- [tests/app/test_session_devices.cpp](../tests/app/test_session_devices.cpp): presets saved from the device view listed
  in the browser, renamed there, and the preset index watching the library.

The benchmark [benchmarks/browser_backend_bench.cpp](../benchmarks/browser_backend_bench.cpp) measures the backend on a
large synthetic library (indexing, starting from the saved index, searches) and checks every query against the
reference; see [benchmarks/README.md](../benchmarks/README.md) and [testing.md](testing.md).
