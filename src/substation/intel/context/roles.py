"""What a track is for: its role (kick, bass, pad, vocal...), from the evidence
it gives, kept so the classifier can explain itself.

Evidence, strongest first:
- file-name tokens: audio clips' files, a Sampler's sample, the samples of a
  rack's chains (sample packs name things well), cleaned of tempo, key, pack
  prefixes and numbering (clean_name);
- devices: a plug-in's VST3 category, name and vendor; a rack whose chains are
  drums;
- MIDI: notes on the GM drum map, a low line one note at a time (bass), wide
  chords (keys, pad).
(Audio features join in with analysis, later.)

Pure functions of the model: no Qt, no engine."""

from __future__ import annotations

import re
import statistics
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from ...model import device_state, keys
from ...model.project import MidiClip, Track, iter_devices

ROLES = ("kick", "snare", "clap", "hats", "perc", "drums", "bass", "sub", "lead", "pad", "keys", "chords",
         "pluck", "vocal", "fx", "texture", "other")
DRUM_ROLES = frozenset({"kick", "snare", "clap", "hats", "perc", "drums"})

# File-name and device-name tokens (lower case) -> role.
TOKENS = {
    "kick": "kick", "kik": "kick", "kck": "kick", "bd": "kick", "bassdrum": "kick",
    "snare": "snare", "snr": "snare", "sd": "snare",
    "clap": "clap", "clp": "clap", "claps": "clap",
    "hat": "hats", "hats": "hats", "hh": "hats", "hihat": "hats", "hihats": "hats", "oh": "hats", "ohh": "hats",
    "chh": "hats", "ride": "hats", "cymbal": "hats",
    "perc": "perc", "percs": "perc", "percussion": "perc", "shaker": "perc", "tamb": "perc",
    "tambourine": "perc", "conga": "perc", "bongo": "perc", "rim": "perc", "rimshot": "perc", "cowbell": "perc",
    "tom": "perc", "toms": "perc", "snap": "perc", "click": "perc",
    "drum": "drums", "drums": "drums", "kit": "drums", "break": "drums", "breaks": "drums", "breakbeat": "drums",
    "top": "drums", "tops": "drums", "groove": "drums", "fill": "drums", "beat": "drums",
    "bass": "bass", "bs": "bass", "bassline": "bass", "reese": "bass",
    "sub": "sub", "808": "sub", "subbass": "sub",
    "lead": "lead", "ld": "lead", "solo": "lead", "melody": "lead", "hook": "lead",
    "pad": "pad", "pads": "pad", "strings": "pad", "string": "pad", "choir": "pad",
    "keys": "keys", "piano": "keys", "rhodes": "keys", "organ": "keys", "epiano": "keys", "wurli": "keys",
    "chord": "chords", "chords": "chords", "chd": "chords", "stab": "chords", "stabs": "chords",
    "pluck": "pluck", "plucks": "pluck", "pl": "pluck", "arp": "pluck",
    "vox": "vocal", "vocal": "vocal", "vocals": "vocal", "voc": "vocal", "acapella": "vocal", "acappella": "vocal",
    "adlib": "vocal", "adlibs": "vocal", "voice": "vocal", "chop": "vocal", "chops": "vocal",
    "fx": "fx", "sfx": "fx", "riser": "fx", "uplifter": "fx", "downlifter": "fx", "sweep": "fx", "impact": "fx",
    "whoosh": "fx", "transition": "fx", "crash": "fx", "reverse": "fx",
    "texture": "texture", "atmos": "texture", "atmosphere": "texture", "ambience": "texture", "ambient": "texture",
    "drone": "texture", "foley": "texture", "noise": "texture",
}

# How much each kind of evidence counts.
NAME_WEIGHT = 0.6  # sample packs name things well
DEVICE_WEIGHT = 0.35
MIDI_WEIGHT = 0.35
TRACK_NAME_WEIGHT = 0.2  # what someone called it (or a file's name it got)

GM_DRUMS = frozenset({35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
                      59, 69, 70, 75, 76, 77, 81})
DEFAULT_NAME = re.compile(r"^\d+ (Audio|MIDI|Group)$|^[A-Z]+ Return$")


@dataclass(frozen=True)
class Role:
    label: str
    confidence: float
    evidence: tuple[str, ...] = ()

    def to_dict(self) -> dict:
        return {"label": self.label, "confidence": round(self.confidence, 2), "evidence": list(self.evidence)}


UNKNOWN = Role("other", 0.0)


# --- Names --------------------------------------------------------------------------------


def _words(text: str) -> list[str]:
    text = re.sub(r"([a-z])([A-Z])", r"\1 \2", text)  # camelCase
    return [w for w in re.split(r"[\s_\-.,()\[\]{}+&]+", text) if w]


