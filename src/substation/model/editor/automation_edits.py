"""Editing automation: envelopes (points, curves, ranges, copy and paste) and
the automation lanes a track shows."""

from __future__ import annotations

from dataclasses import dataclass, replace

from .. import automation
from ..automation import MIXER_PAN, MIXER_VOLUME, AutomationView, Envelope
from ..commands import SetEnvelopeCommand, SetEnvelopesCommand

LaneRef = tuple[str, str]  # (automation owner, target key)


@dataclass(frozen=True)
class CopiedAutomation:
    """Automation copied from a lane range (Ctrl+C / Ctrl+X), `length` beats long:
    each lane's envelope over it (points from beat 0), in the lanes' order."""

    length: float
    lanes: tuple[tuple[LaneRef, Envelope], ...]


class AutomationEdits:
    """Envelopes and the lanes shown. Part of ProjectEditor (editor/__init__.py)."""

    # Envelopes are normalized (see automation.py); an owner is a track id or MASTER.

    def set_envelope(self, owner: str, key: str, points, text: str = "Change Automation",
                     merge_key: object | None = None) -> None:
        new = automation.normalize(points)
        old = self.project.envelope(owner, key)
        if new != old:
            self._push(SetEnvelopeCommand(self.project, owner, key, old, new, text, merge_key))

    def add_automation_point(self, owner: str, key: str, beat: float, value: float,
                             merge_key: object | None = None) -> int:
        """A new breakpoint; returns its index. With a `merge_key`, dragging it
        right away (move_automation_points with the same key) is the same undo step."""
        points, index = automation.add_point(self.project.envelope(owner, key), beat, value)
        self.set_envelope(owner, key, points, "Add Automation Point", merge_key)
        return index

    def move_automation_points(self, owner: str, key: str, original: Envelope, indices, delta_beats: float,
                               delta_value: float, merge_key: object | None = None) -> dict[int, int]:
        """Move points of `original` (the envelope when the drag began) together.
        Returns where they are now ({index in `original`: index})."""
        points, where = automation.move_points_mapped(original, indices, delta_beats, delta_value)
        self.set_envelope(owner, key, points, "Move Automation", merge_key)
        return where

    def delete_automation_points(self, owner: str, key: str, indices) -> None:
        points = automation.delete_points(self.project.envelope(owner, key), indices)
        self.set_envelope(owner, key, points, "Delete Automation Point")

    def set_automation_curve(self, owner: str, key: str, original: Envelope, index: int, curve: float,
                             merge_key: object | None = None) -> None:
        self.set_envelope(owner, key, automation.set_curve(original, index, curve), "Change Automation Curve",
                          merge_key)

    def clear_envelope(self, owner: str, key: str) -> None:
        self.set_envelope(owner, key, (), "Delete Envelope")

    def _each_lane(self, text: str, lanes, change) -> None:
        """`change(envelope)` on each lane, as one undo step."""
        changed = [(owner, key, change(self.project.envelope(owner, key))) for owner, key in dict.fromkeys(lanes)
                   if self.project.has_owner(owner)]
        changed = [(o, k, new) for o, k, new in changed if new != self.project.envelope(o, k)]
        if not changed:
            return
        self.undo_stack.beginMacro(text)
        for owner, key, new in changed:
            self.set_envelope(owner, key, new, text)
        self.undo_stack.endMacro()

    def delete_automation_range(self, start: float, end: float, lanes: list[LaneRef]) -> None:
        """Delete the automation between two beats on these lanes."""
        self._each_lane("Delete Automation", lanes, lambda points: automation.remove_range(points, start, end))

    def move_automation_range(self, start: float, end: float, originals: dict[LaneRef, Envelope],
                              delta_beats: float, delta_value: float, merge_key: object | None = None) -> None:
        """Move the automation between two beats on these lanes (`originals`: their
        envelopes when the drag began) in time and value, as one undo step."""
        lanes = {lane: points for lane, points in originals.items() if points and self.project.has_owner(lane[0])}
        new = {lane: automation.move_range(points, start, end, delta_beats, delta_value)
               for lane, points in lanes.items()}
        current = {lane: self.project.envelope(*lane) for lane in new}
        if new != current:
            self._push(SetEnvelopesCommand(self.project, current, new, "Move Automation", merge_key))

    def duplicate_automation_range(self, start: float, end: float, lanes: list[LaneRef]) -> None:
        """Copy the automation between two beats to right after `end`, over what was there."""
        def duplicate(points: Envelope) -> Envelope:
            if not points:
                return points
            return automation.paste_range(points, automation.copy_range(points, start, end), end, end - start)
        self._each_lane("Duplicate Automation", lanes, duplicate)

    def copy_automation_range(self, start: float, end: float, lanes) -> CopiedAutomation | None:
        """Ctrl+C on a lane range: the automation between two beats on these lanes
        (those that have any). None if none of them has."""
        if end <= start:
            return None
        copied = tuple((lane, automation.copy_range(self.project.envelope(*lane), start, end))
                       for lane in dict.fromkeys(lanes) if self.project.has_owner(lane[0]))
        copied = tuple((lane, points) for lane, points in copied if points)
        return CopiedAutomation(end - start, copied) if copied else None

    def cut_automation_range(self, start: float, end: float, lanes) -> CopiedAutomation | None:
        """Ctrl+X on a lane range: copy it (copy_automation_range), then delete it. One undo step."""
        content = self.copy_automation_range(start, end, lanes)
        if content is not None:
            self._each_lane("Cut Automation", [lane for lane, _ in content.lanes],
                            lambda points: automation.remove_range(points, start, end))
        return content

    def automation_paste_targets(self, content: CopiedAutomation, lanes=()) -> list[LaneRef | None]:
        """Where each copied lane goes: onto `lanes` (the selected ones, in order)
        one to one if there are as many, or one copied lane onto each of them;
        otherwise onto the lanes it was copied from. None for a lane that is gone
        (its owner, or the device or send it automates)."""
        lanes = list(dict.fromkeys(lanes))
        if lanes and len(content.lanes) in (1, len(lanes)):
            targets = lanes
        else:
            targets = [lane for lane, _ in content.lanes]
        return [lane if self._lane_exists(lane) else None for lane in targets]

    def _lane_exists(self, lane: LaneRef) -> bool:
        owner, key = lane
        if not self.project.has_owner(owner) or not automation.is_key(key):
            return False
        device = automation.key_device(key)
        if device is not None:
            return self.project.has_device(owner, device)
        send = automation.key_send(key)
        return send is None or send in self.project.track(owner).sends

    def paste_automation(self, content: CopiedAutomation, at_beat: float, lanes=()) -> list[LaneRef]:
        """Ctrl+V: copied automation at `at_beat`, replacing what is there, onto the
        lanes automation_paste_targets picks. One undo step; the lanes pasted onto."""
        at = max(0.0, at_beat)
        targets = self.automation_paste_targets(content, lanes)
        sources = [points for _lane, points in content.lanes]
        if len(sources) == 1:
            sources *= len(targets)
        pasted = {lane: points for lane, points in zip(targets, sources, strict=True) if lane is not None}
        edges = (at, at + content.length)
        current = {lane: self.project.envelope(*lane) for lane in pasted}
        new = {lane: automation.drop_redundant(automation.paste_range(current[lane], points, at, content.length),
                                               edges) for lane, points in pasted.items()}
        changed = {lane: points for lane, points in new.items() if points != current[lane]}
        if changed:
            self._push(SetEnvelopesCommand(self.project, {lane: current[lane] for lane in changed}, changed,
                                           "Paste Automation"))
        return list(pasted)

    def set_automation_locked(self, locked: bool) -> None:
        """Lock Envelopes: whether automation stays in place when clips move
        (unlocked, it moves with them). A setting, not an edit: not undoable."""
        if locked != self.project.automation_locked:
            self.project.update_settings(automation_locked=locked)

    # View state: what the arrangement shows of each owner's automation. Saved with
    # the project, but not undoable (like track heights).

    def _update_view(self, owner: str, **changes) -> None:
        view = self.project.automation_view(owner)
        new = replace(view, **changes)
        if new != view:
            self.project.set_automation_view(owner, new)

    def default_automation_key(self, owner: str) -> str:
        """What a lane shows when nothing was chosen: the first automated target, else the volume."""
        return next(iter(self.project.automation(owner)), MIXER_VOLUME)

    def show_automation(self, owner: str, key: str | None = None) -> None:
        """Show an owner's automation, with `key` (if given) in its main lane."""
        view = self.project.automation_view(owner)
        self._update_view(owner, shown=True, key=key or view.key or self.default_automation_key(owner))

    def hide_automation(self, owner: str) -> None:
        self._update_view(owner, shown=False)

    def toggle_all_automation(self) -> bool:
        """Show every track's (and the master's) automation, or hide it all if all
        of it shows. Returns whether it shows now."""
        owners = self.project.owners()
        show = not all(self.project.automation_view(o).shown for o in owners)
        for owner in owners:
            if show:
                self.show_automation(owner)
            else:
                self.hide_automation(owner)
        return show

    def add_automation_lane(self, owner: str) -> None:
        """Another lane below the owner's main lane: the first automated target not
        shown yet (else the first mixer control not shown)."""
        view = self.project.automation_view(owner)
        shown = {view.key, *view.lanes}
        candidates = [*self.project.automation(owner), MIXER_VOLUME, MIXER_PAN]
        key = next((k for k in candidates if k not in shown), candidates[0])
        self._update_view(owner, shown=True, key=view.key or self.default_automation_key(owner),
                          lanes=view.lanes + (key,))

    def set_automation_lane(self, owner: str, index: int, key: str) -> None:
        """Show `key` in lane `index` (-1: the main lane)."""
        view = self.project.automation_view(owner)
        if index < 0:
            self._update_view(owner, shown=True, key=key)
        elif index < len(view.lanes):
            self._update_view(owner, lanes=view.lanes[:index] + (key,) + view.lanes[index + 1:])

    def remove_automation_lane(self, owner: str, index: int) -> None:
        view = self.project.automation_view(owner)
        if 0 <= index < len(view.lanes):
            self._update_view(owner, lanes=view.lanes[:index] + view.lanes[index + 1:])

    def reset_automation_view(self, owner: str) -> None:
        self._update_view(owner, **vars(AutomationView()))
