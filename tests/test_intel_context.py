"""The song as the intelligence layer describes it: names cleaned of tempo, key,
pack prefixes and numbering; track roles from file names, devices and MIDI on
hand-built projects; MIDI statistics of the notes that play; and SongContext,
built whole or kept up to date track by track."""

import json

import pytest
from PySide6.QtGui import QUndoStack

from substation.intel.context.revision import IntelRevision
from substation.intel.context.roles import classify, clean_name, midi_stats, name_roles
from substation.intel.context.song import SongContextBuilder, build_context
from substation.model import device_state
from substation.model.devices import new_chain, new_device, new_rack
from substation.model.editor import ProjectEditor
from substation.model.project import Clip, MidiClip, Note, Project, Track


@pytest.fixture
def editor(app):
    return ProjectEditor(Project(), QUndoStack())


def audio(name: str, *files: str) -> Track:
    clips = [Clip(id=f"c{i}", path=f"C:/samples/{f}", name=f, start_beat=i * 4.0, duration_sec=1.0,
                  source_duration_sec=1.0) for i, f in enumerate(files)]
    return Track(id=name, name=name, color="#fff", clips=clips)


def midi(name: str, notes, devices=(), offset=0.0, length=16.0) -> Track:
    clip = MidiClip(id="m", name=name, start_beat=0.0, duration_beats=length, offset_beats=offset,
                    notes=tuple(Note(*n) for n in notes))
    return Track(id=name, name=name, color="#fff", kind="midi", clips=[clip], devices=list(devices))


@pytest.mark.parametrize("name, cleaned", [
    ("Bass_Loop_128_Am.wav", "Bass Loop"),
    ("KSHMR_Kick_07.wav", "Kick"),
    ("Top_Loop_128_Am.wav", "Top Loop"),
    ("Vox_Chop_03.wav", "Vox Chop"),
    ("PD Dreamy Glass 120bpm F#min.wav", "Dreamy Glass"),
    ("hihat-closed-02.flac", "hihat closed"),
])
def test_names_are_cleaned(name, cleaned):
    assert clean_name(name) == cleaned


def test_name_roles():
    assert name_roles("KSHMR_Kick_07.wav") == ["kick"]
    assert name_roles("Vox_Chop_01.wav") == ["vocal", "vocal"]
    assert name_roles("Bass_Loop_128_Am.wav") == ["bass"]
    assert name_roles("Audio 0001.wav") == []


def test_roles_from_file_names():
    role = classify(audio("1 Audio", "KSHMR_Kick_07.wav"))
    assert role.label == "kick" and role.confidence >= 0.5
    assert any("Kick_07" in e for e in role.evidence)
    assert classify(audio("2 Audio", "Kick_01.wav", "Snare_02.wav", "HH_Closed_03.wav")).label == "drums"
    assert classify(audio("3 Audio", "Vox_Chop_01.wav", "Vox_Chop_02.wav")).label == "vocal"
    assert classify(audio("4 Audio", "Audio 0001.wav")).label == "other"  # names that say nothing
    assert classify(Track(id="g", name="Group", color="#fff", kind="group")).label == "other"


def test_frozen_audio_is_no_evidence():
    from substation.model.project import Freeze
    track = audio("5 Audio", "Kick_01.wav")
    track.frozen = Freeze("C:/song/Freeze/Pad Freeze.wav", 4.0, 120.0)
    track.clips.append(Clip(id="f", path=track.frozen.path, name="Pad Freeze", start_beat=0.0, duration_sec=4.0))
    assert classify(track).label == "kick"


