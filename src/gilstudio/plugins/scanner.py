"""Finds the VST3 plug-ins installed on this computer and what each file contains.

Reading a plug-in file means running its code, and a broken plug-in can crash
or hang the process that loads it. So files are read in a child process
(`python -m gilstudio.plugins.scan_worker`), many per process: if one takes the
process down or doesn't answer in time, it is marked as failed and the rest
carry on in a new process. Results are cached per file, so a file is read
again only when it changes (or on a rescan).

Qt-free; the browser runs this on a worker thread.
"""

from __future__ import annotations

import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from dataclasses import asdict, dataclass, field
from pathlib import Path

CACHE_VERSION = 1
SCAN_TIMEOUT = 60.0  # seconds one file may take (some copy-protected plug-ins are slow)


@dataclass(frozen=True)
class PluginInfo:
    """One plug-in (a VST3 class) in a file."""

    name: str
    format: str  # "VST3"
    path: str  # the .vst3 bundle or file
    uid: str = ""  # VST3 class id
    vendor: str = ""
    version: str = ""
    category: str = ""  # VST3 sub-categories, e.g. "Instrument|Synth" or "Fx|EQ"
    instrument: bool = False


@dataclass(frozen=True)
class ScanFailure:
    path: str
    reason: str


@dataclass
class ScanResult:
    plugins: list[PluginInfo] = field(default_factory=list)
    failures: list[ScanFailure] = field(default_factory=list)


def standard_paths() -> list[Path]:
    """The standard VST3 folders, or those in GILSTUDIO_VST3_PATH (separated by
    os.pathsep; empty for none)."""
    override = os.environ.get("GILSTUDIO_VST3_PATH")
    if override is not None:
        return [Path(p) for p in override.split(os.pathsep) if p]
    common = Path(os.environ.get("CommonProgramFiles", r"C:\Program Files\Common Files"))
    local = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Programs" / "Common"
    return [common / "VST3", local / "VST3"]


def search_paths(custom: list[str] | tuple[str, ...] = ()) -> list[Path]:
    """The standard folders, then the user's own (each once)."""
    paths: dict[str, Path] = {}
    for path in [*standard_paths(), *(Path(p) for p in custom if p)]:
        paths.setdefault(os.path.normcase(os.path.normpath(str(path))), path)
    return list(paths.values())


def find_plugin_files(roots: list[Path] | None = None) -> list[str]:
    """The .vst3 bundles and files under the search paths (bundles are folders;
    nothing inside one is listed). Linked folders (symlinks, junctions) are
    followed, each real folder once, so a link back up can't loop."""
    found: dict[str, str] = {}
    seen: set[str] = set()  # real paths of the folders already listed
    for root in search_paths() if roots is None else roots:
        if root.suffix.lower() == ".vst3" and root.exists():
            found.setdefault(os.path.normcase(str(root)), str(root))
            continue
        if not root.is_dir():
            continue
        stack = [root]
        while stack:
            folder = stack.pop()
            try:
                real = os.path.normcase(os.path.realpath(folder))
                if real in seen:
                    continue
                seen.add(real)
                entries = list(os.scandir(folder))
            except OSError:
                continue
            for entry in entries:
                if entry.name.lower().endswith(".vst3"):
                    found.setdefault(os.path.normcase(entry.path), entry.path)
                else:
                    try:
                        if entry.is_dir():
                            stack.append(Path(entry.path))
                    except OSError:
                        continue
    return sorted(found.values(), key=str.lower)


def cache_path() -> Path:
    override = os.environ.get("GILSTUDIO_PLUGIN_CACHE")
    if override:
        return Path(override)
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    return base / "GIL Studio" / "vst3-cache.json"


def _binary(path: str) -> Path:
    """The file a VST3 bundle's code is in (the path itself for a single file)."""
    p = Path(path)
    inner = p / "Contents" / "x86_64-win" / p.name
    return inner if p.is_dir() and inner.exists() else p


_WINDOWS_ERRORS = {126: "a file it needs is missing", 193: "it is not a 64-bit Windows plug-in",
                   1114: "it failed to start"}


def _friendly(reason: str) -> str:
    """The loader's error, without its path and Windows' line breaks."""
    reason = " ".join(reason.split())
    match = re.search(r"LoadLibraryW failed with error number: (\d+)", reason)
    if match:
        code = int(match.group(1))
        return f"Windows could not load it: {_WINDOWS_ERRORS.get(code, f'error {code}')}."
    if reason.startswith("LoadLibraryW failed for path") and ": " in reason:
        return "Windows could not load it: " + reason.rsplit(": ", 1)[1]
    return reason


def _signature(path: str) -> list[int] | None:
    try:
        stat = _binary(path).stat()
    except OSError:
        return None
    return [stat.st_mtime_ns, stat.st_size]


