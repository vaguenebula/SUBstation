"""Freezing tracks from the UI (Ctrl+Shift+F, the track menus): each track is
rendered (with a wait cursor) before one undo step freezes them all. What
can't be done is said through the editor's `refused` signal (the status bar)."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from ..audio.engine_bridge import EngineBridge
from ..model.editor import ProjectEditor
from .arrangement.view_state import Selection


def _owners(editor: ProjectEditor, track_ids) -> list[str]:
    """The tracks (groups too) and returns among these, in order, each once."""
    p = editor.project
    return [t for t in dict.fromkeys(track_ids) if p.has_track(t) or p.has_return(t)]


def freeze_tracks(editor: ProjectEditor, bridge: EngineBridge, track_ids) -> list[str]:
    """Freezes these tracks (not those in a group frozen with them), one undo
    step; returns those frozen."""
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
    freezes = {}
    QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
    try:
        for track_id in tracks:
            try:
                freezes[track_id] = bridge.render_freeze(track_id)
            except (ValueError, OSError, RuntimeError) as exc:
                editor.refused.emit(f"{p.track(track_id).name} could not be frozen: {exc}")
                return []
    finally:
        QApplication.restoreOverrideCursor()
    return editor.freeze_tracks(freezes)


def unfreeze_tracks(editor: ProjectEditor, track_ids) -> list[str]:
    """Unfreezes these tracks, or the frozen groups they are in; returns those unfrozen."""
    p = editor.project
    holders = [h for t in _owners(editor, track_ids) if (h := p.frozen_by(t)) is not None]
    return editor.unfreeze_tracks(holders)


def toggle_freeze(editor: ProjectEditor, bridge: EngineBridge, track_ids) -> list[str]:
    """Ctrl+Shift+F: unfreezes the tracks if they are all frozen (or in frozen
    groups), else freezes those that aren't. Returns those that changed."""
    p = editor.project
    tracks = _owners(editor, track_ids)
    if tracks and all(p.is_frozen(t) for t in tracks):
        return unfreeze_tracks(editor, tracks)
    return freeze_tracks(editor, bridge, [t for t in tracks if not p.is_frozen(t)])


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
