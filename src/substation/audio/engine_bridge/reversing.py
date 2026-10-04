"""Reversing audio: a reversed copy of a file, written as a WAV file in the
reversed folder (the project's "Reversed" folder once it is saved), for
reversed clips to play (as Ableton does)."""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np

from ...model.project import Project
from .recording import recordings_folder
from .sources import _key


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


def write_float_wav(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    """(channels, frames) samples as a 32-bit float WAV file."""
    channels, frames = samples.shape
    data = np.ascontiguousarray(samples.T, dtype="<f4").tobytes()
    block = 4 * channels
    header = b"".join([
        b"RIFF", struct.pack("<I", 4 + (8 + 18) + (8 + 4) + (8 + len(data))), b"WAVE",
        b"fmt ", struct.pack("<IHHIIHHH", 18, 3, channels, sample_rate, sample_rate * block, block, 32, 0),
        b"fact", struct.pack("<II", 4, frames),
        b"data", struct.pack("<I", len(data)),
    ])
    with open(path, "wb") as file:
        file.write(header)
        file.write(data)


class ReverseSync:
    """Reversed copies of files. Part of EngineBridge (engine_bridge/__init__.py)."""

    def render_reversed(self, path: str) -> tuple[str, float]:
        """A reversed copy of `path` (decoded already: see source()), as a new WAV
        file in the reversed folder, decoded at once so that it plays without a
        gap; the same file again if this file was reversed before in this
        session. Returns its path and its length in seconds. Raises ValueError
        (with a message for the user) if `path` isn't decoded, OSError if the
        copy can't be written."""
        source = self.source(path)
        if source is None:
            reason = "can't be found" if self.load_error(path) else "is still loading"
            raise ValueError(f"{Path(path).name} {reason}: it can't be reversed")
        done = self._reversed.get(_key(path))
        if done is not None and self.source(done) is not None:
            return done, self.source(done).frames / self.source(done).sample_rate
        folder = reversed_folder(self.project)
        folder.mkdir(parents=True, exist_ok=True)
        target = reversed_path(folder, path)
        samples = np.asarray(source.samples(0, source.frames))[:, ::-1]
        try:
            write_float_wav(target, samples, int(source.sample_rate))
            self._sources[_key(str(target))] = self.engine.load_source(str(target))
        except (OSError, RuntimeError) as exc:
            target.unlink(missing_ok=True)  # (what was written of it)
            raise OSError(f"Could not write {target.name}: {exc}") from exc
        self._reversed[_key(path)] = str(target)
        self.source_ready.emit(str(target))
        return str(target), source.frames / source.sample_rate
