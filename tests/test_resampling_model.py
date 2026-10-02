"""Resampling in the model: an audio track taking its input from another
track's output (a track, a group, a return) or the master's; cycles refused,
and inputs that would close one dropped when tracks move into groups; a source
going takes the inputs from it along (one undo step); saving; and the engine
taking it all (through the bridge)."""

import pytest
from PySide6.QtGui import QUndoStack

from gilstudio import _engine as ge
from gilstudio.model.automation import MASTER
from gilstudio.model.editor import ProjectEditor
from gilstudio.model.project import Project
from gilstudio.model.serialization import load_into, project_to_dict


@pytest.fixture
def editor(app):
    return ProjectEditor(Project(), QUndoStack())


def test_an_input_from_a_track_and_its_undo(editor):
    p = editor.project
    source = editor.add_midi_track(name="Synth")
    track = editor.add_audio_track(name="Bounce")
    editor.set_track_input(track.id, (0, 1))
    steps = editor.undo_stack.count()
    editor.set_track_input_track(track.id, source.id)
    assert (track.input, track.input_track) == ((), source.id) and track.has_input
    assert editor.undo_stack.count() == steps + 1 and editor.undo_stack.undoText() == "Change Track Input"
    editor.set_track_input(track.id, (2,))  # back to the device's channels
    assert (track.input, track.input_track) == ((2,), None)
    editor.undo_stack.undo()
    assert (track.input, track.input_track) == ((), source.id)
    editor.undo_stack.undo()
    assert (track.input, track.input_track) == ((0, 1), None)
    editor.set_track_input_track(track.id, MASTER)  # resampling
    assert track.input_track == MASTER and p.input_name(MASTER) == "Resampling"
    editor.set_track_input_track(track.id, None)
    assert (track.input, track.input_track, track.has_input) == ((), None, False)
    assert p.input_name(source.id) == "Synth"
    assert [t.id for t in p.input_sources(track.id)] == [source.id]


def test_what_can_be_a_source(editor):
    p = editor.project
    a = editor.add_audio_track()
    b = editor.add_audio_track()
    midi = editor.add_midi_track()
    group = editor.group_tracks([b.id])
    ret = editor.add_return_track()
    editor.set_send(a.id, ret.id, 0.0)
    assert [t.id for t in p.input_sources(b.id)] == [a.id, group.id, midi.id, ret.id]
    with pytest.raises(ValueError):
        editor.set_track_input_track(b.id, b.id)  # itself
    with pytest.raises(ValueError):
        editor.set_track_input_track(b.id, group.id)  # its own group
    assert p.input_would_cycle(b.id, group.id) and p.input_would_cycle(a.id, ret.id)
    with pytest.raises(ValueError):
        editor.set_track_input_track(a.id, ret.id)  # a return it sends to
    with pytest.raises(ValueError):
        editor.set_track_input_track(midi.id, a.id)  # MIDI tracks take MIDI
    with pytest.raises(ValueError):
        editor.set_track_input_track(group.id, MASTER)  # groups record nothing
    with pytest.raises(ValueError):
        editor.set_track_input_track(a.id, "nothing")
    editor.set_track_input_track(b.id, ret.id)
    assert p.would_cycle(b.id, ret.id)  # a send closes cycles through inputs too: b -> ret -> b
    with pytest.raises(ValueError):
        editor.set_send(b.id, ret.id, 0.0)
    assert p.input_would_cycle(a.id, b.id)  # a -> ret -> b
    assert not p.input_would_cycle(b.id, a.id) and not p.input_would_cycle(b.id, MASTER)
    assert p.send_targets(b.id) == []


def test_moving_a_track_into_its_source_drops_the_input(editor):
    """In the group it takes its input from (or a group fed by it), a track's
    input would close a cycle: it goes, in the same undo step."""
    p = editor.project
    a = editor.add_audio_track()
    b = editor.add_audio_track()
    group = editor.group_tracks([a.id])
    editor.set_track_input_track(b.id, group.id)
    steps = editor.undo_stack.count()
    assert editor.move_tracks([b.id], p.track_index(a.id) + 1, group.id)
    assert (b.parent, b.input_track) == (group.id, None)
    assert editor.undo_stack.count() == steps + 1
    editor.undo_stack.undo()
    assert (b.parent, b.input_track) == (None, group.id)
    # Out of the group, or into another one, it keeps its input.
    other = editor.group_tracks([b.id])
    assert (b.parent, b.input_track) == (other.id, group.id)


