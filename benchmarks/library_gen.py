"""Synthetic sample libraries for the browser benchmarks.

Shaped like real sample collections: packs, categories and sub-folders, 20-150
files a folder, names made of instrument, character, key and tempo words with
the separators people use, mostly WAV with some FLAC and MP3, and the files that
sit next to samples (Ableton .asd analysis files, artwork, readmes). The files
are empty: the browser reads names, never contents.

A library is made once per (size, seed) and reused; `LIBRARY.txt` in its root
says what it holds."""

from __future__ import annotations

import os
import random
import tempfile
from pathlib import Path

INSTRUMENTS = ["Kick", "Snare", "Clap", "Hat", "Open Hat", "Closed Hat", "Tom", "Perc", "Shaker", "Ride",
               "Crash", "Bass", "808", "Sub", "Lead", "Pad", "Pluck", "Chord", "Stab", "FX", "Riser", "Impact",
               "Vox", "Vocal Chop", "Loop", "Fill", "Break", "Top Loop", "Arp", "Keys", "Piano", "Guitar",
               "Strings", "Brass", "Synth", "Rim", "Snap", "Cowbell", "Conga", "Bongo"]
CHARACTER = ["Deep", "Tight", "Punchy", "Dark", "Bright", "Warm", "Dirty", "Clean", "Analog", "Vinyl", "Lofi",
             "Hard", "Soft", "Wide", "Short", "Long", "Big", "Small", "Crispy", "Fat", "Dusty", "Metallic",
             "Airy", "Gritty", "Smooth", "Distorted", "Reversed", "Layered", "Processed", "Dry"]
KEYS = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
GENRES = ["House", "Techno", "Trap", "Hip Hop", "DnB", "Lofi", "Ambient", "Pop", "RnB", "Dubstep", "Garage",
          "Afro", "Latin", "Cinematic", "Synthwave", "Jazz"]
CATEGORIES = ["Drums", "One Shots", "Loops", "Bass", "Melodic", "FX", "Vocals", "Textures"]
SUBFOLDERS = ["Kicks", "Snares", "Claps", "Hats", "Percussion", "Cymbals", "Toms", "Top Loops", "Full Loops",
              "Bass Loops", "Bass Shots", "Chords", "Leads", "Pads", "Plucks", "Risers", "Impacts", "Sweeps",
              "Chops", "Phrases", "Atmos", "Foley"]
SEPARATORS = ["_", " ", "-", " - "]
AUDIO = [(".wav", 80), (".mp3", 9), (".flac", 8), (".wave", 1), (".WAV", 2)]
EXTRAS = ["Readme.txt", "Artwork.png", "License.pdf", "Cover.jpg"]


def sample_name(rng: random.Random, number: int) -> str:
    sep = rng.choice(SEPARATORS)
    words = []
    if rng.random() < 0.3:
        words.append(rng.choice(GENRES).replace(" ", ""))
    if rng.random() < 0.6:
        words.append(rng.choice(CHARACTER))
    words.append(rng.choice(INSTRUMENTS))
    if rng.random() < 0.35:
        words.append(rng.choice(KEYS) + rng.choice(["", "m", "min", "maj"]))
    if rng.random() < 0.3:
        words.append(f"{rng.randint(70, 175)}{rng.choice(['', 'bpm', 'BPM'])}")
    words.append(f"{number:02d}")
    ext = rng.choices([e for e, _ in AUDIO], weights=[w for _, w in AUDIO])[0]
    return sep.join(words) + ext


def default_root(n_audio: int, seed: int) -> Path:
    return Path(tempfile.gettempdir()) / "sub-browser-bench" / f"lib-{n_audio}-{seed}"


def make_library(n_audio: int, seed: int = 1, root: Path | None = None) -> Path:
    """A library of about `n_audio` audio files (and ~40 % as many other files)."""
    root = Path(root) if root is not None else default_root(n_audio, seed)
    marker = root / "LIBRARY.txt"
    description = f"SUBstation browser benchmark library: {n_audio} audio files, seed {seed}\n"
    if marker.exists() and marker.read_text(encoding="utf-8") == description:
        return root
    rng = random.Random(seed)
    made = 0
    pack = 0
    while made < n_audio:
        pack += 1
        pack_dir = root / f"Pack {pack:03d} - {rng.choice(CHARACTER)} {rng.choice(GENRES)}"
        for extra in rng.sample(EXTRAS, rng.randint(1, len(EXTRAS))):
            _touch(pack_dir / extra)
        for category in rng.sample(CATEGORIES, rng.randint(2, 5)):
            for sub in rng.sample(SUBFOLDERS, rng.randint(1, 4)):
                folder = pack_dir / category / sub
                count = min(rng.randint(20, 150), n_audio - made)
                for i in range(count):
                    name = sample_name(rng, i + 1)
                    _touch(folder / name)
                    if rng.random() < 0.3:  # Ableton's analysis file next to it
                        _touch(folder / (name + ".asd"))
                made += count
                if made >= n_audio:
                    break
            if made >= n_audio:
                break
    marker.write_text(description, encoding="utf-8")
    return root


def _touch(path: Path) -> None:
    try:
        os.close(os.open(path, os.O_CREAT | os.O_WRONLY))
    except FileNotFoundError:
        path.parent.mkdir(parents=True, exist_ok=True)
        os.close(os.open(path, os.O_CREAT | os.O_WRONLY))


if __name__ == "__main__":
    import sys
    import time

    size = int(sys.argv[1]) if len(sys.argv) > 1 else 100_000
    start = time.perf_counter()
    print(make_library(size), f"{time.perf_counter() - start:.1f} s")