def clean_name(name: str) -> str:
    """A file's (or preset's) name without its extension, tempo, key, pack prefix
    and numbering: "Bass_Loop_128_Am.wav" -> "Bass Loop", "KSHMR_Kick_07" -> "Kick"."""
    stem = re.sub(r"\.[A-Za-z0-9]{2,4}$", "", Path(name).name)
    info = keys.parse_filename(stem)
    words = _words(stem)
    kept = []
    for i, word in enumerate(words):
        lower = word.lower()
        if re.fullmatch(r"\d+(\.\d+)?(bpm)?|bpm\d+|bpm|v\d+|#\d+", lower):
            continue  # numbering, tempo
        if info.key is not None and re.fullmatch(r"[A-G](#|b)?(m|min|maj|minor|major)?", word):
            continue  # the key
        if lower in ("min", "maj", "minor", "major", "key", "loop", "one", "shot", "oneshot") and i > 0 \
                and lower != "loop":
            continue
        kept.append(word)
    # A pack prefix: an all-capitals first word that says nothing about the sound.
    if len(kept) > 1 and kept[0].isupper() and len(kept[0]) >= 2 and kept[0].lower() not in TOKENS:
        kept = kept[1:]
    return " ".join(w if not w.isupper() or len(w) <= 3 else w.capitalize() for w in kept)


def name_roles(name: str) -> list[str]:
    """The roles a name's words point at, in order."""
    return [TOKENS[w.lower()] for w in _words(clean_name(name)) if w.lower() in TOKENS]


# --- MIDI -----------------------------------------------------------------------------------


@dataclass(frozen=True)
class MidiStats:
    notes: int
    pitch_min: int
    pitch_max: int
    pitch_median: float
    polyphony: float  # notes sounding at once, on average over the time any sounds
    notes_per_onset: float  # notes starting together (within 1/32 beat), on average
    density: float  # notes per beat, over the span they play in
    median_length: float  # beats
    drum_map: float  # the share of notes on the GM drum map's drum keys
    distinct_pitches: int

    def to_dict(self) -> dict:
        return {k: round(v, 3) if isinstance(v, float) else v for k, v in self.__dict__.items()}


def played_notes(clips: Iterable) -> list[tuple[float, float, int]]:
    """(start, end, pitch) in timeline beats of every note the MIDI clips play
    (only those inside each clip's window: a clip is a window onto its notes)."""
    played = []
    for clip in clips:
        if isinstance(clip, MidiClip):
            played += [(start, end, note.pitch) for start, end, note in clip.played_notes()]
    return sorted(played)


def midi_stats(clips: Iterable) -> MidiStats | None:
    notes = played_notes(clips)
    if not notes:
        return None
    pitches = [p for _, _, p in notes]
    lengths = [max(0.0, e - s) for s, e, _ in notes]
    span = max(e for _, e, _ in notes) - min(s for s, _, _ in notes)
    sounding = sum(lengths)
    edges = sorted([(s, 1) for s, _, _ in notes] + [(e, -1) for _, e, _ in notes])
    covered, active, last = 0.0, 0, edges[0][0]
    for beat, step in edges:
        if active > 0:
            covered += beat - last
        active += step
        last = beat
    onsets: list[int] = []
    previous = None
    for start, _, _ in notes:
        if previous is not None and start - previous <= 1 / 32:
            onsets[-1] += 1
        else:
            onsets.append(1)
            previous = start
    return MidiStats(
        notes=len(notes), pitch_min=min(pitches), pitch_max=max(pitches), pitch_median=statistics.median(pitches),
        polyphony=sounding / covered if covered > 0 else 1.0, notes_per_onset=len(notes) / len(onsets),
        density=len(notes) / span if span > 0 else float(len(notes)), median_length=statistics.median(lengths),
        drum_map=sum(p in GM_DRUMS for p in pitches) / len(pitches), distinct_pitches=len(set(pitches)))


def midi_role(stats: MidiStats) -> tuple[str, float, str] | None:
    """(role, confidence, why) from how the notes play, if they say anything."""
    if stats.drum_map >= 0.8 and stats.distinct_pitches <= 10 and stats.median_length <= 0.5 \
            and stats.pitch_max <= 81:
        return "drums", 0.7, f"notes on drum keys ({stats.distinct_pitches} keys, short)"
    if stats.notes_per_onset >= 2.5 or stats.polyphony >= 2.5:
        if stats.median_length >= 2.0:
            return "pad", 0.6, f"chords of {stats.notes_per_onset:.0f} held long"
        return "chords", 0.6, f"chords of {stats.notes_per_onset:.0f}"
    if stats.polyphony <= 1.3 and stats.pitch_median < 48:  # below C2 (60 = C3)
        if stats.pitch_median < 36 and stats.median_length >= 1.0:
            return "sub", 0.6, "a low line, long notes, one at a time"
        return "bass", 0.65, "a low line, one note at a time"
    if stats.polyphony <= 1.3 and stats.pitch_median >= 60:
        return "lead", 0.4, "a line one note at a time, middle or high"
    if stats.polyphony > 1.3:
        return "keys", 0.35, "several notes at once"
    return None