def test_a_source_going_takes_the_inputs_from_it(editor):
    p = editor.project
    source = editor.add_audio_track()
    group = editor.group_tracks([source.id])
    ret = editor.add_return_track()
    a = editor.add_audio_track()
    b = editor.add_audio_track()
    c = editor.add_audio_track()
    editor.set_track_input_track(a.id, source.id)
    editor.set_track_input_track(b.id, group.id)
    editor.set_track_input_track(c.id, ret.id)
    steps = editor.undo_stack.count()
    editor.delete_tracks([source.id])
    assert (a.input_track, b.input_track) == (None, group.id) and editor.undo_stack.count() == steps + 1
    editor.undo_stack.undo()
    assert p.has_track(source.id) and a.input_track == source.id
    editor.ungroup([group.id])
    assert b.input_track is None and p.track(source.id).parent is None
    editor.undo_stack.undo()
    assert b.input_track == group.id
    editor.delete_tracks([group.id])  # with what is in it
    assert (a.input_track, b.input_track) == (None, None)
    editor.delete_tracks([ret.id])
    assert c.input_track is None
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert (a.input_track, b.input_track, c.input_track) == (source.id, group.id, ret.id)


def test_inputs_are_saved_and_loaded(editor):
    p = editor.project
    a = editor.add_audio_track()
    b = editor.add_audio_track()
    c = editor.add_audio_track()
    ret = editor.add_return_track()
    editor.set_track_input_track(a.id, c.id)  # a track listed after it
    editor.set_track_input_track(b.id, MASTER)
    editor.set_track_input_track(c.id, ret.id)
    data = project_to_dict(p)
    assert [t.get("input_track") for t in data["tracks"]] == [c.id, MASTER, ret.id]
    loaded = Project()
    load_into(loaded, data)
    assert [t.input_track for t in loaded.tracks] == [c.id, MASTER, ret.id]

    # Edited files: an input from a track that isn't there, or one closing a cycle, is dropped.
    data["tracks"][0]["input_track"] = "gone"
    data["tracks"][2]["input_track"] = b.id
    data["tracks"][1]["input_track"] = c.id  # b <- c <- b: the later one goes
    data["tracks"][1]["input"] = [0, 1]
    load_into(loaded, data)
    assert [t.input_track for t in loaded.tracks] == [None, c.id, None]
    assert loaded.tracks[1].input == ()
    del data["tracks"][1]["input_track"]  # (older files have none)
    load_into(loaded, data)
    assert loaded.tracks[1].input == (0, 1)


def test_the_engine_takes_inputs(app):
    from gilstudio.audio.engine_bridge import EngineBridge

    engine = ge.Engine()
    project = Project()
    stack = QUndoStack()
    editor = ProjectEditor(project, stack)
    bridge = EngineBridge(engine, project)
    try:
        def engine_input(track) -> int | None:
            return engine.track_input_track(bridge._track_ids[track.id])

        source = editor.add_audio_track()
        group = editor.group_tracks([source.id])
        track = editor.add_audio_track()
        editor.set_track_input_track(track.id, group.id)
        assert engine_input(track) == bridge._track_ids[group.id]
        editor.set_track_input_track(track.id, MASTER)
        assert engine_input(track) == ge.MASTER
        editor.set_track_input(track.id, (0,))
        assert engine_input(track) is None
        editor.set_track_input_track(track.id, source.id)
        assert engine_input(track) == bridge._track_ids[source.id]
        # Into the source's group: the input goes before the route that would close the cycle.
        editor.set_track_input_track(track.id, group.id)
        editor.move_tracks([track.id], project.track_index(source.id) + 1, group.id)
        assert engine_input(track) is None
        assert engine.track_output(bridge._track_ids[track.id]) == bridge._track_ids[group.id]
        stack.undo()
        assert engine_input(track) == bridge._track_ids[group.id]
        # Deleting the source, and undoing it.
        editor.delete_tracks([group.id])
        assert engine_input(track) is None
        stack.undo()
        assert engine_input(track) == bridge._track_ids[group.id]
        # A project loaded with a track taking the output of one listed after it.
        later = editor.add_audio_track()
        editor.set_track_input_track(track.id, later.id)
        load_into(project, project_to_dict(project))
        assert engine_input(project.track(track.id)) == bridge._track_ids[later.id]
    finally:
        bridge.shutdown()
        engine.close_device()
