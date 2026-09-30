"""MIDI in the model: clips as windows onto notes, note editing, editor rules and files."""

import json
import random
from dataclasses import replace

import pytest
from PySide6.QtGui import QUndoStack

from gilstudio.audio.engine_bridge import note_descs
from gilstudio.model import edits, notes
from gilstudio.model.editor import ProjectEditor
from gilstudio.model.project import MidiClip, Note, Project, Track
from gilstudio.model.serialization import (
    ProjectFileError,
    load_project,
    project_to_dict,
    save_project,
)

TEMPO = 120.0


def midi_clip(start=0.0, beats=4.0, offset=0.0, clip_notes=(), cid="m"):
    return MidiClip(id=cid, name="m", start_beat=start, duration_beats=beats, offset_beats=offset,
                    notes=notes.normalize(clip_notes))


def played(clip):
    return [(round(start, 6), round(end, 6), note.pitch) for start, end, note in clip.played_notes()]


C, E, G = 60, 64, 67


@pytest.fixture
def qapp():
    from PySide6.QtWidgets import QApplication

    return QApplication.instance() or QApplication([])


@pytest.fixture
def editor(qapp):
    return ProjectEditor(Project(), QUndoStack())


# --- Clips -----------------------------------------------------------------------------


def test_midi_clip_plays_notes_starting_in_its_window():
    clip = midi_clip(start=8.0, beats=2.0, offset=1.0, clip_notes=[
        Note(C, 0.0, 4.0),   # starts before the window: not played (no chasing)
        Note(E, 1.0, 0.5),   # at the window start: plays at the clip start
        Note(G, 2.5, 1.0),   # runs past the window: cut at the clip end
        Note(C, 3.0, 1.0),   # at the window end: not played
    ])
    assert played(clip) == [(8.0, 8.5, E), (9.5, 10.0, G)]
    assert clip.length_beats(TEMPO) == clip.length_beats(60.0) == 2.0  # beats, whatever the tempo
    track = Track(id="t", name="t", color="#fff", kind="midi", clips=[clip])
    [a, b] = note_descs(track)
    assert (a.start_beat, a.length_beats, a.key, a.velocity) == (8.0, 0.5, E, 100)
    assert (b.start_beat, b.length_beats) == (9.5, 0.5)


def test_splitting_and_trimming_keep_every_note():
    clip = midi_clip(start=4.0, beats=4.0, clip_notes=[Note(C, 0.0, 1.0), Note(E, 2.0, 1.0)])
    left, right = edits.split_clip(clip, 6.0, TEMPO)
    assert (left.id, left.duration_beats, played(left)) == ("m", 2.0, [(4.0, 5.0, C)])
    assert (right.start_beat, right.offset_beats, played(right)) == (6.0, 2.0, [(6.0, 7.0, E)])
    assert right.notes == left.notes == clip.notes  # hidden, not lost
    # Trimming the right piece's start back out reveals the first note again.
    restored = edits.trim_start(right, 4.0, TEMPO)
    assert played(restored) == played(clip)
    assert edits.trim_end(left, 20.0, TEMPO).duration_beats == 16.0  # no source to run out of
    assert edits.split_clip(clip, 8.0, TEMPO) is None


def test_trimming_before_the_first_content_beat_moves_the_notes_along():
    clip = midi_clip(start=4.0, beats=2.0, clip_notes=[Note(C, 0.5, 1.0)])
    grown = edits.trim_start(clip, 1.0, TEMPO)
    assert (grown.start_beat, grown.duration_beats, grown.offset_beats) == (1.0, 5.0, 0.0)
    assert grown.notes == (Note(C, 3.5, 1.0),)
    assert played(grown) == played(clip) == [(4.5, 5.5, C)]
    assert edits.trim_start(clip, -3.0, TEMPO).start_beat == 0.0


def test_time_selection_edits_on_midi_clips():
    clip = midi_clip(start=0.0, beats=8.0, clip_notes=[Note(C, 1.0, 1.0), Note(E, 5.0, 1.0)])
    a, b = edits.remove_range([clip], 2.0, 4.0, TEMPO)
    assert [(c.start_beat, c.end_beat(TEMPO)) for c in (a, b)] == [(0.0, 2.0), (4.0, 8.0)]
    assert played(a) + played(b) == [(1.0, 2.0, C), (5.0, 6.0, E)]
    [copy] = edits.slice_range([clip], 4.0, 6.0, TEMPO)
    assert copy.id != clip.id and played(copy) == [(5.0, 6.0, E)]
    # New clips win overlaps, as for audio.
    newer = midi_clip(start=3.0, beats=2.0, cid="new")
    result = edits.resolve_overlaps([clip, newer], {"new"}, TEMPO)
    assert [(c.id if c.id in ("m", "new") else "piece", c.start_beat, c.end_beat(TEMPO)) for c in result] == [
        ("m", 0.0, 3.0), ("new", 3.0, 5.0), ("piece", 5.0, 8.0)]


# --- Notes ------------------------------------------------------------------------------


