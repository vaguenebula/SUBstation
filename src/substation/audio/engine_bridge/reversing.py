"""Reversing audio: a reversed copy of a file, written as a WAV file in the
reversed folder (the project's "Reversed" folder once it is saved), for
reversed clips to play (as Ableton does). The copy is written a chunk at a
time on a thread of its own (ReverseJob), so a long file neither holds up the
window nor needs a second copy of itself in memory."""

from __future__ import annotations

import struct
import threading
from pathlib import Path

import numpy as np

from ... import _engine as ge
from ...model.project import Clip, Project
from .recording import recordings_folder
from .sources import _key

REVERSE_CHUNK = 1 << 18  # frames written at a time


def reversed_folder(project: Project) -> Path:
    """Where reversed copies go: the project's "Reversed" folder once it is
    saved, else a "Reversed" folder in the recordings folder (see recordings_folder)."""
    if project.path is not None:
        return Path(project.path).parent / "Reversed"
    return recordings_folder(project) / "Reversed"


def reversed_path(folder: Path, source: str) -> Path:
    """A new file for the reversed copy of `source`: its name and " R" (numbered if taken)."""
    stem = Path(source).stem
    path, n = folder / f"{stem} R.wav", 2
    while path.exists():
        path, n = folder / f"{stem} R {n}.wav", n + 1
    return path


def float_wav_header(channels: int, frames: int, sample_rate: int) -> bytes:
    """The header of a 32-bit float WAV file of `frames` frames."""
    block = 4 * channels
    size = frames * block
    return b"".join([
        b"RIFF", struct.pack("<I", 4 + (8 + 18) + (8 + 4) + (8 + size)), b"WAVE",
        b"fmt ", struct.pack("<IHHIIHHH", 18, 3, channels, sample_rate, sample_rate * block, block, 32, 0),
        b"fact", struct.pack("<II", 4, frames),
        b"data", struct.pack("<I", size),
    ])


def write_float_wav(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    """(channels, frames) samples as a 32-bit float WAV file."""
    channels, frames = samples.shape
    with open(path, "wb") as file:
        file.write(float_wav_header(channels, frames, sample_rate))
        file.write(np.ascontiguousarray(samples.T, dtype="<f4").tobytes())


class ReverseJob:
    """A reversed copy of a decoded file being written on a thread of its own
    (ReverseSync.start_reversed), from its end a chunk at a time, then decoded.
    Followed as the engine's RenderJob is: `progress`, `done`, `cancel()`."""

    def __init__(self, engine: ge.Engine, source: ge.AudioSource, path: Path):
        self.path = str(path)
        self.frames = source.frames
        self.seconds = source.frames / source.sample_rate
        self._engine = engine
        self._source = source
        self._written = 0
        self._cancel = threading.Event()
        self._done = threading.Event()
        self._loaded: ge.AudioSource | None = None
        self._error: Exception | None = None
        self._thread = threading.Thread(target=self._run, name="reverse", daemon=True)
        self._thread.start()

    @property
    def progress(self) -> float:
        """How far it got: 0..1 (writing is most of it; decoding the copy the rest)."""
        return 1.0 if self.done else 0.9 * self._written / max(1, self.frames)

    @property
    def done(self) -> bool:
        return self._done.is_set()

    @property
    def cancelled(self) -> bool:
        return self._cancel.is_set()

    def cancel(self) -> None:
        """Stops it soon; its file goes."""
        self._cancel.set()

    def finish(self) -> ge.AudioSource | None:
        """Waits for it: the copy, decoded, or None if it was cancelled (its file
        gone). Raises OSError if it couldn't be written."""
        self._thread.join()
        if self._error is not None:
            raise OSError(f"Could not write {Path(self.path).name}: {self._error}") from self._error
        return self._loaded

    def _run(self) -> None:
        target = Path(self.path)
        source = self._source
        try:
            with open(target, "wb") as file:
                file.write(float_wav_header(source.channels, self.frames, int(source.sample_rate)))
                end = self.frames
                while end > 0 and not self.cancelled:
                    start = max(0, end - REVERSE_CHUNK)
                    chunk = np.asarray(source.samples(start, end - start))[:, ::-1]
                    file.write(np.ascontiguousarray(chunk.T, dtype="<f4").tobytes())
                    self._written += end - start
                    end = start
            if not self.cancelled:
                self._loaded = self._engine.load_source(self.path)
        except (OSError, RuntimeError, ValueError) as exc:
            self._error = exc
        finally:
            if self._loaded is None:  # (cancelled or failed: what was written of it goes)
                target.unlink(missing_ok=True)
            self._done.set()


class ReverseSync:
    """Reversed copies of files. Part of EngineBridge (engine_bridge/__init__.py)."""

    def reversed_copy(self, path: str) -> str | None:
        """The reversed copy of `path` there is already, if any: one made this
        session, or one a clip of the project plays (saved with it). Not decoded
        yet, it is asked for (request_source)."""
        copy = self._reversed.get(_key(path))
        if copy is None or not Path(copy).is_file():
            copy = next((clip.path for track in self.project.tracks for clip in track.clips
                         if isinstance(clip, Clip) and clip.reversed_from
                         and _key(clip.reversed_from) == _key(path) and Path(clip.path).is_file()), None)
        if copy is None:
            return None
        self._reversed[_key(path)] = copy
        if self.source(copy) is None:
            self.request_source(copy)
        return copy

    def start_reversed(self, path: str) -> ReverseJob:
        """Starts writing a reversed copy of `path` (decoded already: see source())
        as a new WAV file in the reversed folder; finish_reversed takes it. Raises
        ValueError (with a message for the user) if `path` isn't decoded."""
        source = self.source(path)
        if source is None:
            reason = "can't be found" if self.load_error(path) else "is still loading"
            raise ValueError(f"{Path(path).name} {reason}: it can't be reversed")
        folder = reversed_folder(self.project)
        folder.mkdir(parents=True, exist_ok=True)
        return ReverseJob(self.engine, source, reversed_path(folder, path))

    def finish_reversed(self, path: str, job: ReverseJob) -> tuple[str, float] | None:
        """A reversed copy of `path` written: its path and its length in seconds,
        decoded (so that it plays without a gap), and given again for this file
        from now on. None if it was cancelled. Raises OSError if it failed."""
        loaded = job.finish()
        if loaded is None:
            return None
        self._sources[_key(job.path)] = loaded
        self._reversed[_key(path)] = job.path
        self.source_ready.emit(job.path)
        return job.path, job.seconds

    def render_reversed(self, path: str) -> tuple[str, float]:
        """A reversed copy of `path`, waited for: the one there is already
        (reversed_copy), or a new one (start_reversed). Returns its path and its
        length in seconds; raises as start_reversed and finish_reversed do."""
        copy = self.reversed_copy(path)
        source = self.source(path)
        if copy is not None and source is not None:
            return copy, source.frames / source.sample_rate
        job = self.start_reversed(path)
        return self.finish_reversed(path, job)
