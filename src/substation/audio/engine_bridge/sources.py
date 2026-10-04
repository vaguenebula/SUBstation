"""Audio sources: files decoded in a thread pool, for clips and previews."""

from __future__ import annotations

import os
from collections.abc import Callable

from PySide6.QtCore import QObject, QRunnable, Signal

from ... import _engine as ge

AUDIO_EXTENSIONS = (".wav", ".wave", ".flac", ".mp3")


def is_audio_file(path: str) -> bool:
    return path.lower().endswith(AUDIO_EXTENSIONS)


def _key(path: str) -> str:
    return os.path.normcase(os.path.abspath(path))


class _LoadSignals(QObject):
    loaded = Signal(str, object)
    failed = Signal(str, str)


class _LoadTask(QRunnable):
    def __init__(self, engine: ge.Engine, path: str, signals: _LoadSignals):
        super().__init__()
        self.engine = engine
        self.path = path
        self.signals = signals

    def run(self) -> None:
        try:
            source = self.engine.load_source(self.path)
        except Exception as exc:  # noqa: BLE001 - reported to the UI
            self.signals.failed.emit(self.path, str(exc))
        else:
            self.signals.loaded.emit(self.path, source)


class SourceLoader:
    """Decoding audio files. Part of EngineBridge (engine_bridge/__init__.py)."""

    def source(self, path: str) -> ge.AudioSource | None:
        return self._sources.get(_key(path))

    def is_loading(self, path: str) -> bool:
        return _key(path) in self._loading

    def load_error(self, path: str) -> str | None:
        return self._failed.get(_key(path))

    def request_source(self, path: str, then: Callable[[], None] | None = None) -> None:
        """Decode `path` in the background (once); `then` runs on the UI thread when ready."""
        key = _key(path)
        source = self._sources.get(key)
        if source is not None and source.sample_rate == int(self.engine.sample_rate):
            if then:
                then()
            return
        if key in self._loading:
            if then:
                self._loading[key].append(then)
            return
        self._failed.pop(key, None)
        self._loading[key] = [then] if then else []
        self._pool.start(_LoadTask(self.engine, path, self._load_signals))

    def _on_loaded(self, path: str, source: ge.AudioSource) -> None:
        key = _key(path)
        self._sources[key] = source
        callbacks = self._loading.pop(key, [])
        self.source_ready.emit(path)
        for callback in callbacks:
            callback()

    def _on_failed(self, path: str, message: str) -> None:
        key = _key(path)
        self._loading.pop(key, None)
        self._failed[key] = message
        self.source_failed.emit(path, message)
        self.status_message.emit(message)

    def file_info(self, path: str) -> ge.AudioFileInfo | None:
        """Length/format from the file header (cached). None if unreadable."""
        key = _key(path)
        if key not in self._file_info:
            try:
                self._file_info[key] = ge.probe_file(path)
            except RuntimeError as exc:
                self.status_message.emit(str(exc))
                return None
        return self._file_info[key]

    def refresh_sources(self) -> None:
        """After a sample-rate change: re-fetch every source used by the project."""
        stale = list(self._sources)
        self._sources.clear()
        for key in stale:
            self.request_source(key)
