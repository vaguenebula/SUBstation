"""Freezing: freezing, unfreezing and flattening tracks, and what frozen audio
holds, which can't change (the edits a frozen track, or a track in a frozen
group, refuses)."""

from __future__ import annotations

import copy
from dataclasses import replace

from .. import automation
from ..commands import (
    ReplaceTrackCommand,
    SetChainsCommand,
    SetClipsCommand,
    SetDeviceEnabledCommand,
    SetDeviceNameCommand,
    SetDeviceParamCommand,
    SetDeviceParamsCommand,
    SetDevicesCommand,
    SetDeviceSidechainCommand,
    SetDeviceStateCommand,
    SetEnvelopeCommand,
    SetEnvelopesCommand,
    SetFreezeCommand,
    SetMacrosCommand,
    UpdateChainCommand,
)
from ..project import Freeze, new_id


class FreezeEdits:
    """Freezing, unfreezing, flattening, and what frozen tracks refuse. Part of ProjectEditor (editor/__init__.py)."""

    def freeze_tracks(self, freezes: dict[str, Freeze]) -> list[str]:
        """Freezes tracks (and groups, returns) with their rendered audio
        (EngineBridge.render_freeze), one undo step; they are disarmed. Tracks in
        a group frozen with them, or that can't be frozen (freeze_problem), are
        left as they are. Returns those frozen."""
        p = self.project
        frozen = [t for t in freezes if p.has_owner(t) and p.freeze_problem(t) is None
                  and not (p.has_track(t) and any(a in freezes for a in p.ancestors(t)))]
        if not frozen:
            return []
        self.arm_tracks([t for t in frozen if p.has_track(t)], False)
        text = "Freeze Track" if len(frozen) == 1 else "Freeze Tracks"
        self.undo_stack.beginMacro(text)
        try:
            for track_id in frozen:
                self._push(SetFreezeCommand(p, track_id, None, freezes[track_id], text))
        finally:
            self.undo_stack.endMacro()
        return frozen

    def unfreeze_tracks(self, track_ids) -> list[str]:
        """Unfreezes tracks, one undo step: their devices load again. (A track in a
        frozen group stays as it is until the group is unfrozen.) Returns those unfrozen."""
        p = self.project
        thawed = [t for t in dict.fromkeys(track_ids) if p.has_owner(t) and p.frozen_by(t) == t]
        if not thawed:
            return []
        text = "Unfreeze Track" if len(thawed) == 1 else "Unfreeze Tracks"
        self.undo_stack.beginMacro(text)
        try:
            for track_id in thawed:
                self._push(SetFreezeCommand(p, track_id, p.track(track_id).frozen, None, text))
        finally:
            self.undo_stack.endMacro()
        return thawed

    def flatten_tracks(self, track_ids) -> list[str]:
        """Flattens frozen audio and MIDI tracks: each becomes an audio track
        playing its frozen audio as a clip, without its devices or their
        automation (its mixer, sends and routing stay). One undo step; returns
        those flattened."""
        p = self.project
        flat = [t for t in dict.fromkeys(track_ids) if p.has_track(t) and p.flatten_problem(t) is None]
        if not flat:
            return []
        text = "Flatten Track" if len(flat) == 1 else "Flatten Tracks"
        self.undo_stack.beginMacro(text)
        try:
            for track_id in flat:
                track = p.track(track_id)
                after = copy.deepcopy(track)
                after.kind = "audio"
                after.clips = [replace(track.frozen.clip(track_id, track.name), id=new_id())]
                after.devices = []
                after.frozen = None
                after.automation = {k: points for k, points in track.automation.items()
                                    if automation.key_device(k) is None}
                view = track.automation_view
                after.automation_view = replace(
                    view, key=None if view.key is not None and automation.key_device(view.key) else view.key,
                    lanes=tuple(k for k in view.lanes if automation.key_device(k) is None))
                self._push(ReplaceTrackCommand(p, copy.deepcopy(track), after, text))
        finally:
            self.undo_stack.endMacro()
        return flat

    # --- What frozen audio holds ------------------------------------------------------------

    def _frozen_problem(self, command) -> str | None:
        """Why a command can't be made: it changes the clips, devices or device
        automation of a frozen track (or of a track in a frozen group); None: it can.
        (Taking away a sidechain whose source goes is fine.)"""
        p = self.project
        tracks: list[str] = []
        what = "devices"
        if isinstance(command, SetClipsCommand):
            tracks, what = [t for t, clips in command.after.items() if clips != command.before.get(t)], "clips"
        elif isinstance(command, SetChainsCommand):
            tracks = list(command.after)
        elif isinstance(command, (SetDeviceParamCommand, SetDeviceParamsCommand, UpdateChainCommand)):
            tracks = [command.key[0]]
        elif isinstance(command, SetDeviceSidechainCommand):
            tracks = [command.track_id] if command.new is not None else []
        elif isinstance(command, (SetDevicesCommand, SetDeviceEnabledCommand, SetDeviceStateCommand,
                                  SetMacrosCommand, SetDeviceNameCommand)):
            tracks = [command.track_id]
        elif isinstance(command, SetEnvelopeCommand):
            tracks, what = ([command.key[0]] if self.lane_frozen(*command.key) else []), "automation"
        elif isinstance(command, SetEnvelopesCommand):
            tracks, what = [owner for owner, key in command.new if self.lane_frozen(owner, key)], "automation"
        holder = next((p.frozen_by(t) for t in tracks if p.has_owner(t) and p.is_frozen(t)), None)
        if holder is None:
            return None
        return f"{p.track(holder).name} is frozen: unfreeze it to change its {what}"

    def lane_frozen(self, owner: str, key: str) -> bool:
        """Whether an automation lane is baked into frozen audio: a device's of a
        frozen track (or one in a frozen group), and the mixer's of a track in a
        frozen group. (A frozen track's own mixer and every send stay live.)"""
        p = self.project
        holder = p.frozen_by(owner) if p.has_owner(owner) else None
        if holder is None or automation.key_send(key) is not None:
            return False
        return holder != owner or not automation.is_mixer_key(key)

    def _outside_frozen(self, index: int, parent: str | None) -> tuple[int, str | None]:
        """Where a track meant for `index` in `parent` goes: there, unless that is in
        a frozen group (which would then hear it): then after that group, in its group."""
        p = self.project
        holder = p.frozen_by(parent) if parent is not None else None
        if holder is None:
            return index, parent
        return p.subtree_end(p.track_index(holder)), p.track(holder).parent

    def _held_problem(self, track_ids) -> str | None:
        """Why these tracks can't leave their groups: one is in a frozen group."""
        p = self.project
        for track_id in track_ids:
            holder = p.frozen_by(track_id)
            if holder is not None and holder != track_id:
                return f"{p.track(holder).name} is frozen: unfreeze it to change what is in it"
        return None

    def _arrangement_problem(self, tree, going=frozenset()) -> str | None:
        """Why the tracks can't be arranged so (None: they can): a track would go
        into or out of a frozen group (or a group in one), whose audio holds what
        is in it. (Groups `going` go next: those frozen hold nothing then.)"""
        p = self.project
        before, after = dict(p.tree()), dict(tree)

        def holder(track_id: str, parents: dict) -> str | None:
            parent = parents.get(track_id)
            while parent is not None:
                if parent not in going and p.track(parent).frozen is not None:
                    return parent
                parent = parents.get(parent)
            return None

        for track_id, parent in tree:
            if parent == before.get(track_id):
                continue
            frozen = holder(track_id, before) or holder(track_id, after)
            if frozen is not None:
                return f"{p.track(frozen).name} is frozen: unfreeze it to change what is in it"
        return None