def test_note_names_follow_ableton():
    assert [notes.note_name(p) for p in (60, 61, 0, 127)] == ["C3", "C#3", "C-2", "G8"]
    assert notes.is_black_key(61) and not notes.is_black_key(64)


def test_placed_notes_shorten_or_replace_notes_on_the_same_key():
    long = Note(C, 0.0, 4.0)
    placed = notes.place([long, Note(E, 0.0, 4.0)], [], [Note(C, 2.0, 1.0)])
    assert placed == (Note(C, 0.0, 2.0), Note(E, 0.0, 4.0), Note(C, 2.0, 1.0))
    assert notes.place([Note(C, 1.0, 1.0)], [], [Note(C, 0.0, 4.0)]) == (Note(C, 0.0, 4.0),)
    assert notes.place([Note(C, 1.0, 4.0)], [], [Note(C, 0.0, 2.0)]) == (Note(C, 0.0, 2.0), Note(C, 2.0, 3.0))
    # Moving a note means removing the old one and placing the new one.
    assert notes.place([long], [long], [Note(G, 1.0, 4.0)]) == (Note(G, 1.0, 4.0),)


def test_moves_and_resizes_stay_in_range():
    group = [Note(C, 1.0, 1.0), Note(120, 2.0, 1.0)]
    assert notes.clamp_move(group, -5.0, 12) == (-1.0, 7)
    assert notes.shifted(group, -1.0, 7) == [Note(67, 0.0, 1.0), Note(127, 1.0, 1.0)]
    assert notes.resized(group, "end", -2.0, 0.25) == [Note(C, 1.0, 0.25), Note(120, 2.0, 0.25)]
    assert notes.resized([Note(C, 1.0, 1.0)], "start", -3.0) == [Note(C, 0.0, 2.0)]
    assert notes.resized([Note(C, 1.0, 1.0)], "start", 3.0, 0.25) == [Note(C, 1.75, 0.25)]
    assert notes.with_velocity([Note(C, 0.0, 1.0, 100), Note(E, 0.0, 1.0, 20)], -30) == [
        Note(C, 0.0, 1.0, 70), Note(E, 0.0, 1.0, 1)]


def test_legato_joins_notes_and_chords():
    chord = [Note(C, 0.0, 0.5), Note(E, 0.0, 0.25)]
    melody = [Note(G, 1.5, 0.25), Note(C, 3.0, 2.0)]
    result = notes.legato(chord + melody, chord + melody, 4.0)
    # A chord reaches the next start together; the last note reaches the clip's end
    # (and is shortened to it); legato shortens as well as lengthens.
    assert [(n.pitch, n.start, n.length) for n in result] == [(C, 0.0, 1.5), (E, 0.0, 1.5), (G, 1.5, 1.5),
                                                              (C, 3.0, 1.0)]
    # Only the targets count as "next": an unselected note in between is passed over,
    # unless it is on the same key, which a note never runs into.
    between = Note(E, 1.0, 0.5)
    assert notes.legato([Note(C, 0.0, 0.25), Note(G, 2.0, 0.25)], [between], 4.0)[0].length == 2.0
    assert notes.legato([Note(E, 0.0, 0.25)], [between], 4.0)[0].length == 1.0
    past_the_end = Note(C, 5.0, 1.0)
    assert notes.legato([past_the_end], [], 4.0) == [past_the_end]


def test_quantize_moves_starts_onto_the_grid():
    played = [Note(C, 0.1, 0.5), Note(E, 0.9, 0.2), Note(G, 1.3, 1.0, 90)]

    def starts(result):
        return [(round(n.start, 6), n.length) for n in sorted(result, key=notes.by_time)]

    assert starts(notes.quantized(played, 0.25)) == [(0.0, 0.5), (1.0, 0.2), (1.25, 1.0)]
    assert starts(notes.quantized(played, 0.25, amount=0.5)) == [(0.05, 0.5), (0.95, 0.2), (1.275, 1.0)]
    assert notes.quantized([Note(C, 0.3, 0.1)], 1 / 3)[0].start == pytest.approx(1 / 3)  # triplets
    # Two notes on a key landing together: the longer one stays.
    assert notes.quantized([Note(C, 0.9, 0.5), Note(C, 1.05, 1.0)], 1.0) == [Note(C, 1.0, 1.0)]


def test_humanize_nudges_timing_and_velocity_within_bounds():
    played = [Note(C + i, float(i), 0.5, 100) for i in range(40)]
    result = notes.humanized(played, random.Random(7), 0.5)
    assert result == notes.humanized(played, random.Random(7), 0.5)  # repeatable with the same seed
    by_pitch = {n.pitch: n for n in result}
    shifts = [by_pitch[n.pitch].start - n.start for n in played]
    changes = [by_pitch[n.pitch].velocity - n.velocity for n in played]
    assert max(map(abs, shifts)) <= 0.5 * notes.HUMANIZE_BEATS and len(set(shifts)) > 10
    assert max(map(abs, changes)) <= 0.5 * notes.HUMANIZE_VELOCITY + 0.5 and len(set(changes)) > 3
    assert all(by_pitch[n.pitch].length == n.length for n in played)
    assert notes.humanized(played, random.Random(7), 0.0) == played


