# Browser

The browser lists built-in devices, plug-ins and the audio files under the user's places, searches them as you type,
and ranks what you use most first. Its file index and search are a native C++ backend in
[browser/src](../browser/src) (module `substation._browser`); the panel, the paged list model and the use counts are
Python in [src/substation/ui/browser](../src/substation/ui/browser). How it behaves for the user is in
[guide/browser.md](guide/browser.md).

## Overview

The backend shares nothing with the audio engine: no locks, no threads, no code. It has two threads of its own:

- the **indexer**, at Windows' background priority for CPU, disk and memory (it yields to playback and to decoding),
  which keeps a tree of the folders under the places, each with its audio files, saves it, and keeps it up to date;
- one for **searches**, which filters and orders immutable snapshots of that tree.

Neither thread ever calls into Python. When there are results, or the index changed, the backend sets a Win32 event;
a `QWinEventNotifier` wakes the UI thread, which takes them. Every call from Python releases the GIL.

The backend replaced Python code that did the same on the UI thread (and a walking thread that held the GIL). That
code is kept unchanged in [tests/browser_reference.py](../tests/browser_reference.py), and the native backend is held
to it item for item: the same files from a folder tree, in the same order, and the same results for every query,
sort, filter and use count. Searching 200 000 files takes about 10 ms and never holds up the window or the audio;
see [benchmarks/README.md](../benchmarks/README.md) for the measurements and the profile that led to the design.

```
 UI thread (Python)                       substation._browser (C++)
 ------------------                       ------------------------------------------------
 BrowserPanel                             Browser
   | set_places / rescan  -------------->   Indexer thread (background priority)
   | set_items (devices, plug-ins) ----->     saved index  <-> browser-index.bin
   | set_usage (library.json) ---------->     ReadDirectoryChangesW per place
   | search(text, sort, ...) ----------->     publishes immutable Snapshots
   |                                       |
   |                                       Search thread
   |                                         runSearch(latest query, latest snapshot)
   |                                         -> Result (hits into the snapshot)
   |                                       |
   |  QWinEventNotifier <---- Win32 event set (results ready / index changed)
   v
 FileIndex._take(): native.take() -> SearchResult -> ItemListModel (256 rows at a time)
```

## Files

### Native backend (`browser/src`)

