import json

import pytest

from gilstudio.model.project import Clip, Device, Project, Track
from gilstudio.model.serialization import ProjectFileError, load_project, project_to_dict, save_project
from gilstudio.model.timebase import TimeSignature


def make_project(audio_path: str) -> Project:
    p = Project()
    p.tempo = 97.5
    p.time_signature = TimeSignature(6, 8)
    p.loop_enabled, p.loop_start, p.loop_end = True, 3.0, 9.0
    p.master_volume_db = -3.0
    p.tracks = [
        Track(id="t1", name="Drums", color="#ff94a6", volume_db=-6.0, pan=0.25, mute=True, height=90,
              devices=[Device(id="d1", kind="utility", params={"gain": -2.0, "pan": 0.0, "width": 50.0})],
              clips=[Clip(id="c1", path=audio_path, name="kick", start_beat=1.5, duration_sec=2.0, offset_sec=0.25,
                          source_duration_sec=4.0, gain_db=-1.0)]),
        Track(id="t2", name="Empty", color="#8bc5ff", solo=True),
    ]
    return p


def test_roundtrip(tmp_path):
    audio = tmp_path / "audio" / "kick.wav"
    audio.parent.mkdir()
    audio.write_bytes(b"")
    original = make_project(str(audio))
    target = tmp_path / "song.gilproj"
    save_project(original, target)
    assert original.path == target

    loaded = Project()
    load_project(loaded, target)
    assert project_to_dict(loaded, target) == project_to_dict(original, target)
    assert loaded.time_signature == TimeSignature(6, 8)
    assert loaded.tracks[0].clips[0].path == str(audio)


def test_relative_path_fallback_when_folder_moves(tmp_path):
    folder = tmp_path / "old"
    (folder / "audio").mkdir(parents=True)
    audio = folder / "audio" / "kick.wav"
    audio.write_bytes(b"")
    save_project(make_project(str(audio)), folder / "song.gilproj")

    moved = tmp_path / "new"
    folder.rename(moved)
    loaded = Project()
    load_project(loaded, moved / "song.gilproj")
    assert loaded.tracks[0].clips[0].path == str(moved / "audio" / "kick.wav")


def test_rejects_foreign_files(tmp_path):
    bad = tmp_path / "x.gilproj"
    bad.write_text(json.dumps({"format": "something-else"}))
    with pytest.raises(ProjectFileError):
        load_project(Project(), bad)
    bad.write_text("{not json")
    with pytest.raises(ProjectFileError):
        load_project(Project(), bad)