# --- The classifier ---------------------------------------------------------------------------


def _sample_paths(track: Track) -> list[tuple[str, str]]:
    """(where, file name) of the samples a track plays: its audio clips' files, its
    Samplers' samples (in racks too). Frozen audio isn't evidence: the layer made it."""
    found = []
    frozen = track.frozen.path if track.frozen is not None else None
    for clip in track.clips:
        path = getattr(clip, "path", None)
        if path and path != frozen:
            found.append(("file", Path(path).name))
    for device in iter_devices(track.devices):
        if device.kind == "sampler":
            sample = device_state.from_model(device.state).get("sample")
            if sample:
                found.append(("sample", Path(sample).name))
    return found


def _device_evidence(track: Track) -> list[tuple[str, float, str]]:
    evidence = []
    for device in iter_devices(track.devices):
        if not device.enabled:
            continue
        if device.plugin is not None:
            for role in name_roles(device.plugin.name):
                evidence.append((role, DEVICE_WEIGHT, f"plug-in {device.plugin.name}"))
        if device.is_rack:
            chain_roles = {r for chain in device.chains for r in name_roles(chain.name)}
            for chain in device.chains:
                for inner in iter_devices(chain.devices):
                    if inner.kind == "sampler":
                        sample = device_state.from_model(inner.state).get("sample")
                        if sample:
                            chain_roles.update(name_roles(Path(sample).name))
            if len(chain_roles & DRUM_ROLES) >= 2:
                evidence.append(("drums", DEVICE_WEIGHT + 0.25, "a rack of drum sounds"))
    return evidence


def classify(track: Track, plugin_categories: dict[str, str] | None = None) -> Role:
    """A track's role from its evidence. `plugin_categories`: plug-in uid ->
    its VST3 sub-categories (from the scan), where known."""
    if not track.has_clips and not track.devices:
        return UNKNOWN
    scores: dict[str, float] = {}
    evidence: list[str] = []

    def add(role: str, weight: float, why: str) -> None:
        scores[role] = scores.get(role, 0.0) + weight
        evidence.append(f"{why} → {role}")

    samples = _sample_paths(track)
    by_role: dict[str, list[str]] = {}
    for _where, name in samples:
        roles = name_roles(name)
        if roles:
            by_role.setdefault(roles[0], []).append(name)
    if by_role:
        total = sum(len(v) for v in by_role.values())
        drum_kinds = [r for r in by_role if r in DRUM_ROLES]
        if len(drum_kinds) >= 2 and sum(len(by_role[r]) for r in drum_kinds) >= total * 0.6:
            add("drums", NAME_WEIGHT, f"drum samples ({', '.join(sorted(drum_kinds))})")
        else:
            for role, names in by_role.items():
                share = len(names) / total
                example = ", ".join(names[:3]) + (", ..." if len(names) > 3 else "")
                add(role, NAME_WEIGHT * share, f"file name {example}")
    for role, weight, why in _device_evidence(track):
        add(role, weight, why)
    for device in iter_devices(track.devices):
        if device.plugin is None or not device.enabled:
            continue
        category = (plugin_categories or {}).get(device.plugin.uid, "")
        if "Drum" in category:
            add("drums", DEVICE_WEIGHT, f"{device.plugin.name} is a drum instrument")
        elif "Piano" in category:
            add("keys", DEVICE_WEIGHT, f"{device.plugin.name} is a piano")
    if track.is_midi:
        stats = midi_stats(track.clips)
        if stats is not None:
            found = midi_role(stats)
            if found is not None:
                role, confidence, why = found
                add(role, MIDI_WEIGHT * confidence / 0.7, f"MIDI: {why}")
    if not DEFAULT_NAME.match(track.name):
        for role in dict.fromkeys(name_roles(track.name)):
            add(role, TRACK_NAME_WEIGHT, f"track name {track.name}")
    if not scores:
        return UNKNOWN
    label = max(scores, key=lambda r: (scores[r], -ROLES.index(r)))
    # As strong as its evidence, and less so as other roles have some too.
    confidence = min(0.99, scores[label]) * scores[label] / sum(scores.values())
    return Role(label, round(confidence, 3), tuple(evidence))
