"""Group tracks in the model: the tree kept in the flat track list, grouping,
ungrouping, moving tracks into and out of groups (all undoable), folding, and
saving; and the engine hearing a group as a bus (through the bridge)."""

import json

import numpy as np
import pytest
from PySide6.QtGui import QUndoStack

from substation import _engine as ge
from substation.model.editor import ProjectEditor
from substation.model.project import Clip, Project, Track, tree_problem
from substation.model.serialization import load_into, project_to_dict

from .conftest import SAMPLE_RATE


@pytest.fixture
def editor(app):
    return ProjectEditor(Project(), QUndoStack())


def names(project: Project) -> list[tuple[str, str | None]]:
    """Each track's name and its group's name, in order."""
    by_id = {t.id: t.name for t in project.tracks}
    return [(t.name, by_id.get(t.parent)) for t in project.tracks]


def tracks(editor: ProjectEditor, *names_) -> list[str]:
    return [editor.add_audio_track(name=name).id for name in names_]


def test_the_tree_invariant():
    def make(*rows):
        return [Track(id=i, name=i, color="", kind="group" if i.startswith("g") else "audio", parent=parent)
                for i, parent in rows]

    assert tree_problem(make(("g1", None), ("a", "g1"), ("g2", "g1"), ("b", "g2"), ("c", "g1"), ("d", None))) is None
    assert tree_problem(make(("a", "g1"), ("g1", None))) is not None  # its group comes later
    assert tree_problem(make(("g1", None), ("a", "g1"), ("b", None), ("c", "g1"))) is not None  # not together
    assert tree_problem(make(("g1", None), ("a", "g1"), ("b", "a"))) is not None  # not in a group
    assert tree_problem(make(("g1", None), ("g1", None))) is not None


def test_group_and_ungroup_round_trip(editor):
    p = editor.project
    a, _b, c = tracks(editor, "A", "B", "C")
    stack = editor.undo_stack
    group = editor.group_tracks([c, a])
    assert group.is_group and group.parent is None
    assert names(p) == [(group.name, None), ("A", group.name), ("C", group.name), ("B", None)]
    assert stack.undoText() == "Group Tracks"

    # Grouping inside a group nests.
    inner = editor.group_tracks([c])
    assert names(p) == [(group.name, None), ("A", group.name), (inner.name, group.name), ("C", inner.name),
                        ("B", None)]
    assert p.ancestors(c) == [inner.id, group.id] and p.depth(c) == 2
    assert [t.name for t in p.descendants(group.id)] == ["A", inner.name, "C"]

    stack.undo()
    stack.undo()
    assert names(p) == [("A", None), ("B", None), ("C", None)]
    stack.redo()
    stack.redo()
    assert names(p)[3] == ("C", inner.name)

    # Ungrouping: what was in the group takes its place, in its group.
    editor.ungroup([group.id])
    assert names(p) == [("A", None), (inner.name, None), ("C", inner.name), ("B", None)]
    assert stack.undoText() == "Ungroup Tracks"
    stack.undo()
    assert names(p)[0] == (group.name, None) and names(p)[3] == ("C", inner.name)
    assert tree_problem(p.tracks) is None


def test_moving_tracks_into_and_out_of_groups(editor):
    p = editor.project
    a, b, c = tracks(editor, "A", "B", "C")
    group = editor.group_tracks([a])
    # Into the group, after A.
    assert editor.move_tracks([c], p.track_index(a) + 1, group.id)
    assert names(p) == [(group.name, None), ("A", group.name), ("C", group.name), ("B", None)]
    # Out of it, to the end.
    assert editor.move_tracks([a], len(p.tracks), None)
    assert names(p) == [(group.name, None), ("C", group.name), ("B", None), ("A", None)]
    # Not amid another group's tracks without being in it, nor a group into itself.
    other = editor.group_tracks([b])
    assert not editor.move_tracks([a], p.track_index(c), None)
    assert not editor.move_tracks([group.id], p.track_index(c) + 1, group.id)
    editor.move_tracks([other.id], p.track_index(c) + 1, group.id)  # a group with its tracks
    assert names(p) == [(group.name, None), ("C", group.name), (other.name, group.name), ("B", other.name),
                        ("A", None)]
    assert not editor.move_tracks([group.id], p.track_index(b) + 1, other.id)  # into a group in it
    for _ in range(4):
        editor.undo_stack.undo()
    assert names(p) == [(group.name, None), ("A", group.name), ("B", None), ("C", None)]