| File | What it holds |
|---|---|
| [Model.h](../browser/src/Model.h) / [Model.cpp](../browser/src/Model.cpp) | What a search reads: `FolderFiles` (one folder's file names, packed), `SnapFolder`, `Snapshot`, `ExternalItem`/`ExternalGroup` (devices, plug-ins), `UsageRecord`/`Usage` (use counts and `rank()`), `Query`, `Hit`, `Result` (with `find()`). |
| [Indexer.h](../browser/src/Indexer.h) / [Indexer.cpp](../browser/src/Indexer.cpp) | The indexer thread: the folder tree, the walk, folder times, watching, publishing snapshots, the saved index (`save()`/`load()`). Also `Limits`, `PlaceSpec`, `IndexStatus`. |
| [Search.h](../browser/src/Search.h) / [Search.cpp](../browser/src/Search.cpp) | Filtering and ordering: `runSearch()`, `matchQuality()`, `UsageCache`, `SearchInputs`. |
| [Text.h](../browser/src/Text.h) / [Text.cpp](../browser/src/Text.cpp) | Python's `str.lower()`, `str.casefold()`, `str.split()` and the regex word starts (`pyLower`, `pyCasefold`, `pySplit`, `wordStarts`), WTF-8 conversion (`toUtf8`, `toWide`), and Windows' own lower case (`ntLower`). |
| [UnicodeTables.inc](../browser/src/UnicodeTables.inc) | Tables generated from Python itself: `kLower`, `kFold`, `kSpace`, `kWord`, `kCaseIgnorable`, `kCased`, `kUnicodeVersion`. Do not edit. |
| [Browser.h](../browser/src/Browser.h) / [Browser.cpp](../browser/src/Browser.cpp) | `Browser`: owns the indexer and the search thread, the event that tells the UI there is something to take, and the hand-over of results. |
| [Platform.h](../browser/src/Platform.h) / [Platform.cpp](../browser/src/Platform.cpp) | Win32: `listFolder`, `folderTime`, `enterBackgroundMode`, `Event` (auto-reset), `FolderWatcher` (`ReadDirectoryChangesW`). |
| [bindings.cpp](../browser/src/bindings.cpp) | The nanobind module `substation._browser`. |
| [browser/tools/gen_unicode_tables.py](../browser/tools/gen_unicode_tables.py) | Writes `UnicodeTables.inc` from the running Python. |

The backend is built as the static library `sub_browser` and the module `_browser`; see
[building.md](building.md#the-two-native-modules).

### Python side (`src/substation/ui/browser`)

| File | What it holds |
|---|---|
| [browser_panel.py](../src/substation/ui/browser/browser_panel.py) | `BrowserPanel`: the sidebar (categories and places), search field, sort list, result list, the folder tree, preview, Enter/Down handling, use counting. |
| [file_index.py](../src/substation/ui/browser/file_index.py) | `FileIndex` (the native backend on the UI thread's side), `SearchResult` (a result read a page at a time), `PluginIndex` (the background VST3 scan), `index_path()`, `place_spec()`, `usage_records()`. |
| [browser_models.py](../src/substation/ui/browser/browser_models.py) | `BrowserItem` (and its `key`), `ItemListModel` (paged, draggable), the drag MIME types and their readers (`plugin_refs`, `device_kinds`). |
| [library.py](../src/substation/ui/browser/library.py) | `Library`: use counts kept in `library.json`, and `rank()`. |
| [search.py](../src/substation/ui/browser/search.py) | What to ask the search for: `SORTS`, the group numbers (`AUDIO`, `BUILTIN`, `PLUGINS`), `scope_query()`, `place_prefix()`, `plugin_tag()`. The sort orders are documented here. |

## Key types and concepts

### Items and keys

A `BrowserItem` (frozen dataclass) has a `name`, a `path`, a `kind` (`"audio"`, `"plugin"` or `"device"`; for a built-in
device `path` is the device kind), a `detail` (the parent folder, the plug-in's vendor, or the device's category), an
optional `PluginRef` and a tooltip. Its `key` says who it is, for what the browser remembers about it:

| Kind | Key |
|---|---|
| audio | `audio:` + `os.path.normcase(os.path.normpath(path))` (`audio_key()`) |
| plugin | `plugin:<format>:<uid>` |
| device | `device:<kind>` |

The native side makes the same keys for indexed files without Python: a folder's key is the place's
`normcase(normpath(root))` joined with `ntLower()` of each folder name, and `ntLower` uses `LCMapStringEx` with the
invariant locale, Windows' own lower case, as `os.path.normcase` does.

### Groups

Every list is a search over one or more **groups**, in order. Group 0 (`_browser.AUDIO`, `kAudioGroup`) is the index's
audio files; other numbers are external groups the UI hands over with `set_external`: `BUILTIN = 1` (built-in
devices) and `PLUGINS = 2` (plug-ins). `scope_query()` turns a sidebar entry into (groups, tag, place prefix):

| Sidebar entry (scope) | Groups | Tag | Place prefix |
|---|---|---|---|
| All `("all",)` | BUILTIN, PLUGINS, AUDIO | | |
| Samples `("samples",)` | AUDIO | | |
| Built-in, or a category `("builtin", name)` | BUILTIN | the category | |
| Plug-ins, or *Instruments* / *Audio Effects* | PLUGINS | the category (`plugin_tag()`) | |
| A place `("place", path)` | AUDIO | | `place_prefix(path)` |

An external item's tag is what `tag` filters on: a built-in device's category, a plug-in's *Instruments* or
*Audio Effects*. The place prefix is the place's lower-case path with one trailing `\`.

### Snapshots

`Snapshot` is the index at one moment, immutable and shared by `shared_ptr`, so searches never lock against the
indexer:

- `folders`: the folders that have files, in the order the walk first reached them. Each `SnapFolder` holds its
  `FolderFiles`, its path as shown (the place's root as given, then the folder names), its lower-case path (for the
  place filter), its key (for use counts) and its detail (the folder's name; a place's root shows `basename(root)`).
- `audio`: every file as (folder, file), in the list's own order: by lower-case name, then walk order (the Python index
  sorted its walk by `name.lower()`, which keeps the walk order for equal names).
- `byFold`: positions in `audio` by casefolded name (ties in the list's own order), so ordering by name is a pass over
  it rather than a sort.
- `folderByKey`, `folderByPath`: lookups for use counts and `Result::find`.

`FolderFiles` packs a folder's names into one string with offsets: the name, its `pyLower`, its `pyCasefold` and its
`ntLower`. For ASCII names the last three share one copy.

### Results

`Result` holds its `generation`, the snapshot and external groups it points into, the `hits` (group, index) in order
and `searchMs`. It crosses into Python as `_browser.Result`; its rows are only made into Python objects when asked
for, a page at a time.

## How it works

### The indexer thread

`Indexer::run()` first enters background mode (`THREAD_MODE_BACKGROUND_BEGIN`: CPU, I/O and memory priority), then
loads the saved index, then loops:

1. Take commands under the lock: new places (`setPlaces`), a rescan request, stop.
2. Apply them. `applyPlaces()` keeps what is known of a place that stays (its node and its watcher), reuses a known
   node for a new place whose key is already in the tree (a place inside another), and starts a `FolderWatcher` for
   each new place; places that went lose their watchers. A rescan marks every folder dirty.
3. If something must be looked at, run `updatePass()`: publish what is known at once if the places changed (the saved
   index shows immediately), then walk.
4. Otherwise wait in `WaitForMultipleObjects` for the wake event, a place's watcher, or a deadline (changes settling,
   the next save).

**The walk** follows the Python walk it replaces exactly, so the lists are the same: from each place, depth first,
the last folder first (a stack), at most 16 folders deep (`maxDepth`), and no further folders once 300 000 files
(`maxFiles`) were found under a place; names starting with `.` or `$` are skipped; junctions are walked into,
symbolic links to folders are not (`listFolder` reads `FILE_ATTRIBUTE_REPARSE_POINT` and `IO_REPARSE_TAG_SYMLINK`, as
`DirEntry.is_dir(follow_symlinks=False)` does). Entries come in the order the file system lists them, as
`os.scandir` gives them, and only names ending in one of the audio extensions (`AUDIO_EXTENSIONS` in
`audio/engine_bridge.py`: `.wav`, `.wave`, `.flac`, `.mp3`) are kept.

During a pass a folder is listed again if it was never listed, is marked dirty, or its last-write time
(`folderTime`; for a junction, the time of the folder it leads to) differs from the one saved when it was listed.
Each folder's time is compared once per check round (`round_`). After the pass, folders no longer reachable from any
place are dropped (`removeUnreachable`), a snapshot is published if anything changed, and a save is scheduled for
5 s later.

The tree is shared between places: nodes are found by key (`byKey_`), so a folder under two overlapping places is one
node, and a snapshot lists it once.

**Commands interrupt passes.** `setPlaces()` and `rescan()` set an atomic `interrupt_` that the pass checks between
folders; the pass returns, the loop takes the commands, and walks again. `IndexStatus::busy` is true while commands
asked for are not finished (`done_ < requested_`).

**Publishing while scanning.** During a long scan `publishWhileScanning()` publishes a snapshot 30 ms after the
start if no files were shown yet, then every 150 ms or four times as long as building the last snapshot took,
whichever is more, and only when new files were found. So results show while a first scan is still going.

**Watching.** Each place has a `FolderWatcher`: `ReadDirectoryChangesW` on the place's root, recursive, for file and
folder names (added, removed, renamed), into a 64 KB buffer, overlapped. When it fires, `markChanged()` marks the
folder the change was in dirty, or the nearest folder above it that is known (new folders are found by listing that
one); changes in hidden folders are ignored. Changes settle for 250 ms after the last one (at most 1 s after the
first) before a pass. If the buffer overflowed (too much changed at once), every folder's time is compared again
(`++round_`). If a watcher fails, it is dropped and `rewatch()` tries again after the next pass, once the place's
root exists.

### The search thread

`Browser::search()` takes the next generation (`latest_`, an atomic counter), replaces any waiting query with it,
clears any finished result and wakes the search thread. A running search sees the counter move and stops: the
filters and sorts look at it every 4096 items (`kCheckEvery`). The thread then runs the newest query over the newest
snapshot (`indexer_.snapshot()`), the external groups and the use counts as they were when it started.

A finished result is kept only if it is still the latest; then the event is set. `take()` hands it out once, and only
if its generation is still the latest, so the UI never shows the results of a search it has replaced.

### Matching and ordering

Matching and ordering are defined by the Python search (see [search.py](../src/substation/ui/browser/search.py) and
`find()` in the reference):

- **Terms**: `pySplit(pyLower(text))`, as `query.lower().split()`.
- **Match**: every term is in the item's lower-case name or its lower-case detail. For files the detail is the
  folder's, so whether a term is in it is worked out once per folder.
- **Place filter**: the folder's lower-case path plus `\` starts with the prefix.
- **Tag filter**: external items with that tag only.
- **Rank** (`sortByRank`): items with a use rank above 0 first, by rank, then match quality (stable); then the unused
  ones by match quality, best first, keeping the list's own order (a counting sort over quality buckets); items with
  a negative rank last. With no terms every quality is 0, so only the used items move.
- **Name** (`sortByName`): by casefolded name, stable over the list's own order. Each group is ordered and the groups
  are merged, which is the same as a stable sort of the whole list. For files the snapshot's `byFold` is already in
  that order.

The list's own order is the groups' order (built-in devices, plug-ins as the scan found them, then samples by name).

**Match quality** (`matchQuality`, as the reference's `match_quality`): 8 if the name's stem (an audio file's name
without its extension) equals the terms joined by spaces; then for each term, 3 if the name starts with it, else 2
if a word in the name starts with it, else 1 if it is anywhere in the name. Word starts are where
`re.finditer(r"(?:^|[\s_\-.()\[\]])(\w)", name)` finds its group (`wordStarts`).

**Use rank** (`Usage::rank`, as `Library.rank`): `score * 0.5 ** (days since last use / 30)`, 0 when there is no
score. For files, `resolveAudioUsage()` matches each `audio:` record to a file through its folder's key and the
file's `ntLower` name (a record for a drive's root is looked up with its trailing `\`); the result is cached in
`UsageCache` for as long as the snapshot and the use counts stay the same.

### Text: Python's rules, from Python's tables

[Text.h](../browser/src/Text.h) implements `str.lower()`, `str.casefold()`, `str.split()` and the regex classes `\s` and
`\w` from tables generated out of Python itself, so the backend agrees with Python for every character:

- `kLower` and `kFold`: each character's mapping where it changes (up to three code points). Capital sigma (U+03A3)
  is lowered by its context (the Final_Sigma rule, `isFinalSigma`), which needs Python's "case-ignorable" and "cased"
  properties; Python doesn't expose them, so the generator reads them back by lowering probe strings (`kCaseIgnorable`,
  `kCased`).
- `kSpace`: `str.isspace()`, what `split()` and `\s` split on. `kWord`: `str.isalnum()` or `_`.
- ASCII is handled by a fast path and a 128-entry class table.

Strings are **WTF-8**: UTF-8 that may also hold unpaired surrogates, which Windows file names (and so Python strings)
can contain. The bindings convert with `surrogatepass` both ways, so such names survive the trip as they do through
`os.scandir`. Byte order is code point order, so comparing bytes compares strings as Python does.

To follow a newer Unicode version, re-run the generator with that Python:

```powershell
python browser/tools/gen_unicode_tables.py
```

The header of `UnicodeTables.inc` records the Python and Unicode versions it was made with (currently Python 3.12.10,
Unicode 15.0.0); `_browser.UNICODE_VERSION` exposes the latter.

### The saved index

The index is saved to `%LOCALAPPDATA%\SUBstation\browser-index.bin` (`index_path()`; the environment variable
`SUBSTATION_BROWSER_INDEX` overrides it, as the tests do). An empty store path means nothing is saved. It is written
5 s after a pass that changed something, and when the backend closes; it goes to `browser-index.bin.tmp` first and is
moved over the old one (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`).

Format (little-endian, as written by `Writer` in `Indexer.cpp`):

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
with the disk (`checked = 0`), so on the next start the saved index shows at once and only folders that changed are
listed. A file added in a way that doesn't change the folder's time (on drives that don't report it) is only found
by *Rescan*, which lists every folder again.

### The UI side

`FileIndex` (in `file_index.py`) makes the backend with `index_path()`, `AUDIO_EXTENSIONS`, `MAX_FILES = 300_000` and
`MAX_DEPTH = 16`. The backend's `event_handle` goes into a `QWinEventNotifier` (as a `shiboken6.VoidPtr`), whose
`activated` signal starts a zero-interval single-shot timer that calls `_take()`. `_take()` calls `native.take()`,
emits `results` with a `SearchResult` when there is one, and `updated` when indexing started or stopped or the index
version changed. (Off Windows, where there is no `QWinEventNotifier`, the timer polls every 15 ms.)

`BrowserPanel` asks for every list as a search:

- The search field's `textChanged` starts a zero-interval timer, which only merges changes that come together; there
  is no typing delay, since searching doesn't hold up the UI. `_refresh()` calls `FileIndex.search()` with the text,
  the sort, `library.clock()` and the scope's groups, tag and prefix.
- A place with no search text shows a `QFileSystemModel` tree of the folder instead (results that arrive after the
  tree was shown are dropped).
- When the index or the plug-ins change, the list is searched again with `keep=True`: the panel remembers the
  current item and scroll position and restores them when the results come (`SearchResult.find()`), if the item is
  within the first 5000 rows (`KEEP_WITHIN`).
- Enter or Down in the search field selects the first result (once the results are there, if they are still on
  their way); Enter on a result adds it, as a double-click does.

**Paging.** Results cross into Python a page at a time: `ItemListModel` shows the first 256 rows (`PAGE`) at once and
more through Qt's `canFetchMore`/`fetchMore` as the view scrolls near the end; `ensure_rows()` fetches up to a row
when it has to (restoring a position). A view lays out every row it has, about 1 µs a row, so a list of every file
would cost the UI thread about 200 ms each time it changed. `SearchResult.items()` turns rows
`(kind, name, path, detail, key)` into `BrowserItem`s: files (empty key) are made fresh, other items are the
panel's own objects by key.

**Use counts.** An item counts as used when it is added to the project from the browser: double-click, Enter, or a
drag that is dropped somewhere (`_start_drag` emits `dropped` only when the drop was accepted). `Library.record_use()`
adds 1 to `uses`, sets `score` to the current rank plus 1 and `last_used` to now, and saves; the panel then hands the
records to the backend (`set_usage`, as `(key, score, last_used or NaN)`). The list is not re-sorted then, so the
selection stays put. `library.json` (`%LOCALAPPDATA%\SUBstation\library.json`, or `SUBSTATION_LIBRARY`) is
`{"version": 1, "items": {key: record}}`; records are plain dicts and fields this version doesn't know are kept. A file
that can't be read or has another version counts as empty; a failed save is ignored. The tooltip of an item shows how
often it was used.

**Plug-ins.** `PluginIndex` runs `PluginScanner` on a `QThread` (`_PluginScanThread`), at start-up and on *Rescan
Plug-ins*; a scan asked for while one runs is run after it. Its items go to the backend as group `PLUGINS`; see
[python/plugin-scanner.md](python/plugin-scanner.md).

**Preview.** Selecting an audio file (in the list or the tree) previews it through the bridge
(`EngineBridge.preview_file`) while the headphones button is on; a click anywhere outside the browser stops it (an
application-wide event filter). See [python/engine-bridge.md](python/engine-bridge.md).

**Places and settings.** The places and the sort are kept in `QSettings` (`browser/places`, `browser/sort`). The
first start has the user's Music folder (or the home folder) as its place. *Add Folder…* indexes only the new place;
*Remove from Places* drops it; *Rescan* calls `FileIndex.rebuild()`, which sets the places and asks for a rescan.
`MainWindow` calls `BrowserPanel.shutdown()` on close, which stops the backend's threads (saving the index) and waits
for a plug-in scan.

## The `_browser` module

| Name | What it is |
|---|---|
| `Browser(store, extensions, max_files=300000, max_depth=16)` | Starts the backend. `store`: where the index is saved (`''` for nowhere); `extensions`: lower case, with the dot. |
| `event_handle` | The Win32 event, set when there is something to `take()`. |
| `set_places(places)` | Places as `(root, normcase(normpath(root)), basename(root))` (`place_spec()`). |
| `rescan()` | List every folder again. |
| `set_external(group, items)` | A group of other items as `(kind, name, path, detail, key, tag)`. |
| `set_usage(records, half_life_days)` | Use counts as `(key, score, last_used)`, `last_used` NaN when unknown. |
| `search(text, sort, now, groups, tag="", place_prefix="")` | Starts a search, replacing any that runs; returns its generation. `sort` is `"rank"` or `"name"`. |
| `take()` | `(indexing, index version, files, Result or None)`. |
| `indexing`, `version`, `file_count`, `searching`, `stats` | Status; `stats` has files, folders, `load_ms`, `build_ms`, `pass_ms`, `listed`, `checked` (for benchmarks). |
| `wait_idle(seconds)` | Waits until the index settled and no search runs; False on timeout (tests, benchmarks). |
| `close()` | Stops the threads, saving the index. |
| `Result.generation`, `.search_ms`, `.total` | |
| `Result.rows(start, count)` | Rows `[start, start + count)` as `(kind, name, path, detail, key)`; key is `''` for indexed files. |
| `Result.find(kind, identity)` | Row of an item (an indexed file by path, others by key), or -1. |
| `AUDIO`, `UNICODE_VERSION` | Group 0; the Unicode version of the tables. |
| `lower`, `casefold`, `split`, `nt_lower`, `word_starts`, `match_quality` | The text functions, exposed to test them against Python's own. |

Item kinds are numbered as `KINDS = ("audio", "plugin", "device")` in `file_index.py` (`Kind` in `Model.h`).

The bindings convert arguments while holding the GIL and release it for the call; `take()` releases it while it takes.
Making rows into Python objects holds it, which is why results are read a page at a time.

## Invariants

- The backend's threads never call into Python or the engine, and never wait on the UI thread.
- Searches read only immutable, shared data: snapshots, external groups and use counts are replaced, never changed.
  `setExternal` and `setUsage` build new objects and swap them in under the lock.
- Only the latest search's results are handed out, and only once.
- The index lists what the Python walk listed, in the same order, and the search orders as the Python `find()` did,
  for every character. A change to either has to change the reference too, or be a deliberate break of that parity.

## Extending it

- **A new sort order**: add it to `Sort` in `Model.h`, implement it in `Search::run()`, parse its name in
  `bindings.cpp` (`search`), and add it to `SORTS` in `search.py`.
- **A new kind of item to list** (say, presets): give it a group number in `search.py`, hand its items over with
  `FileIndex.set_items(group, [(item, tag), ...])`, and add a scope to `scope_query()` and the sidebar. A new kind
  name goes into `KINDS` and `Kind` together.
- **New filters** (items hidden from search) and orders (similar sounds) belong in `search.py`'s `scope_query` and the
  native `Query`; `library.py` keeps unknown fields so later versions can store what they need per item.
- **Another audio extension**: add it to `AUDIO_EXTENSIONS`. The saved index records the extensions it was made with,
  so the next start lists everything again.
- **Changing the saved format**: bump `kFormat` in `Indexer.cpp`; old files are then ignored.
- **A newer Python** (another Unicode version): re-run `gen_unicode_tables.py` and rebuild.

## Gotchas

- `_browser` has no API version check, unlike `_engine` (see [building.md](building.md#api_version-and-engine_api)):
  after changing `browser/src/bindings.cpp`, re-run the install command, or Python code may call a module built from
  older code.
- The indexer waits on its wake event and the places' watchers in one `WaitForMultipleObjects`, which takes at most
  64 handles: only the first 63 places are watched for changes. The others are still checked by folder time on the
  next pass.
- Drives that don't report changes (some network drives) are only re-read by folder times or *Rescan*.
- A saved index written a moment before is sometimes slow to read (0.5–1 s), apparently antivirus scanning the new
  file; see the benchmarks' *Variance* note.
- File system calls are the wide (`W`) ones and paths cross as WTF-8; never convert through the ANSI code page.
- `Result.find()` for an indexed file compares paths as shown (the place's root as given), not keys.

## Tests

- [tests/test_browser_native.py](../tests/test_browser_native.py): the backend against
  [tests/browser_reference.py](../tests/browser_reference.py). `str.lower` and `casefold` of every Unicode character,
  the final sigma, `split` and word starts on random strings (skipped if the tables were made with another Unicode
  version), keys as `os.path.normcase`, match quality; random queries, sorts, tags, places and use counts ordered as
  Python orders them; the files of a folder tree (hidden names, depth and file limits, junctions and symbolic links,
  overlapping and missing places); the saved index (checked by folder times and not listed again, files changed while
  closed, rescan, damaged files ignored); changes seen while running; places changing incrementally; only the latest
  search's results handed out; paging through `ItemListModel`; and that waiting releases the GIL.
- [tests/test_browser_search.py](../tests/test_browser_search.py): keys, use counts decaying and persisting,
  unknown fields kept, a bad `library.json` ignored, match quality preferring name starts, rank putting used items
  first.
- [tests/test_ui_smoke.py](../tests/test_ui_smoke.py): the panel in the real window: indexing and searching, Down
  previewing the first result until a click elsewhere, Ctrl+F searching *All*, keeping its place when files change,
  results dropped after the tree was shown, Enter selecting then adding, drops from the browser, built-in devices in
  the browser, used items ranking first.
- [tests/test_ui_plugins.py](../tests/test_ui_plugins.py): plug-ins in the browser.

The benchmarks ([benchmarks/README.md](../benchmarks/README.md)) compare the backend with the reference and the panel
before and after; see [testing.md](testing.md#benchmarks).