class PluginScanner:
    def __init__(self, cache_file: Path | None = None, timeout: float = SCAN_TIMEOUT,
                 worker: list[str] | None = None, folders: list[Path] | None = None):
        self.folders = folders  # where to look (default: the standard folders)
        self.cache_file = cache_path() if cache_file is None else Path(cache_file)
        self.timeout = timeout
        self.worker = worker or [sys.executable, "-m", "gilstudio.plugins.scan_worker"]

    # --- Cache ---------------------------------------------------------------------

    def _load_cache(self) -> dict[str, dict]:
        try:
            data = json.loads(self.cache_file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}
        if not isinstance(data, dict) or data.get("version") != CACHE_VERSION:
            return {}
        files = data.get("files")
        return files if isinstance(files, dict) else {}

    def _save_cache(self, files: dict[str, dict]) -> None:
        try:
            self.cache_file.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.cache_file.with_suffix(".tmp")
            tmp.write_text(json.dumps({"version": CACHE_VERSION, "files": files}, indent=1), encoding="utf-8")
            os.replace(tmp, self.cache_file)
        except OSError:
            pass  # a cache we can't write only costs time

    # --- Scanning ------------------------------------------------------------------

    def scan(self, files: list[str] | None = None, rescan: bool = False,
             progress: Callable[[int, int, str], None] | None = None,
             cancelled: Callable[[], bool] | None = None) -> ScanResult:
        """Everything in `files` (default: all installed plug-in files). Only new or
        changed files are read, unless `rescan`. `progress(done, total, path)` is
        called before each file that is read."""
        files = find_plugin_files(self.folders) if files is None else files
        cache = {} if rescan else self._load_cache()
        entries: dict[str, dict] = {}
        todo: list[str] = []
        for path in files:
            key = os.path.normcase(path)
            entry = cache.get(key)
            if entry is not None and entry.get("signature") == _signature(path):
                entries[key] = entry
            else:
                todo.append(path)

        for path, answer in self._read(todo, progress, cancelled):
            entries[os.path.normcase(path)] = {"path": path, "signature": _signature(path), **answer}
        # Keep what we know about files not scanned this time, while they exist.
        scanned = {os.path.normcase(f) for f in files}
        kept = {k: v for k, v in cache.items() if k not in scanned and Path(v.get("path", "")).exists()}
        self._save_cache(kept | entries)

        result = ScanResult()
        for path in files:
            entry = entries.get(os.path.normcase(path))
            if entry is None:
                continue  # cancelled before this one
            if entry.get("error"):
                result.failures.append(ScanFailure(path, _friendly(entry["error"])))
            for p in entry.get("plugins", []):
                result.plugins.append(PluginInfo(
                    name=p.get("name") or Path(path).stem, format="VST3", path=path, uid=p.get("uid", ""),
                    vendor=p.get("vendor", ""), version=p.get("version", ""), category=p.get("category", ""),
                    instrument=bool(p.get("instrument", False))))
        result.plugins.sort(key=lambda p: (p.name.lower(), p.vendor.lower()))
        return result

    def _read(self, paths: list[str], progress, cancelled):
        """Yields (path, answer) for each path, reading them in worker processes."""
        total = len(paths)
        done = 0
        remaining = list(paths)
        while remaining:
            if cancelled and cancelled():
                return
            creation = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
            process = subprocess.Popen(self.worker, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
                                       creationflags=creation)
            lines: queue.Queue[str | None] = queue.Queue()
            batch, remaining = remaining, []

            def pump(stream=process.stdout, sink=lines) -> None:
                for line in stream:
                    sink.put(line)
                sink.put(None)  # the process ended

            def feed(stream=process.stdin, paths=batch) -> None:
                # From a thread of its own: a plug-in that hangs stops the worker
                # reading, and a full pipe must not stop us.
                try:
                    for path in paths:
                        stream.write(json.dumps(path) + "\n")
                        stream.flush()
                    stream.close()
                except (OSError, ValueError):
                    pass  # the worker has gone; the answers show which file it was on

            threading.Thread(target=pump, daemon=True).start()
            threading.Thread(target=feed, daemon=True).start()
            finished = False
            try:
                if not self._next_line(lines):
                    raise RuntimeError("The plug-in scanner could not start.")
                for index, path in enumerate(batch):
                    if cancelled and cancelled():
                        return
                    if progress:
                        progress(done, total, path)
                    answer = self._next_answer(lines, path)
                    done += 1
                    if isinstance(answer, str):
                        # This file crashed or hung the worker: note it, go on without it.
                        yield path, {"error": f"The plug-in {answer}.", "plugins": []}
                        remaining = batch[index + 1:]
                        break
                    yield path, answer
                else:
                    finished = True
            finally:
                try:
                    process.wait(timeout=5 if finished else 0)  # it ends by itself once its input ends
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    def _next_line(self, lines: queue.Queue, deadline: float | None = None) -> dict | None:
        """The worker's next answer, None if it ended, {} if the deadline passed."""
        while True:
            try:
                timeout = None if deadline is None else max(0.0, deadline - time.monotonic())
                line = lines.get(timeout=timeout if deadline is not None else self.timeout)
            except queue.Empty:
                return {}
            if line is None:
                return None
            try:
                answer = json.loads(line)
            except ValueError:
                continue  # not ours
            if isinstance(answer, dict):
                return answer

    def _next_answer(self, lines: queue.Queue, path: str) -> dict | str:
        """The answer for `path`, or why there is none ("crashed while loading" or "timed out")."""
        deadline = time.monotonic() + self.timeout
        while True:
            answer = self._next_line(lines, deadline)
            if answer is None:
                return "crashed while loading"
            if not answer:
                return "timed out"
            if answer.get("path") == path:
                return {"plugins": answer.get("plugins", []), "error": answer.get("error")}


def scan_plugins(rescan: bool = False, progress=None, cancelled=None) -> ScanResult:
    return PluginScanner().scan(rescan=rescan, progress=progress, cancelled=cancelled)


def plugin_to_dict(plugin: PluginInfo) -> dict:
    return asdict(plugin)
