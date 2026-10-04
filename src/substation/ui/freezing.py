"""Freezing tracks from the UI (Ctrl+Shift+F, the track menus): each track is
rendered in the background, the progress shown (and Cancel) in a dialog
(rendering.py), before one undo step freezes them all; cancelled, nothing is
frozen. What can't be done is said through the editor's `refused` signal (the
status bar)."""

from __future__ import annotations

from PySide6.QtWidgets import QWidget

from ..audio.engine_bridge import EngineBridge
from ..model.editor import ProjectEditor
from .arrangement.view_state import Selection
from .rendering import RenderProgress


def _owners(editor: ProjectEditor, track_ids) -> list[str]:
    """The tracks (groups too) and returns among these, in order, each once."""
    p = editor.project
    return [t for t in dict.fromkeys(track_ids) if p.has_track(t) or p.has_return(t)]


def freeze_tracks(editor: ProjectEditor, bridge: EngineBridge, track_ids, parent: QWidget | None = None) -> list[str]:
    """Freezes these tracks (not those in a group frozen with them), one undo
    step; returns those frozen. The renders' progress shows in a dialog over
    `parent`, whose Cancel freezes none."""
    p = editor.project
    tracks = _owners(editor, track_ids)
    tracks = [t for t in tracks if not (p.has_track(t) and any(a in tracks for a in p.ancestors(t)))]
    if not tracks:
        return []
    if bridge.is_recording:
        editor.refused.emit("Stop recording to freeze tracks")
        return []
    problems = [problem for t in tracks if (problem := p.freeze_problem(t)) is not None]
    tracks = [t for t in tracks if p.freeze_problem(t) is None]
    if problems:
        editor.refused.emit(problems[0])
    if not tracks:
        return []
    freezes = {}

    def discard() -> list[str]:
        for freeze in freezes.values():  # renders no track will play
            bridge.discard_freeze(freeze)
        return []

    with RenderProgress(parent, "Freeze Tracks" if len(tracks) > 1 else "Freeze Track") as progress:
        if not progress.wait_for_devices(bridge):
            return []
        for index, track_id in enumerate(tracks):
            name = p.track(track_id).name
            try:
                render = bridge.start_freeze(track_id)
                count = f" ({index + 1} of {len(tracks)})" if len(tracks) > 1 else ""
                progress.follow(render.job, f"Freezing {name}{count}…", (index, len(tracks)))
                freeze = bridge.finish_freeze(render)
            except (ValueError, OSError, RuntimeError) as exc:
                editor.refused.emit(f"{name} could not be frozen: {exc}")
                return discard()
            if freeze is None:  # cancelled
                return discard()
            freezes[track_id] = freeze
    frozen = editor.freeze_tracks(freezes)
    for track_id, freeze in freezes.items():
        if track_id not in frozen:  # (the editor refused it)
            bridge.discard_freeze(freeze)
    return frozen


def unfreeze_tracks(editor: ProjectEditor, track_ids) -> list[str]:
    """Unfreezes these tracks, or the frozen groups they are in; returns those unfrozen."""
    p = editor.project
    holders = [h for t in _owners(editor, track_ids) if (h := p.frozen_by(t)) is not None]
    return editor.unfreeze_tracks(holders)


def toggle_freeze(editor: ProjectEditor, bridge: EngineBridge, track_ids, parent: QWidget | None = None) -> list[str]:
    """Ctrl+Shift+F: unfreezes the tracks if they are all frozen (or in frozen
    groups), else freezes those that aren't. Returns those that changed."""
    p = editor.project
    tracks = _owners(editor, track_ids)
    if tracks and all(p.is_frozen(t) for t in tracks):
        return unfreeze_tracks(editor, tracks)
    return freeze_tracks(editor, bridge, [t for t in tracks if not p.is_frozen(t)], parent)


def flatten_tracks(editor: ProjectEditor, selection: Selection, track_ids) -> list[str]:
    """Flattens the frozen audio and MIDI tracks among these (one undo step), and
    selects them again (they are new tracks to the arrangement); returns them."""
    p = editor.project
    tracks = [t for t in _owners(editor, track_ids) if p.has_track(t)]
    flat = editor.flatten_tracks(tracks)
    if not flat:
        problems = [problem for t in tracks if (problem := p.flatten_problem(t)) is not None]
        if problems:
            editor.refused.emit(problems[0])
        return []
    for index, track_id in enumerate(flat):
        selection.select_track(track_id, focus_track=True, mode="toggle" if index else "")
    return flat
