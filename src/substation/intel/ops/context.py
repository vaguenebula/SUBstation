"""What an operation runs with (OpContext), who runs it (actors), and the
argument types and lookups operations share."""

from __future__ import annotations

import os
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Annotated

from ...model.editor import ProjectEditor
from ...model.project import AnyClip, Device, MidiClip, Project, Track, find_device
from ..context.song import build_context
from ..facts import EngineFacts, UiFacts
from .errors import DENIED, FROZEN, OpError, invalid, not_found
from .registry import Doc, Ge, MaxLen, MinLen, Pattern

# --- Actors --------------------------------------------------------------------------------

USER = "user"
AUTO = "auto"  # the autonomy policy
ASSISTANT = "assistant"


def agent(client: str) -> str:
    """An outside agent, by its client's name ("Claude Code")."""
    return f"agent:{client}"


def actor_prefix(actor: str) -> str | None:
    """What an actor's undo steps start with ("Agent (Claude Code)"); None for the user."""
    if actor == USER:
        return None
    if actor == AUTO:
        return "Auto"
    if actor == ASSISTANT:
        return "Assistant"
    if actor.startswith("agent:"):
        return f"Agent ({actor[len('agent:'):]})"
    return actor


def undo_label(actor: str, label: str) -> str:
    prefix = actor_prefix(actor)
    return label if prefix is None else f"{prefix}: {label}"


# --- Argument types ------------------------------------------------------------------------

TrackId = Annotated[str, Doc("A track's id (a group's, a return's); 'master' for the master where it fits."),
                    MinLen(1), MaxLen(64)]
OwnerId = Annotated[str, Doc("An automation owner: a track's (group's, return's) id, or 'master'."),
                    MinLen(1), MaxLen(64)]
DeviceId = Annotated[str, Doc("A device's id (unique in the project, in racks too)."), MinLen(1), MaxLen(64)]
ClipId = Annotated[str, Doc("A clip's id (unique in the project)."), MinLen(1), MaxLen(64)]
Beat = Annotated[float, Doc("A position in quarter-note beats from the timeline's start."), Ge(0)]
Beats = Annotated[float, Doc("A length in quarter-note beats."), Ge(0)]
TrackName = Annotated[str, Doc("A name, as shown in the track header."), MinLen(1), MaxLen(64)]
Color = Annotated[str, Doc("A colour as #rrggbb."), Pattern(r"#[0-9a-fA-F]{6}")]
NoteRow = Annotated[list[float], Doc("A note: [pitch (0-127, 60 = C3), start (beats from the clip's "
                                     "start), length (beats), velocity (1-127, default 100)]."),
                    MinLen(3), MaxLen(4)]
PointRow = Annotated[list[float], Doc("A breakpoint: [beat, value, curve (-1..1, default 0)]."), MinLen(2), MaxLen(3)]


# --- The context ---------------------------------------------------------------------------


@dataclass
class OpContext:
    project: Project
    editor: ProjectEditor
    actor: str = USER
    engine: EngineFacts | None = None
    ui: UiFacts | None = None
    revision: object | None = None  # IntelRevision
    selections: OrderedDict = field(default_factory=OrderedDict)  # selection id -> its description (pinned)
    builder: object | None = None  # a SongContextBuilder, kept up to date (else contexts are built afresh)

    def song(self):
        """The SongContext now."""
        if self.builder is not None:
            return self.builder.context()
        return build_context(self.project, revision=self.revision.value if self.revision is not None else 0)

    # --- Lookups (each raises OpError not_found) ---------------------------------------------

    def owner(self, owner_id: str) -> Track:
        """A track, group, return or the master."""
        if not self.project.has_owner(owner_id):
            raise not_found(f"There is no track {owner_id!r}", track_id=owner_id)
        return self.project.track(owner_id)

    def track(self, track_id: str) -> Track:
        """A track of the arrangement (not a return or the master)."""
        if not self.project.has_track(track_id):
            if self.project.has_owner(track_id):
                raise invalid(f"{self.project.track(track_id).name} isn't a track of the arrangement")
            raise not_found(f"There is no track {track_id!r}", track_id=track_id)
        return self.project.track(track_id)

    def device(self, track_id: str, device_id: str) -> Device:
        track = self.owner(track_id)
        device = find_device(track.devices, device_id)
        if device is None:
            raise not_found(f"{track.name} has no device {device_id!r}", device_id=device_id)
        return device

    def find_clip(self, clip_id: str) -> tuple[str, AnyClip]:
        for track in self.project.tracks:
            for clip in track.clips:
                if clip.id == clip_id:
                    return track.id, clip
        raise not_found(f"There is no clip {clip_id!r}", clip_id=clip_id)

    def midi_clip(self, clip_id: str) -> tuple[str, MidiClip]:
        track_id, clip = self.find_clip(clip_id)
        if not isinstance(clip, MidiClip):
            raise invalid(f"{clip.name} is an audio clip, not a MIDI clip")
        return track_id, clip

    # --- Checks ------------------------------------------------------------------------------

    def check_unfrozen(self, track_id: str, what: str = "devices") -> None:
        """Raises 'frozen' if this would change what a frozen track's audio holds."""
        p = self.project
        holder = p.frozen_by(track_id) if p.has_owner(track_id) else None
        if holder is not None:
            raise OpError(FROZEN, f"{p.track(holder).name} is frozen: unfreeze it to change its {what}",
                          track_id=holder)

    def check_path(self, path: str, extra_roots=()) -> Path:
        """A file the layer may read: under a browser place, the project's folder,
        or `extra_roots`. Raises 'denied' (outside them) or 'not_found'."""
        resolved = Path(path)
        if not resolved.is_absolute():
            raise invalid("A path must be absolute", path=path)
        roots = [Path(r) for r in (self.ui.places() if self.ui is not None else [])]
        if self.project.path is not None:
            roots.append(Path(self.project.path).parent)
        roots += [Path(r) for r in extra_roots]
        real = os.path.normcase(os.path.realpath(resolved))
        if not any(_is_under(real, os.path.normcase(os.path.realpath(r))) for r in roots):
            raise OpError(DENIED, f"{resolved.name} isn't in a browser place or the project's folder", path=path)
        if not resolved.is_file():
            raise not_found(f"There is no file {resolved}", path=path)
        return resolved

    # --- Results -----------------------------------------------------------------------------

    def ref(self, track_id: str) -> dict:
        track = self.project.track(track_id)
        return {"id": track.id, "name": track.name, "kind": track.kind}


def _is_under(path: str, root: str) -> bool:
    return path == root or path.startswith(root.rstrip("\\/") + os.sep)