# --- Editor -----------------------------------------------------------------------------


def test_midi_tracks_come_with_the_synth(editor):
    track = editor.add_midi_track()
    assert (track.kind, track.name, [d.kind for d in track.devices]) == ("midi", "1 MIDI", ["synth"])
    assert editor.add_device(track.id, "utility").kind == "utility"
    assert editor.add_device(track.id, "synth") is not None  # replaces the instrument, stays first
    assert [d.kind for d in editor.project.track(track.id).devices] == ["synth", "utility"]
    audio = editor.add_audio_track()
    assert editor.add_device(audio.id, "synth") is None  # instruments need a MIDI track
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert [d.kind for d in editor.project.track(track.id).devices] == ["synth", "utility"]


def test_move_device_goes_to_its_new_position(editor):
    track = editor.add_midi_track()
    synth, a, b, c = [track.devices[0].id] + [editor.add_device(track.id, "utility").id for _ in range(3)]

    def chain():
        return [d.id for d in editor.project.track(track.id).devices]

    editor.move_device(track.id, a, 2)  # right
    assert chain() == [synth, b, a, c]
    editor.move_device(track.id, c, 1)  # left
    assert chain() == [synth, c, b, a]
    editor.move_device(track.id, b, 9)  # past the end
    assert chain() == [synth, c, a, b]
    editor.move_device(track.id, c, 0)  # not before the instrument
    editor.move_device(track.id, synth, 3)  # which doesn't move
    assert chain() == [synth, c, a, b]
    assert editor.undo_stack.undoText() == "Move Device"


def test_midi_clips_and_note_edits_are_undoable(editor):
    track = editor.add_midi_track()
    ref = editor.add_midi_clip(track.id, 4.0, 4.0)
    assert editor.project.clip(*ref).name == track.name
    key = object()
    for length in (1.0, 2.0, 3.0):  # one gesture: one undo step
        editor.set_clip_notes(ref, [Note(E, 0.0, length), Note(C, 0.0, length)], "Resize Notes", key)
    assert editor.project.clip(*ref).notes == (Note(C, 0.0, 3.0), Note(E, 0.0, 3.0))
    editor.undo_stack.undo()
    assert editor.project.clip(*ref).notes == ()
    assert editor.add_midi_clip(editor.add_audio_track().id, 0.0, 4.0) is None


def test_clips_only_move_onto_tracks_of_their_kind(editor):
    audio1 = editor.add_audio_track()
    midi = editor.add_midi_track()
    audio2 = editor.add_audio_track()
    [ref] = editor.add_clips(audio1.id, 0.0, [("a.wav", 1.0)])
    assert editor.clamp_track_delta([ref], 1) == 0  # the MIDI track in between: stay
    assert editor.clamp_track_delta([ref], 2) == 2
    [(dest, _)] = editor.move_clips([ref], 0.0, track_delta=1)
    assert dest == audio1.id
    midi_ref = editor.add_midi_clip(midi.id, 0.0, 4.0)
    assert editor.move_clips([midi_ref], 2.0, track_delta=1)[0][0] == midi.id
    # Audio files meant for a MIDI track land on a new audio track instead.
    [(new_track, _)] = editor.add_clips(midi.id, 0.0, [("b.wav", 1.0)])
    assert new_track not in (audio1.id, midi.id, audio2.id)
    assert not editor.project.track(new_track).is_midi


# --- Files -------------------------------------------------------------------------------


def test_midi_tracks_roundtrip(tmp_path, qapp):
    project = Project()
    clip = midi_clip(start=2.0, beats=4.0, offset=1.0, clip_notes=[Note(C, 1.0, 0.5, 90), Note(G, 2.25, 1.0)])
    project.tracks = [Track(id="t1", name="Keys", color="#ff94a6", kind="midi", clips=[clip])]
    target = tmp_path / "song.gilproj"
    save_project(project, target)
    data = json.loads(target.read_text(encoding="utf-8"))
    assert data["version"] >= 2 and data["tracks"][0]["kind"] == "midi"  # 2 added MIDI tracks
    assert data["tracks"][0]["clips"][0]["notes"] == [[C, 1.0, 0.5, 90], [G, 2.25, 1.0, 100]]
    loaded = Project()
    load_project(loaded, target)
    assert loaded.tracks[0].clips == [clip]
    assert project_to_dict(loaded, target) == project_to_dict(project, target)

    data["tracks"][0]["kind"] = "video"
    target.write_text(json.dumps(data), encoding="utf-8")
    with pytest.raises(ProjectFileError):
        load_project(Project(), target)


def test_version_1_projects_load_as_audio_tracks(tmp_path, qapp):
    target = tmp_path / "old.gilproj"
    target.write_text(json.dumps({"format": "gilstudio-project", "version": 1, "tracks": [
        {"id": "t", "name": "Old", "color": "#fff", "clips": []}]}), encoding="utf-8")
    project = Project()
    load_project(project, target)
    assert project.tracks[0].kind == "audio"
    assert replace(project.tracks[0]).is_midi is False