def test_inserted_tracks_go_into_the_group_they_are_inserted_in(editor):
    p = editor.project
    a, b = tracks(editor, "A", "B")
    group = editor.group_tracks([a, b])
    index, parent = editor.insertion_point(a)
    new = editor.add_midi_track(index, name="M", parent=parent)
    assert new.parent == group.id and p.track_index(new.id) == p.track_index(a) + 1
    index, parent = editor.insertion_point(group.id)
    after_group = editor.add_audio_track(index, name="After", parent=parent)
    assert after_group.parent is None and p.track_index(after_group.id) == len(p.tracks) - 1
    # Between a group's tracks, a track can only be in that group.
    amid = editor.add_audio_track(p.track_index(b), name="Amid", parent=None)
    assert amid.parent == group.id
    # Clips dropped on a group go on a new audio track.
    refs = editor.add_clips(group.id, 0.0, [("x.wav", 1.0)], track_index=len(p.tracks))
    assert not p.track(refs[0][0]).is_group and p.track(group.id).clips == []


def test_deleting_a_group_deletes_its_tracks_and_undo_brings_them_back(editor):
    p = editor.project
    a, b, _c = tracks(editor, "A", "B", "C")
    group = editor.group_tracks([a, b])
    inner = editor.group_tracks([b])
    before = names(p)
    editor.delete_tracks([group.id])
    assert names(p) == [("C", None)]
    editor.undo_stack.undo()
    assert names(p) == before
    assert p.track(inner.id).parent == group.id


def test_folding_is_view_state(editor):
    p = editor.project
    a, b = tracks(editor, "A", "B")
    group = editor.group_tracks([a])
    steps = editor.undo_stack.count()
    editor.set_folded(group.id, True)
    assert p.track(group.id).folded and p.is_hidden(a) and not p.is_hidden(b)
    assert editor.undo_stack.count() == steps
    editor.set_folded(b, True)  # a track folds too (to a thin row), hiding nothing else
    assert p.track(b).folded and not p.is_hidden(b)
    assert editor.undo_stack.count() == steps


def test_groups_arent_armed(editor):
    a, = tracks(editor, "A")
    group = editor.group_tracks([a])
    editor.arm_tracks([a, group.id], True)
    assert editor.project.track(a).armed and not editor.project.track(group.id).armed


def test_groups_are_saved_and_loaded(editor, app):
    p = editor.project
    a, b, _c = tracks(editor, "A", "B", "C")
    group = editor.group_tracks([a, b])
    inner = editor.group_tracks([b])
    editor.set_folded(inner.id, True)
    editor.set_folded(a, True)
    data = json.loads(json.dumps(project_to_dict(p)))
    loaded = Project()
    load_into(loaded, data)
    assert names(loaded) == names(p)
    assert loaded.track(inner.id).folded and loaded.track(group.id).is_group
    assert loaded.track(a).folded and not loaded.track(group.id).folded
    # A file whose groups don't hold together (edited by hand) loads with the strays out of them.
    stray = data["tracks"].pop()  # C, put first and in the group that comes after it
    stray["parent"] = group.id
    data["tracks"].insert(0, stray)
    load_into(loaded, data)
    assert names(loaded)[0] == ("C", None)
    assert tree_problem(loaded.tracks) is None


def test_the_engine_hears_groups_as_buses(app, make_wav):
    from substation.audio.engine_bridge import EngineBridge

    engine = ge.Engine()
    engine.set_clip_fade_ms(0)
    project = Project()
    stack = QUndoStack()
    editor = ProjectEditor(project, stack)
    bridge = EngineBridge(engine, project)
    try:
        wav = make_wav(np.full((SAMPLE_RATE, 2), 0.5))
        engine.load_source(wav)
        a, b = tracks(editor, "A", "B")
        for track_id in (a, b):
            editor._commit("Add", {track_id: [Clip(id=track_id + "c", path=wav, name="x", start_beat=0.0,
                                                   duration_sec=1.0, source_duration_sec=1.0)]})

        def level() -> float:
            return float(engine.render_offline(0.0, 4000)[-1, 0])

        assert level() == pytest.approx(1.0)
        group = editor.group_tracks([a, b])
        editor.set_track_param(group.id, "volume_db", -6.0206)
        assert level() == pytest.approx(0.5, abs=1e-3)
        editor.set_track_param(group.id, "mute", True)
        assert level() == pytest.approx(0.0)
        editor.move_tracks([b], len(project.tracks), None)  # out of the muted group
        assert level() == pytest.approx(0.5)
        stack.undo()
        assert level() == pytest.approx(0.0)
        stack.undo()  # unmute
        inner = editor.group_tracks([a])  # a group in the group
        assert level() == pytest.approx(0.5, abs=1e-3)
        editor.delete_tracks([group.id])  # with what is in it
        assert level() == pytest.approx(0.0)
        stack.undo()
        assert level() == pytest.approx(0.5, abs=1e-3)
        assert project.track(inner.id).parent == group.id
    finally:
        bridge.shutdown()
        engine.close_device()
