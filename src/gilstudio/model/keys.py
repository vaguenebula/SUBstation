"""Musical keys, and reading a sample's tempo and key from its file name.

Sample packs name loops like `Pack_Bass_Loop_128_Am.wav`, `Keys 92bpm F# minor`
or `Vox_Ebmaj_140BPM`. `parse_filename` finds the tempo and key in such a name;
`clip_settings` turns them into what a dropped clip starts with: warped at that
tempo, and transposed to the project's key.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

NOTE_NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
_TONICS = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

# Audio at least this long is warped even when its name gives no tempo (it is
# then taken to be at the project tempo, as when Warp is turned on by hand).
# Shorter files without a tempo in their name are one-shots and play as they are.
AUTO_WARP_MIN_SEC = 6.0
MIN_BPM, MAX_BPM = 50.0, 250.0  # a tempo in a name outside this is some other number


@dataclass(frozen=True)
class Key:
    tonic: int  # pitch class, 0 = C
    minor: bool = False

    @property
    def name(self) -> str:
        return NOTE_NAMES[self.tonic] + ("m" if self.minor else "")

    @property
    def label(self) -> str:
        return f"{NOTE_NAMES[self.tonic]} {'Minor' if self.minor else 'Major'}"

    def relative_major(self) -> int:
        """The tonic of the major key with the same notes (A minor: C)."""
        return (self.tonic + 3) % 12 if self.minor else self.tonic


# Every key, in the order the key chooser lists them: C, Cm, C#, C#m, ...
ALL_KEYS = [Key(tonic, minor) for tonic in range(12) for minor in (False, True)]


def key_from_name(text: str | None) -> Key | None:
    """A key as Key.name writes it ("F#m", "Bb"), as saved in projects."""
    if not text:
        return None
    match = re.fullmatch(r"([A-G])([#b]?)(m?)", text.strip())
    if not match:
        return None
    letter, accidental, minor = match.groups()
    return Key((_TONICS[letter] + {"#": 1, "b": -1}.get(accidental, 0)) % 12, bool(minor))


def transpose_to(source: Key | None, target: Key | None) -> int:
    """Semitones that bring audio in `source` into `target` (0 if either is
    unknown). A minor key and its relative major share their notes, so a loop
    in A minor needs no shift in C major. The shift is the smallest one, down
    rather than up at a tritone (shifting down sounds more natural)."""
    if source is None or target is None:
        return 0
    shift = (target.relative_major() - source.relative_major()) % 12
    return shift - 12 if shift >= 6 else shift


# --- Reading file names ------------------------------------------------------------

_SEP = r"(?<![A-Za-z0-9])"  # not in the middle of a word or number
_END = r"(?![A-Za-z0-9])"
_NUMBER = r"(\d{2,3}(?:\.\d{1,2})?)"
# "128bpm", "128 BPM", "bpm128", "BPM_128"
_BPM_TAGGED = re.compile(rf"{_SEP}{_NUMBER}[\s_-]?bpm{_END}|{_SEP}bpm[\s_-]?{_NUMBER}{_END}", re.IGNORECASE)
# A number standing on its own: "Loop_128_Am"
_BPM_BARE = re.compile(rf"{_SEP}{_NUMBER}{_END}")
_ACCIDENTAL = r"(#|♯|b|♭|sharp|flat|s(?=[\s_-]*(?:maj|min|m)))?"
# A tonic with a spelled-out mode: "A minor", "Ebmaj", "f# min", "Cmaj7". The letter may be lower case.
_KEY_SPELLED = re.compile(rf"{_SEP}([A-Ga-g]){_ACCIDENTAL}[\s_-]?(major|minor|maj|min)(?![A-Za-z])", re.IGNORECASE)
# Short forms, upper-case tonic only (so words like "am" don't count): "Am", "F#m", "C#", "D".
_KEY_SHORT = re.compile(rf"{_SEP}([A-G])(#|♯|b|♭)?(m)?{_END}")
# Words for a key: "key of A", "Key_Am"
_KEY_WORD = re.compile(r"key", re.IGNORECASE)


@dataclass(frozen=True)
class FileInfo:
    bpm: float | None = None
    key: Key | None = None


def _bpm(number: str) -> float | None:
    value = float(number)
    return value if MIN_BPM <= value <= MAX_BPM else None


def _key(letter: str, accidental: str | None, minor: bool) -> Key:
    shift = 0
    if accidental:
        accidental = accidental.lower()
        shift = 1 if accidental in ("#", "♯", "sharp", "s") else -1
    return Key((_TONICS[letter.upper()] + shift) % 12, minor)


def parse_filename(name: str) -> FileInfo:
    """The tempo and key a file name gives, as far as it gives them."""
    stem = re.sub(r"\.[A-Za-z0-9]{2,4}$", "", name)

    bpm = None
    tagged_span = None
    for match in _BPM_TAGGED.finditer(stem):
        bpm = _bpm(match.group(1) or match.group(2))
        if bpm is not None:
            tagged_span = match.span()
            break
    if bpm is None:
        # A bare number counts if it is the only one in tempo range, so a take
        # or version number next to it ("Loop 2 120") doesn't confuse it.
        bare = [b for b in (_bpm(m.group(1)) for m in _BPM_BARE.finditer(stem)) if b is not None]
        if len(bare) == 1:
            bpm = bare[0]

    key = None
    spelled = list(_KEY_SPELLED.finditer(stem))
    if spelled:
        match = spelled[-1]  # names put the key last more often than not
        key = _key(match.group(1), match.group(2), match.group(3).lower().startswith("min"))
    else:
        searchable = stem if tagged_span is None else stem[:tagged_span[0]] + " " + stem[tagged_span[1]:]
        candidates = []
        for match in _KEY_SHORT.finditer(searchable):
            letter, accidental, minor = match.groups()
            explicit = bool(accidental or minor)
            after_key_word = bool(_KEY_WORD.search(searchable[max(0, match.start() - 5):match.start()]))
            # A bare capital ("A", "D") is too common a word to trust alone: take it
            # only next to a tempo or after "key", and never as the first word ("A Day").
            if not explicit and not after_key_word and (bpm is None or match.start() == 0):
                continue
            candidates.append((explicit, _key(letter, accidental, bool(minor))))
        if candidates:
            explicit = [k for e, k in candidates if e]
            key = (explicit or [k for _, k in candidates])[-1]
    return FileInfo(bpm, key)


def clip_settings(name: str, duration_sec: float, tempo: float, project_key: Key | None) -> dict:
    """What a newly added audio clip starts with, from its file name and length:
    warped at the tempo in its name (or, if it's long, at the project tempo, so
    it plays as it is until the tempo changes), and transposed from the key in
    its name to the project's. Fields for Clip(...); empty when the file plays
    as it is."""
    info = parse_filename(name)
    settings: dict = {}
    if info.bpm is not None or duration_sec >= AUTO_WARP_MIN_SEC:
        settings["warp"] = True
        settings["segment_bpm"] = info.bpm if info.bpm is not None else tempo
    transpose = transpose_to(info.key, project_key)
    if transpose:
        settings["transpose"] = transpose
    return settings
