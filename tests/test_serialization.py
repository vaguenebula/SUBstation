import json

import pytest

from substation.model.automation import (
    MASTER,
    MIXER_VOLUME,
    AutomationPoint,
    AutomationView,
    device_key,
)
from substation.model.project import Clip, Device, Project, Track
from substation.model.serialization import (
    VERSION,
    ProjectFileError,
    load_into,
    load_project,
    project_to_dict,
    save_project,
)
from substation.model.timebase import TimeSignature


def make_project(audio_path: str) -> Project:
    p = Project()
    p.tempo = 97.5
    p.time_signature = TimeSignature(6, 8)
    p.loop_enabled, p.loop_start, p.loop_end = True, 3.0, 9.0
    p.master.volume_db = -3.0
    p.master.devices = [Device(id="m1", kind="utility", params={"gain": -1.0, "pan": 0.0, "width": 100.0})]
    p.master.automation = {device_key("m1", "gain"): (AutomationPoint(0.0, 0.5),)}
    p.tracks = [
        Track(id="t1", name="Drums", color="#ff94a6", volume_db=-6.0, pan=0.25, mute=True, height=90,
              devices=[Device(id="d1", kind="utility", params={"gain": -2.0, "pan": 0.0, "width": 50.0})],
              clips=[Clip(id="c1", path=audio_path, name="kick", start_beat=1.5, duration_sec=2.0, offset_sec=0.25,
                          source_duration_sec=4.0, gain_db=-1.0, warp=True, warp_mode="Formants",
                          segment_bpm=128.0, transpose=-3, detune=12.0, pan=-0.5)]),
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


def test_a_reversed_clip_keeps_the_file_it_came_from(tmp_path):
    folder = tmp_path / "old"
    (folder / "Reversed").mkdir(parents=True)
    original, backwards = folder / "kick.wav", folder / "Reversed" / "kick R.wav"
    for path in (original, backwards):
        path.write_bytes(b"")
    project = Project()
    project.tracks = [Track(id="t1", name="Drums", color="#ff94a6",
                            clips=[Clip(id="c1", path=str(backwards), name="kick", start_beat=0.0, duration_sec=1.0,
                                        reversed_from=str(original))])]
    save_project(project, folder / "song.gilproj")

    moved = tmp_path / "new"
    folder.rename(moved)  # (both files are found again, relative to the project)
    loaded = Project()
    load_project(loaded, moved / "song.gilproj")
    clip = loaded.tracks[0].clips[0]
    assert (clip.path, clip.reversed_from) == (str(moved / "Reversed" / "kick R.wav"), str(moved / "kick.wav"))
    assert project_to_dict(loaded)["tracks"][0]["clips"][0]["reversed_from"] == str(moved / "kick.wav")


def test_rejects_foreign_files(tmp_path):
    bad = tmp_path / "x.gilproj"
    bad.write_text(json.dumps({"format": "something-else"}))
    with pytest.raises(ProjectFileError):
        load_project(Project(), bad)
    bad.write_text("{not json")
    with pytest.raises(ProjectFileError):
        load_project(Project(), bad)


def test_old_warp_mode_names_load_as_their_equivalents(tmp_path):
    audio = tmp_path / "kick.wav"
    audio.write_bytes(b"")
    target = tmp_path / "song.gilproj"
    save_project(make_project(str(audio)), target)
    data = json.loads(target.read_text(encoding="utf-8"))
    clip = data["tracks"][0]["clips"][0]
    for old, new in (("Beats", "Transients"), ("Tones", "Standard"), ("Complex", "Standard"),
                     ("Texture", "Smooth"), ("Complex Pro", "Formants"), ("Re-Pitch", "Re-Pitch"),
                     ("Nonsense", "Standard")):
        clip["warp_mode"] = old
        target.write_text(json.dumps(data), encoding="utf-8")
        loaded = Project()
        load_project(loaded, target)
        assert loaded.tracks[0].clips[0].warp_mode == new, old


def test_roundtrip_keeps_the_masters_devices(tmp_path):
    audio = tmp_path / "kick.wav"
    audio.write_bytes(b"")
    target = tmp_path / "song.gilproj"
    save_project(make_project(str(audio)), target)
    loaded = Project()
    load_project(loaded, target)
    master = loaded.track(MASTER)
    assert master is loaded.master and master.is_master and master not in loaded.tracks
    assert [(d.id, d.kind, d.params["gain"]) for d in master.devices] == [("m1", "utility", -1.0)]
    assert master.volume_db == -3.0 and master.automation == {device_key("m1", "gain"): (AutomationPoint(0.0, 0.5),)}


def test_old_project_files_load_unchanged():
    """A project saved before the master had devices (version 4)."""
    old = {
        "format": "gilstudio-project", "version": 4, "tempo": 128.0, "time_signature": [4, 4],
        "loop": {"enabled": False, "start": 0.0, "end": 16.0}, "automation_locked": False,
        "master": {"volume_db": -4.5, "pan": 0.25, "automation": {"mixer:volume": [[1.0, 0.75, 0.0]]},
                   "automation_view": {"shown": True, "key": "mixer:volume", "lanes": []}},
        "tracks": [{"id": "t1", "kind": "audio", "name": "Drums", "color": "#ff94a6", "volume_db": 0.0, "pan": 0.0,
                    "mute": False, "solo": False, "height": 80, "devices": [], "clips": [], "automation": {},
                    "automation_view": {"shown": False, "key": None, "lanes": []}}],
    }
    project = Project()
    load_into(project, old)
    master = project.master
    assert (master.volume_db, master.pan, master.devices) == (-4.5, 0.25, [])
    assert master.automation == {MIXER_VOLUME: (AutomationPoint(1.0, 0.75),)}
    assert master.automation_view == AutomationView(True, MIXER_VOLUME)
    assert [t.id for t in project.tracks] == ["t1"] and project.tempo == 128.0
    # Saved again, it says the same, and that the master has no devices.
    saved = project_to_dict(project)
    assert saved["version"] == VERSION and saved["master"] == {**old["master"], "devices": []}
    # Tracks from before version 6 have no input, Auto monitoring and aren't armed; before 8, no group;
    # before 9, no sends (nor returns).
    assert saved["tracks"] == [{**t, "input": [], "monitor": "auto", "armed": False, "parent": None,
                                "folded": False, "sends": {}}
                               for t in old["tracks"]]
    assert saved["returns"] == []
    # Very old files have no master at all.
    del old["master"]
    load_into(project, old)
    assert (project.master.volume_db, project.master.devices, project.master.automation) == (0.0, [], {})


def test_roundtrip_keeps_inputs_monitoring_and_arming(tmp_path):
    project = make_project(str(tmp_path / "kick.wav"))
    project.tracks[0].input, project.tracks[0].monitor, project.tracks[0].armed = (2, 3), "in", True
    project.tracks[1].input = (0,)
    target = tmp_path / "song.gilproj"
    save_project(project, target)
    loaded = Project()
    load_project(loaded, target)
    assert [(t.input, t.monitor, t.armed) for t in loaded.tracks] == [((2, 3), "in", True), ((0,), "auto", False)]
    # Anything this version doesn't know comes back as no input, Auto.
    data = json.loads(target.read_text())
    data["tracks"][0].update(input=[0, 1, 2], monitor="sometimes")
    load_into(loaded, data)
    assert (loaded.tracks[0].input, loaded.tracks[0].monitor) == ((), "auto")
