# Plug-in scanner

Finds the VST3 plug-ins installed on this computer and what each file contains, without letting a broken plug-in
take the program down. It lives in [src/substation/plugins/](../../src/substation/plugins): the scanner
([scanner.py](../../src/substation/plugins/scanner.py)), the child process that reads plug-in files
([scan_worker.py](../../src/substation/plugins/scan_worker.py)) and the user's own plug-in folders
([settings.py](../../src/substation/plugins/settings.py)).

How the user sees it (the browser's *Plug-ins* category, *Rescan Plug-ins*, the folders in the preferences) is in
[guide/plugins.md](../guide/plugins.md). Loading and hosting a plug-in once found is the engine's: see
[engine/plugins.md](../engine/plugins.md).

## Overview

- Reading a plug-in file means running its code, and a broken plug-in can crash or hang the process that loads it.
  So files are read in a child process (`python -m substation.plugins.scan_worker`), several per process: if one
  takes the process down or doesn't answer in time, it is marked as failed and the rest carry on in a new process.
- Results are cached per file in `%LOCALAPPDATA%\SUBstation\vst3-cache.json`, so a file is read again only when it
  changes (or on a rescan). The scan runs in the background at start-up and reads only new or changed files.
- Plug-ins are looked for in the standard VST3 folders (`C:\Program Files\Common Files\VST3` and
  `%LOCALAPPDATA%\Programs\Common\VST3`) and in folders of the user's own.
- `scanner.py` is Qt-free; the browser runs it on a worker thread (`_PluginScanThread` in
  [ui/browser/file_index.py](../../src/substation/ui/browser/file_index.py)).

## Files

| File | What it holds |
|---|---|
| [scanner.py](../../src/substation/plugins/scanner.py) | `PluginInfo`, `ScanFailure`, `ScanResult`; `standard_paths()`, `search_paths()`, `find_plugin_files()`, `cache_path()`; `PluginScanner` (cache, child processes); `scan_plugins()`, `plugin_to_dict()` |
| [scan_worker.py](../../src/substation/plugins/scan_worker.py) | the child process: answers one JSON line per plug-in file, using the engine's `scan_vst3()` |
| [settings.py](../../src/substation/plugins/settings.py) | the user's VST3 folders in `QSettings` (`plugins/vst3_folders`): `custom_folders()`, `set_custom_folders()`, `plugin_folders()` |
| [ui/browser/file_index.py](../../src/substation/ui/browser/file_index.py) | `_PluginScanThread` and `PluginIndex`: runs scans in the background, holds the plug-ins and failures for the browser |
| [ui/browser/browser_panel.py](../../src/substation/ui/browser/browser_panel.py) | starts the scan at start-up, *Rescan Plug-ins*, the failures in the *Plug-ins* tooltip and the status line |
| [ui/dialogs.py](../../src/substation/ui/dialogs.py) | *Preferences › Plug-ins*: the folder list, *Add Folder*, *Remove*, *Rescan*, the scan status |

## Key types

```python
@dataclass(frozen=True)
class PluginInfo:          # one plug-in (a VST3 class) in a file
    name: str
    format: str            # "VST3"
    path: str              # the .vst3 bundle or file
    uid: str = ""          # VST3 class id (32 hex digits)
    vendor: str = ""
    version: str = ""
    category: str = ""     # VST3 sub-categories, e.g. "Instrument|Synth" or "Fx|EQ"
    instrument: bool = False

@dataclass(frozen=True)
class ScanFailure:
    path: str
    reason: str            # for the user

@dataclass
class ScanResult:
    plugins: list[PluginInfo]
    failures: list[ScanFailure]
```

A module with several plug-ins (an instrument and its FX version) gives one `PluginInfo` each. The plug-ins are sorted
by name, then vendor (case-insensitive). A plug-in with no name is named after its file.

## Where it looks

- `standard_paths()`: `%CommonProgramFiles%\VST3` and `%LOCALAPPDATA%\Programs\Common\VST3` (with fallbacks if the
  variables are missing). If `SUBSTATION_VST3_PATH` is set, those folders instead (separated by `os.pathsep`; empty
  for none). The tests set it to the test plug-ins' folder, so they never see the installed ones.
- `search_paths(custom)`: the standard folders, then the user's own, each once (compared by `normcase(normpath())`).
- `settings.plugin_folders()`: `search_paths(custom_folders())`, what a scan looks in.
- `find_plugin_files(roots=None)`: every `.vst3` bundle (a folder) or file under the roots. Nothing inside a bundle is
  listed. Linked folders (symbolic links, junctions) are followed, each real folder once (by `realpath`), so a link
  back up can't loop. A root that is itself a `.vst3` is listed as it is. Unreadable folders are skipped. Sorted
  case-insensitively, one entry per `normcase` path.

The engine has its own list of the standard folders (`vst3_search_paths()`, from Windows' known folders), which the
Python side doesn't use for scanning.

## How a scan works

```
 PluginScanner.scan(files=None, rescan=False, progress, cancelled)
   files = find_plugin_files(folders)
   cache = load vst3-cache.json (unless rescan)
   for each file: signature (mtime_ns, size of its binary) matches the cache? -> reuse, else -> todo
   _read(todo):                                      child process (scan_worker.py)
     spawn worker  ---------------------------------> {"ready": true}
     feed thread:  one JSON path per line on stdin -> for each line: ge.scan_vst3(path)
     pump thread:  stdout lines -> queue      <------ {"path", "plugins": [...]} or {"path", "error"}
     per file: wait up to `timeout` for its answer
       no answer, process ended -> "crashed while loading"  \  note the failure,
       no answer in time        -> "timed out"               /  start a new worker for the rest
   save cache: entries read now + reused ones + cached files not in this scan that still exist
   ScanResult(plugins sorted, failures with friendly reasons)
```

### The signature

A file is read again when its *signature* changes: `[st_mtime_ns, st_size]` of the file its code is in. For a bundle
that is `Contents\x86_64-win\<bundle name>` inside it (`_binary()`), else the path itself. A file whose signature
can't be read gets `None`, which never matches.

### The child process

`PluginScanner(worker=...)` defaults to `[sys.executable, "-m", "substation.plugins.scan_worker"]`, started with
`CREATE_NO_WINDOW` on Windows, stdin and stdout as UTF-8 text pipes, stderr discarded.

The worker ([scan_worker.py](../../src/substation/plugins/scan_worker.py)):

1. Keeps stdout for its answers: it duplicates the stdout handle for itself, then points file descriptors 1 and 2 at
   `os.devnull`, so whatever plug-ins print goes nowhere.
2. On Windows sets the error mode (`SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX`): no
   "program stopped working" or "insert a disk" dialogs if a plug-in misbehaves.
3. Imports `substation._engine` and says `{"ready": true}`.
4. For each JSON-encoded path on stdin, answers one JSON line: `{"path": ..., "plugins": [{"uid", "name", "vendor",
   "version", "category", "instrument"}, ...]}` from `ge.scan_vst3(path)`, or `{"path": ..., "error": ...}` if it
   raises. `scan_vst3` loads the module, initialising COM on its thread first (see
   [engine/plugins.md](../engine/plugins.md#vst3format)).
5. Ends when its input ends.

The parent (`PluginScanner._read`):

- feeds the paths from a thread of its own: a plug-in that hangs stops the worker reading, and a full pipe must not
  stop the parent;
- pumps the worker's stdout into a queue from another thread, with `None` when the process ends;
- waits for `{"ready": ...}`; if the worker ends or doesn't answer first, raises `RuntimeError("The plug-in scanner
  could not start.")`;
- for each path, waits up to `timeout` (`SCAN_TIMEOUT`, 60 s: some copy-protected plug-ins are slow) for the answer
  with that path; lines that aren't JSON objects are skipped as not its own;
- on a crash or a time-out, yields the failure (`"The plug-in crashed while loading."` or `"The plug-in timed out."`)
  and starts a new worker for the remaining paths;
- when the batch is done, waits up to 5 s for the worker to end by itself (at once if it failed), then kills it.

`progress(done, total, path)` is called before each file that is read; `cancelled()` is checked between files and
between workers. A cancelled scan returns what it has; files not reached are left out of the result but keep their
cache entries.

### The cache file

`cache_path()`: `%LOCALAPPDATA%\SUBstation\vst3-cache.json`, or `SUBSTATION_PLUGIN_CACHE` if set (the tests point it
at a temporary file).

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

- Keys are `os.path.normcase(path)`.
- A file version other than `CACHE_VERSION` (1), or a file that can't be read or parsed, counts as an empty cache.
- It is written to a `.tmp` file and moved into place (`os.replace`), so a crash never leaves half a cache. A cache
  that can't be written only costs time.
- Failures are cached too: a file that crashed or timed out is not read again until it changes, or on a rescan.
- Entries of files not in this scan (a folder removed from the list) are kept while the file exists, so adding the
  folder back is quick; they don't appear in the result.

### Friendly reasons

`_friendly(reason)` turns the loader's error into a line for the user: whitespace collapsed; Windows' `LoadLibraryW`
error numbers named (126: "a file it needs is missing", 193: "it is not a 64-bit Windows plug-in", 1114: "it failed
to start", others "error N"), as "Windows could not load it: ..."; a `LoadLibraryW failed for path ...: <message>`
keeps only Windows' message. Other reasons pass through as they are.

## In the application

- **Start-up.** The browser panel calls `PluginIndex.scan()` when it is made: a `_PluginScanThread` (a `QThread`) runs
  `PluginScanner(folders=plugin_folders()).scan(...)` and hands back the plug-ins and failures. Only new or changed
  files are read.
- **One scan at a time.** A scan asked for while one runs is remembered (as a rescan if either asked for one) and runs
  when it finishes.
- **Rescan.** *Options › Rescan Plug-ins* and the button in *Preferences › Plug-ins* call `scan(rescan=True)`: every
  file is read again, also those that failed before.
- **Failures.** Hover over *Plug-ins* in the browser for the list and the reasons (the first 30); the status line
  says how many files could not be read. The preferences show the count and the reasons as a tooltip. A
  `RuntimeError` or `OSError` from the scan (the worker couldn't start) goes to the status line.
- **User folders.** *Options › Preferences › Plug-ins* lists the standard folders (dimmed, always searched) and the
  user's own (red if missing). *Add Folder…* stores it (unless it is already listed) and scans at once: only the new
  folder's files are read, the rest come from the cache. *Remove* takes it out of the list and scans again, so its
  plug-ins leave the browser. Folders are kept in `QSettings` under `plugins/vst3_folders` (a one-item list comes
  back from `QSettings` as a string; `custom_folders()` handles that).
- **Progress.** The browser's status shows "Scanning plug-ins 3/40: Name" while the *Plug-ins* category is shown; the
  preferences show the same.
- The browser lists the plug-ins under *Plug-ins › Instruments / Audio Effects* with their vendor; a project's plug-in
  devices are matched to them by class id, so a plug-in that moved is found again (see
  [python/engine-bridge.md](engine-bridge.md) and [browser.md](../browser.md)).

## Invariants

- Plug-in code never runs in the UI process during a scan. (Loading a plug-in onto a track does run it in the UI
  process, on the main thread: that is hosting, not scanning.)
- The worker's stdout carries only its JSON answers.
- One answer per path, in order; the parent matches answers by path, so a stray line can't be taken for an answer.

## Extending it

- **Another format** (CLAP): the worker would call that format's binding by file extension and answer with a format
  field; `PluginInfo.format` already exists, and `find_plugin_files()` would look for the format's extension. Bump
  `CACHE_VERSION` if the cache's shape changes. See [engine/plugins.md](../engine/plugins.md#clap-not-implemented).
- **More metadata**: add it to the worker's answer, to `PluginInfo`, and to the reading in `scan()`. Old cache
  entries lack it; bump `CACHE_VERSION` to read every file again.

## Gotchas

- A changed plug-in is only noticed by its binary's modification time and size.
- The timeout is per file; a slow but working plug-in over 60 s is marked as timed out until a rescan.
- `scanner.py` must stay Qt-free (it runs on a plain worker thread and in tests without Qt); `settings.py` uses
  `QSettings` and is imported only by the UI.

## Tests

- [tests/test_plugin_scanner.py](../../tests/test_plugin_scanner.py): reading the test bundle (four plug-ins, names,
  vendors, categories, class ids) and a junk file, caching (unchanged files not read again, changed ones are, a
  rescan reads everything), a plug-in that crashes while loading costing only itself (the files after it read by a
  new worker), a hanging plug-in timing out, finding files (links, nested folders, `SUBSTATION_VST3_PATH`), and the
  friendly messages. The test plug-ins crash or hang on purpose when `SUB_TEST_PLUGIN_CRASH` or
  `SUB_TEST_PLUGIN_HANG` is set.
- [tests/test_ui_plugins.py](../../tests/test_ui_plugins.py): plug-ins in the browser, and plug-in folders in the
  preferences.
- [tests/conftest.py](../../tests/conftest.py) sets `SUBSTATION_VST3_PATH` and `SUBSTATION_PLUGIN_CACHE` for the
  whole suite.

See [testing.md](../testing.md).
