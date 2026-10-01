import pytest
from PySide6.QtGui import QUndoStack

from gilstudio.model import serialization
from gilstudio.model.editor import ProjectEditor
from gilstudio.model.keys import Key, clip_settings, key_from_name, parse_filename, transpose_to
from gilstudio.model.project import Project

C, A_MINOR = Key(0), Key(9, minor=True)


@pytest.mark.parametrize("name, bpm, key", [
    ("Pack_Bass_Loop_128_Am.wav", 128, A_MINOR),
    ("Keys 92bpm F# minor.wav", 92, Key(6, True)),
    ("Vox_Ebmaj_140BPM.wav", 140, Key(3)),
    ("drum_loop_bpm95.wav", 95, None),
    ("Synth_Lead_Cmaj7_120.wav", 120, C),
    ("808_C#m_150.wav", 150, Key(1, True)),
    ("bass-g-min-100.wav", 100, Key(7, True)),
    ("Melody - Key of A - 110.wav", 110, Key(9)),
    ("Pad Bbm.wav", None, Key(10, True)),
    ("Loop 2 120 C.wav", 120, C),
    ("A Day in the Life.wav", None, None),  # "A" is a word here, not a key
    ("Kick 01.wav", None, None),
    ("FX_Riser_08.wav", None, None),
    ("Song (Original Mix).mp3", None, None),
])
def test_parse_filename(name, bpm, key):
    info = parse_filename(name)
    assert info.bpm == bpm and info.key == key


def test_transpose_takes_the_shortest_way_and_treats_relative_keys_alike():
    assert transpose_to(A_MINOR, C) == 0  # same notes
    assert transpose_to(Key(2), C) == -2
    assert transpose_to(Key(10), C) == 2
    assert transpose_to(Key(6), C) == -6  # tritone: down
    assert transpose_to(Key(1, True), A_MINOR) == -4
    assert transpose_to(None, C) == transpose_to(C, None) == 0


def test_key_names_round_trip():
    for tonic in range(12):
        for minor in (False, True):
            assert key_from_name(Key(tonic, minor).name) == Key(tonic, minor)
    assert key_from_name(None) is None and key_from_name("H") is None


def test_clip_settings():
    assert clip_settings("Loop_128_D.wav", 2.0, 120.0, C) == {"warp": True, "segment_bpm": 128.0, "transpose": -2}
    assert clip_settings("Song.wav", 180.0, 124.0, C) == {"warp": True, "segment_bpm": 124.0}  # long: warped
    assert clip_settings("Kick.wav", 0.5, 120.0, C) == {}  # a one-shot plays as it is
    assert clip_settings("Stab_F.wav", 0.5, 120.0, None) == {}


def test_added_clips_follow_name_and_project_key():
    project = Project()
    editor = ProjectEditor(project, QUndoStack())
    editor.set_key(Key(7))  # G major
    [(track_id, clip_id)] = editor.add_clips(None, 0.0, [("C:/x/Bass_Loop_100_Am.wav", 4.8)])
    clip = project.clip(track_id, clip_id)
    assert clip.is_warped and clip.segment_bpm == 100.0 and clip.transpose == -5  # C -> G
    assert clip.length_beats(project.tempo) == pytest.approx(8.0)  # 4.8 s at 100 BPM


def test_key_is_undoable_and_saved():
    project = Project()
    editor = ProjectEditor(project, QUndoStack())
    editor.set_key(A_MINOR)
    assert project.key == A_MINOR
    data = serialization.project_to_dict(project)
    editor.undo_stack.undo()
    assert project.key is None
    serialization.load_into(project, data)
    assert project.key == A_MINOR