def test_roles_from_midi():
    bass = midi("1 MIDI", [(36 + (i % 3), i * 1.0, 0.9) for i in range(16)])  # low, one note at a time
    assert classify(bass).label == "bass"
    pad = midi("2 MIDI", [(p, s, 4.0) for s in (0.0, 4.0, 8.0) for p in (60, 64, 67, 71)])  # held chords
    assert classify(pad).label == "pad"
    stabs = midi("3 MIDI", [(p, s, 0.25) for s in (0.0, 1.0, 2.0) for p in (60, 64, 67)])
    assert classify(stabs).label == "chords"
    drums = midi("4 MIDI", [(36, i, 0.1) for i in range(0, 16, 2)] + [(38, i, 0.1) for i in range(1, 16, 2)]
                 + [(42, i / 2, 0.1) for i in range(32)])
    role = classify(drums)
    assert role.label == "drums" and any("drum keys" in e for e in role.evidence)
    lead = midi("5 MIDI", [(72 + (i * 5) % 12, i * 0.5, 0.4) for i in range(16)])
    assert classify(lead).label == "lead"


def test_midi_stats_count_only_the_notes_that_play():
    # The clip is a window from content beat 4 on: the notes before it don't play.
    track = midi("1 MIDI", [(30, 0.0, 1.0), (31, 1.0, 1.0), (72, 4.0, 1.0), (76, 5.0, 1.0)], offset=4.0, length=4.0)
    stats = midi_stats(track.clips)
    assert stats.notes == 2 and (stats.pitch_min, stats.pitch_max) == (72, 76)
    assert classify(track).label == "lead"


def test_roles_from_devices():
    sampler = new_device("sampler")
    sampler.state = device_state.to_model({"sample": "C:/samples/Piano_C3.wav"})
    assert classify(midi("1 MIDI", [(60, 0.0, 1.0)], devices=[sampler])).label == "keys"
    chains = []
    for name in ("Kick", "Snare", "Hat"):
        chains.append(new_chain(name, [new_device("sampler")]))
    rack = new_rack(chains)
    rack.chains[0].devices.insert(0, new_device("synth"))
    role = classify(midi("2 MIDI", [(60, 0.0, 1.0)], devices=[rack]))
    assert role.label == "drums" and any("rack of drum sounds" in e for e in role.evidence)
    track = audio("Backing Vocals", "take.wav")  # the track's own name, when it isn't a default one
    assert classify(track).label == "vocal"


def test_the_context_is_json_and_has_two_details(editor):
    p = editor.project
    p.tracks.extend([audio("Kicks", "Kick_07.wav"), midi("Keys", [(60, 0.0, 4.0), (64, 0.0, 4.0), (67, 0.0, 4.0)])])
    context = build_context(p)
    summary = context.to_dict()
    json.dumps(summary)
    assert summary["project"]["tempo"] == 120.0 and summary["project"]["length_beats"] == 16.0
    kicks, keys = summary["tracks"]
    assert kicks["role"] == {"label": "kick", "confidence": pytest.approx(kicks["role"]["confidence"])}
    assert kicks["clips"]["files"] == ["Kick_07.wav"]
    assert keys["midi"]["notes_per_onset"] == 3.0
    full = context.to_dict("full")
    assert full["tracks"][0]["role"]["evidence"] and full["tracks"][0]["clips"]["list"]
    assert context.track("master").kind == "master"


def test_the_builder_rebuilds_only_what_changed(editor):
    p = editor.project
    revision = IntelRevision(p)
    builder = SongContextBuilder(p, revision)
    a = editor.add_audio_track(name="A")
    b = editor.add_midi_track(name="B")
    first = builder.context()
    assert [t.name for t in first.tracks] == ["A", "B"]
    assert builder.context() is first  # nothing changed: the same one
    rebuilt = builder.rebuilt
    editor.rename_track(a.id, "Kick")
    second = builder.context()
    assert builder.rebuilt == rebuilt + 1 and second.tracks[0].name == "Kick"
    assert second.tracks[1] is first.tracks[1]  # B is reused
    assert second.project.revision == revision.value
    editor.set_tempo(90.0)  # every track again (unwarped clips' ends move)
    rebuilt = builder.rebuilt
    assert builder.context().project.tempo == 90.0 and builder.rebuilt == rebuilt + 3  # two tracks, the master
    editor.delete_tracks([b.id])
    assert [t.name for t in builder.context().tracks] == ["Kick"]
    editor.undo_stack.undo()
    assert [t.name for t in builder.context().tracks] == ["Kick", "B"]
