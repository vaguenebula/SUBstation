"""The intelligence layer's operations: the registry (schemas from types,
checking arguments), the runner (one undo step labelled for its actor, all or
nothing, if_revision, frozen tracks, the activity log), every write operation
undoing back to the model and the engine as they were, and through the engine:
MIDI clips that play, automation on a plug-in's parameter, shapes on an
unautomated parameter, the selection pinned while a batch made from it waits,
the main-thread dispatcher and the meter history."""

import json
import os
import threading
from pathlib import Path

import numpy as np
import pytest
from PySide6.QtCore import QThread
from PySide6.QtGui import QUndoStack
from PySide6.QtTest import QTest

from substation.intel.activity import ActivityLog
from substation.intel.ops import REGISTRY, OpError, OpRunner, Risk, agent, schemas
from substation.intel.ops.automation import shape_points
from substation.intel.ops.presets import vstpreset_class_id
from substation.intel.ops.registry import coerce
from substation.intel.qt.engine_facts import MeterHistory
from substation.model import automation
from substation.model.devices import new_device
from substation.model.editor import ProjectEditor
from substation.model.presets import save_to_library
from substation.model.project import Freeze, Project
from substation.model.serialization import project_to_dict

from .conftest import SAMPLE_RATE, TEST_PLUGINS, write_wav

AGENT = agent("Test")
SNAPSHOT = Path(__file__).parent / "snapshots" / "intel_ops.json"


@pytest.fixture
def runner(app):
    editor = ProjectEditor(Project(), QUndoStack())
    return OpRunner(editor.project, editor, activity=ActivityLog(persist=False))


def wait_until(predicate, timeout=5.0) -> bool:
    import time
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        QTest.qWait(10)
    return False


# --- The registry ---------------------------------------------------------------------------


def test_schemas_are_strict_and_come_from_the_types():
    schema = REGISTRY["rename_track"].schema
    assert schema["additionalProperties"] is False
    assert schema["required"] == ["track_id", "name"]
    assert schema["properties"]["name"] == {"type": "string", "description": "A name, as shown in the track header.",
                                            "minLength": 1, "maxLength": 64}
    assert "if_revision" in schema["properties"]  # every write can be based on a revision
    assert "if_revision" not in REGISTRY["get_project"].schema["properties"]  # reads can't
    shape = REGISTRY["automate_shape"].schema["properties"]
    assert shape["shape"]["enum"] == ["ramp", "swell", "dip", "step", "lfo"]
    assert shape["from_value"]["type"] == ["number", "null"]
    assert shape["restore"] == {"type": "boolean", "default": True}
    for name, op in REGISTRY.items():
        assert op.summary and op.risk in Risk, name
        json.dumps(op.schema)  # JSON all through


def test_schemas_snapshot():
    """The operations' schemas, as callers see them: a change here is deliberate
    (set UPDATE_SNAPSHOTS=1 to write the new ones)."""
    current = json.dumps(schemas(), indent=1, sort_keys=True, ensure_ascii=False) + "\n"
    if os.environ.get("UPDATE_SNAPSHOTS") or not SNAPSHOT.exists():
        SNAPSHOT.parent.mkdir(exist_ok=True)
        SNAPSHOT.write_text(current, encoding="utf-8")
    assert current == SNAPSHOT.read_text(encoding="utf-8"), "operation schemas changed: UPDATE_SNAPSHOTS=1 if meant"


def test_arguments_are_checked(runner):
    with pytest.raises(OpError) as error:
        runner.run("rename_track", {"track_id": "x", "name": "A", "colour": "red"})
    assert error.value.code == "invalid" and "colour" in error.value.message
    for args in ({"track_id": "x"}, {"track_id": "x", "name": 3}, {"track_id": "x", "name": "a" * 65},
                 {"track_id": "x", "name": None}):
        with pytest.raises(OpError) as error:
            runner.run("rename_track", args)
        assert error.value.code == "invalid", args
    with pytest.raises(OpError) as error:
        runner.run("no_such_op")
    assert error.value.code == "not_found"
    assert coerce(float, 3, "x") == 3.0 and coerce(int, 4.0, "x") == 4
    with pytest.raises(OpError):
        coerce(int, True, "x")
    with pytest.raises(OpError):
        coerce(float, float("nan"), "x")
    assert runner.call("set_tempo", {"bpm": 5})["error"]["code"] == "invalid"  # (call: errors as results)


# --- The runner ------------------------------------------------------------------------------


def test_one_undo_step_labelled_for_its_actor(runner):
    stack = runner.editor.undo_stack
    track = runner.run("add_midi_track", {"name": "Keys"}, AGENT)["track"]
    assert stack.count() == 1 and stack.text(0) == "Agent (Test): Insert MIDI Track"
    result = runner.run("create_midi_clip", {"track_id": track["id"], "start_beat": 0, "length_beats": 4,
                                             "notes": [[60, 0, 1, 90], [64, 1, 1]]}, "auto")
    assert stack.count() == 2 and stack.text(1) == "Auto: Insert MIDI Clip"  # (a clip and its notes: one step)
    assert result["clip"]["notes"] == [[60, 0.0, 1.0, 90], [64, 1.0, 1.0, 100]]
    runner.run("rename_track", {"track_id": track["id"], "name": "Lead"})
    assert stack.text(2) == "Rename Track"  # the user's own
    # Nothing changed: no empty step. Nor for what isn't undone (solo is a listening aid).
    result = runner.run("rename_track", {"track_id": track["id"], "name": "Lead"}, AGENT)
    assert result["changed"] is False and stack.count() == 3
    result = runner.run("set_track_mixer", {"track_id": track["id"], "solo": True}, AGENT)
    assert result["changed"] and runner.project.track(track["id"]).solo and stack.count() == 3


def test_a_failing_batch_leaves_no_trace(runner):
    p, stack = runner.project, runner.editor.undo_stack
    runner.run("add_audio_track", {"name": "A"})
    before, count, index = project_to_dict(p), stack.count(), stack.index()
    calls = [{"op": "add_audio_track", "args": {"name": "B"}},
             {"op": "set_track_color", "args": {"track_id": "$0.track.id", "color": "#123456"}},
             {"op": "rename_track", "args": {"track_id": "nope", "name": "C"}}]
    with pytest.raises(OpError) as error:
        runner.run_batch(calls, "Three Things", AGENT)
    assert error.value.code == "not_found" and error.value.details["index"] == 2
    assert project_to_dict(p) == before
    assert (stack.count(), stack.index(), stack.canRedo()) == (count, index, False)
    # Without the failing call: one step, the reference followed.
    result = runner.run_batch(calls[:2], "Two Things", AGENT)
    assert stack.text(stack.index() - 1) == "Agent (Test): Two Things"
    assert p.track(result["results"][0]["track"]["id"]).color == "#123456"
    assert runner.call("batch", {"calls": [{"op": "play", "args": {}}]})["error"]["code"] == "invalid"


def test_a_stale_revision_is_a_conflict(runner):
    track = runner.run("add_audio_track", {"name": "A"})
    seen = track["revision"]
    runner.run("set_track_color", {"track_id": track["track"]["id"], "color": "#ff0000", "if_revision": seen})
    with pytest.raises(OpError) as error:  # it read revision `seen`; the project moved on since
        runner.run("rename_track", {"track_id": track["track"]["id"], "name": "B", "if_revision": seen}, AGENT)
    assert error.value.code == "conflict" and error.value.details["revision"] == runner.revision.value
    assert runner.project.track(track["track"]["id"]).name == "A"
    with pytest.raises(OpError):
        runner.run_batch([{"op": "rename_track", "args": {"track_id": track["track"]["id"], "name": "B"}}],
                         if_revision=seen)


def test_frozen_tracks_refuse_edits_but_take_names(runner):
    p, stack = runner.project, runner.editor.undo_stack
    track = runner.run("add_midi_track", {"name": "Keys"})
    track_id, synth = track["track"]["id"], track["devices"][0]
    runner.editor.freeze_tracks({track_id: Freeze("frozen.wav", 2.0, 120.0)})
    count = stack.count()
    for name, args in [("add_device", {"track_id": track_id, "device": "eq"}),
                       ("set_device_param", {"track_id": track_id, "device_id": synth, "param_id": "cutoff",
                                             "value": 1000}),
                       ("create_midi_clip", {"track_id": track_id, "start_beat": 0, "length_beats": 4}),
                       ("write_automation", {"owner": track_id, "target": f"device:{synth}:cutoff",
                                             "points": [[0, 100]]})]:
        with pytest.raises(OpError) as error:
            runner.run(name, args, AGENT)
        assert error.value.code == "frozen" and "Keys is frozen" in error.value.message, name
    assert stack.count() == count
    runner.run("rename_track", {"track_id": track_id, "name": "Frozen Keys"}, AGENT)
    runner.run("set_track_mixer", {"track_id": track_id, "volume_db": -3.0}, AGENT)  # its fader stays live
    assert p.track(track_id).name == "Frozen Keys" and p.track(track_id).volume_db == -3.0
    # The editor's own refusals come back as 'frozen' too: here, taking a track out of a frozen group.
    inner = runner.run("add_audio_track", {"name": "Inner"})["track"]["id"]
    group = runner.run("group_tracks", {"track_ids": [inner]})["group"]["id"]
    runner.editor.freeze_tracks({group: Freeze("group.wav", 2.0, 120.0)})
    with pytest.raises(OpError) as error:
        runner.run("group_tracks", {"track_ids": [inner]}, AGENT)
    assert error.value.code == "frozen" and "unfreeze it" in error.value.message
    assert p.track(inner).parent == group


def test_reading_a_track_without_clips(runner):
    track = runner.run("add_audio_track", {"name": "Empty"})["track"]["id"]
    result = runner.run("get_track", {"track_id": track})["track"]
    assert result["name"] == "Empty" and "clips" not in result


def test_the_activity_log(runner, tmp_path):
    runner.activity = ActivityLog(tmp_path / "activity.jsonl")
    track = runner.run("add_audio_track", {"name": "A"}, AGENT)["track"]["id"]
    runner.run("rename_track", {"track_id": track, "name": "B"})  # the user's: not logged
    runner.run("list_tracks", {}, AGENT)  # reads: not logged
    runner.call("rename_track", {"track_id": "nope", "name": "C"}, AGENT)  # failures are
    entries = list(runner.activity.entries)
    assert [(e.actor, e.op, e.ok) for e in entries] == [(AGENT, "add_audio_track", True),
                                                        (AGENT, "rename_track", False)]
    assert entries[0].undo_index == 1 and entries[1].undo_index is None
    lines = (tmp_path / "activity.jsonl").read_text(encoding="utf-8").splitlines()
    assert [json.loads(line)["op"] for line in lines] == ["add_audio_track", "rename_track"]


def test_shapes():
    ramp = shape_points("ramp", 0.2, 0.8, 4.0, 0.5)
    assert [(p.beat, p.value, p.curve) for p in ramp] == [(0.0, 0.2, 0.5), (4.0, 0.8, 0.0)]
    swell = shape_points("swell", 0.2, 0.8, 4.0)
    assert [(p.beat, p.value) for p in swell] == [(0.0, 0.2), (4.0, 0.8), (4.0, 0.2)]
    dip = shape_points("dip", 0.6, 0.1, 2.0)
    assert automation.value_at(dip, 1.0) == pytest.approx(0.1) and automation.value_at(dip, 2.0) == 0.6
    lfo = shape_points("lfo", 0.0, 1.0, 2.0, period=1.0)
    assert automation.value_at(lfo, 0.0) == pytest.approx(0.0) and automation.value_at(lfo, 0.5) == pytest.approx(1.0)
    assert automation.value_at(lfo, 1.0) == pytest.approx(0.0, abs=1e-9)


def test_vstpreset_class_ids():
    header = b"VST3\x01\x00\x00\x006A1C0D5E3B7F4E219C8D2A105E4F7B03L\x00\x00\x00\x00\x00\x00\x00"
    assert vstpreset_class_id(header) == "5E0D1C6A7F3B214E9C8D2A105E4F7B03"  # (the test effect's uid)
    assert vstpreset_class_id(b"RIFF" + b"\x00" * 60) is None


def test_meter_history():
    history = MeterHistory(seconds=10.0)
    for i in range(20):
        history.record({"master": (0.2, 0.5 if i != 7 else 1.2)}, now=float(i) / 2)
    stats = history.stats("master", 5.0, now=9.5)
    assert stats.readings == 11 and stats.peak == 0.5 and stats.clipped == 0
    stats = history.stats("master", 10.0, now=9.5)
    assert stats.peak == 1.2 and stats.clipped == 1
    assert history.stats("nothing", 5.0, now=9.5).readings == 0


# --- Every write operation, through the window -----------------------------------------------------

VIEW_STATE = {"automation_view", "height", "folded", "folded_devices", "armed", "solo"}


def content(project) -> dict:
    """The project as saved, without view state (which operations may show, and undo doesn't take back)."""
    def strip(value):
        if isinstance(value, dict):
            return {k: strip(v) for k, v in value.items() if k not in VIEW_STATE}
        if isinstance(value, list):
            return [strip(v) for v in value]
        return value
    return strip(project_to_dict(project))


def render(window) -> np.ndarray:
    window.bridge.wait_for_device_states()
    return np.array(window.engine.render_offline(0.0, SAMPLE_RATE * 3))


def song(window, tmp_path) -> dict:
    """A small song: a kick on an audio track, a MIDI track playing notes, a return."""
    r = window.intel.runner
    kick = write_wav(tmp_path / "Kick_07.wav", np.full((SAMPLE_RATE // 2, 2), 0.5))
    audio = r.run("add_clip_from_file", {"path": str(kick), "start_beat": 0.0})
    assert wait_until(lambda: window.bridge.source(str(kick)) is not None)
    midi = r.run("add_midi_track", {"name": "Keys"})
    clip = r.run("create_midi_clip", {"track_id": midi["track"]["id"], "start_beat": 0.0, "length_beats": 4.0,
                                      "notes": [[60, 0.1, 1, 100], [67, 1.1, 1, 80]]})
    ret = r.run("add_return_track", {})
    preset = new_device("utility")
    preset.params["gain"] = -6.0
    path = save_to_library(preset, "Quieter")
    other = write_wav(tmp_path / "Snare.wav", np.full((SAMPLE_RATE // 4, 2), 0.25))
    window.undo_stack.clear()
    return {"audio": audio["track"]["id"], "audio_clip": audio["clip"]["id"], "midi": midi["track"]["id"],
            "synth": midi["devices"][0], "midi_clip": clip["clip"]["id"], "return": ret["track"]["id"],
            "preset": str(path), "other_file": str(other)}


def write_cases(ids: dict) -> list[tuple[list[tuple[str, dict]], str, dict]]:
    """(steps run first, the operation, its arguments) for each write operation."""
    a, m, s, c, mc, ret = ids["audio"], ids["midi"], ids["synth"], ids["audio_clip"], ids["midi_clip"], ids["return"]
    volume = automation.MIXER_VOLUME
    cutoff = automation.device_key(s, "cutoff")
    freeze = ("freeze_tracks", {"track_ids": [m]})
    return [
        ([], "rename_track", {"track_id": a, "name": "Kick"}),
        ([], "set_track_color", {"track_id": a, "color": "#00ff00"}),
        ([], "add_audio_track", {"name": "New", "index": 0}),
        ([], "add_midi_track", {"name": "Bass", "instrument": "sampler"}),
        ([], "add_return_track", {"name": "Verb"}),
        ([], "group_tracks", {"track_ids": [a, m]}),
        ([("group_tracks", {"track_ids": [a, m]})], "ungroup_tracks", {"group_ids": ["$group"]}),
        ([], "move_tracks", {"track_ids": [m], "index": 0}),
        ([], "duplicate_tracks", {"track_ids": [m]}),
        ([], "delete_tracks", {"track_ids": [a, ret]}),
        ([], "set_track_mixer", {"track_id": a, "volume_db": -6.0, "pan": 0.5, "mute": False}),
        ([], "set_send", {"track_id": m, "return_id": ret, "level_db": -3.0}),
        ([], "add_device", {"track_id": a, "device": "eq"}),
        ([], "remove_devices", {"track_id": m, "device_ids": [s]}),
        ([], "set_device_enabled", {"track_id": m, "device_id": s, "enabled": False}),
        ([], "set_device_param", {"track_id": m, "device_id": s, "param_id": "cutoff", "value": 300.0}),
        ([("add_device", {"track_id": a, "device": "compressor"})], "set_device_sidechain",
         {"track_id": a, "device_id": "$device", "source_track_id": m, "tap": "pre"}),
        ([], "add_clip_from_file", {"path": ids["other_file"], "start_beat": 4.0, "track_id": a}),
        ([], "move_clips", {"clip_ids": [c], "delta_beats": 2.0}),
        ([], "set_clip_props", {"clip_id": c, "gain_db": -6.0, "transpose": 3}),
        ([], "delete_clips", {"clip_ids": [c, mc]}),
        ([], "create_midi_clip", {"track_id": m, "start_beat": 4.0, "length_beats": 2.0, "notes": [[72, 0, 0.5]]}),
        ([], "write_midi_notes", {"clip_id": mc, "notes": [[48, 0, 2, 120]], "mode": "add"}),
        ([], "transform_notes", {"clip_id": mc, "action": "transpose", "semitones": 2}),
        ([], "write_automation", {"owner": a, "target": volume, "points": [[0, -20.0], [2, 0.0]]}),
        ([("write_automation", {"owner": a, "target": volume, "points": [[0, -20.0], [2, 0.0]]})],
         "clear_automation", {"owner": a, "target": volume}),
        ([], "automate_shape", {"owner": m, "target": cutoff, "start": 1.0, "end": 3.0, "shape": "swell",
                                "to": 12000.0}),
        ([("add_device", {"track_id": a, "device": "utility"})], "load_device_preset",
         {"track_id": a, "device_id": "$device", "path": ids["preset"]}),
        ([], "insert_preset", {"track_id": a, "path": ids["preset"]}),
        ([], "set_tempo", {"bpm": 100.0}),
        ([], "set_key", {"key": "Am"}),
        ([], "set_time_signature", {"numerator": 3, "denominator": 4}),
        ([], "set_loop", {"start": 0.0, "end": 8.0}),
        ([], "freeze_tracks", {"track_ids": [m]}),
        ([freeze], "unfreeze_tracks", {"track_ids": [m]}),
        ([freeze], "flatten_tracks", {"track_ids": [m]}),
        ([], "batch", {"calls": [{"op": "add_device", "args": {"track_id": a, "device": "delay"}},
                                 {"op": "set_device_param", "args": {"track_id": a, "device_id": "$0.device.id",
                                                                     "param_id": "feedback", "value": 50.0}}],
                       "label": "Echo"}),
    ]


def _fill(args, made: dict):
    """Arguments with "$group"/"$device" as the steps before made them."""
    def value(v):
        if v == "$group":
            return made["group"]
        if v == "$device":
            return made["device"]
        if isinstance(v, list):
            return [value(x) for x in v]
        return v
    return {k: value(v) for k, v in args.items()}


def test_every_write_operation_undoes_to_the_model_and_engine_as_they_were(window, tmp_path, monkeypatch):
    monkeypatch.setenv("SUBSTATION_RECORDINGS", str(tmp_path / "Recordings"))  # (freezing writes there)
    ids = song(window, tmp_path)
    r, stack = window.intel.runner, window.undo_stack
    covered = set()
    for steps, name, args in write_cases(ids):
        made = {}
        for step, step_args in steps:
            result = r.run(step, step_args)
            made |= {k: result[k]["id"] for k in ("group", "device") if k in result}
        before_model, before_audio, index = content(window.project), render(window), stack.index()
        result = r.run(name, _fill(args, made), AGENT)
        assert result["changed"], name
        assert stack.index() == index + 1, f"{name}: one undo step"
        assert stack.text(index).startswith("Agent (Test): "), name
        assert content(window.project) != before_model, name
        stack.undo()
        assert content(window.project) == before_model, f"{name}: undo restores the model"
        np.testing.assert_allclose(render(window), before_audio, atol=1e-5, err_msg=f"{name}: and the engine")
        for _ in steps:
            stack.undo()
        covered.add(name)
    undoable = {name for name, op in REGISTRY.items() if op.undoable}
    assert undoable - covered == set()


# --- Through the engine ---------------------------------------------------------------------------


def test_a_midi_clip_made_with_notes_plays_them(window):
    r = window.intel.runner
    track = r.run("add_midi_track", {"name": "Keys"}, AGENT)["track"]["id"]
    count = window.undo_stack.count()
    r.run("create_midi_clip", {"track_id": track, "start_beat": 2.0, "length_beats": 2.0,
                               "notes": [[69, 0.0, 1.0, 127]]}, AGENT)
    assert window.undo_stack.count() == count + 1
    out = render(window)[:, 0]  # 120 BPM: beat 2 is 1 s, the note lasts 0.5 s
    assert np.abs(out[:SAMPLE_RATE - 100]).max() == 0.0
    assert np.abs(out[SAMPLE_RATE + 1000:SAMPLE_RATE + 20000]).max() > 0.05
    window.undo_stack.undo()
    assert np.abs(render(window)).max() == 0.0


@pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")
def test_automation_on_a_plugin_parameter_plays_in_time_and_setting_it_overrides(window, tmp_path):
    r = window.intel.runner
    index = window.browser.plugin_index
    assert wait_until(lambda: not index.scanning and index.plugins)
    effect = next(p for p in r.run("list_available_devices", {"query": "sub test effect"})["devices"])
    assert effect["vst3_category"] == "Fx|Delay" and effect["category"] == "Audio Effects"
    wav = write_wav(tmp_path / "Pad.wav", np.full((SAMPLE_RATE * 3, 2), 0.5))
    track = r.run("add_clip_from_file", {"path": str(wav)})["track"]["id"]
    assert wait_until(lambda: window.bridge.source(str(wav)) is not None)
    device = r.run("add_device", {"track_id": track, "device": effect["id"]}, AGENT)["device"]
    params = r.run("get_device_params", {"track_id": track, "device_id": device["id"], "query": "gain"})["params"]
    assert [p["name"] for p in params] == ["Gain"] and params[0]["value"] == pytest.approx(0.5)
    gain = params[0]["key"]
    # Plain values in (a plug-in's own 0..1): x0.5 for the first second, x1.5 after (its gain is 2 * value).
    r.run("write_automation", {"owner": track, "target": gain, "points": [[0, 0.25], [2, 0.25], [2, 0.75]]}, AGENT)
    out = render(window)[:, 0]
    window.bridge.poll_plugins()  # (as the app does: the engine's values follow the envelope)
    np.testing.assert_allclose(out[1000:SAMPLE_RATE - 1000], 0.25, atol=1e-4)
    np.testing.assert_allclose(out[SAMPLE_RATE + 1000:2 * SAMPLE_RATE], 0.75, atol=1e-4)
    assert window.bridge.own_value(track, gain) == pytest.approx(0.5)  # its own, not the envelope's
    # Setting it by hand overrides its automation, as turning its knob does; the result says so.
    result = r.run("set_device_param", {"track_id": track, "device_id": device["id"], "param_id": params[0]["id"],
                                        "value": 0.5}, AGENT)
    assert result["overrides_automation"] is True and result["text"]
    assert window.bridge.is_overridden(track, gain)
    np.testing.assert_allclose(render(window)[1000:2 * SAMPLE_RATE, 0], 0.5, atol=1e-4)
    r.run("re_enable_automation", {"owner": track}, AGENT)
    np.testing.assert_allclose(render(window)[1000:SAMPLE_RATE - 1000, 0], 0.25, atol=1e-4)
    window.bridge.poll_plugins()
    # Without its envelope it is back at its own value, not the envelope's last.
    r.run("clear_automation", {"owner": track, "target": gain}, AGENT)
    np.testing.assert_allclose(render(window)[1000:2 * SAMPLE_RATE, 0], 0.5, atol=1e-4)


def test_a_swell_on_an_unautomated_parameter_keeps_its_value_outside(window, tmp_path):
    r = window.intel.runner
    wav = write_wav(tmp_path / "Pad.wav", np.full((SAMPLE_RATE * 3, 2), 0.5))
    track = r.run("add_clip_from_file", {"path": str(wav)})["track"]["id"]
    assert wait_until(lambda: window.bridge.source(str(wav)) is not None)
    utility = r.run("add_device", {"track_id": track, "device": "utility"})["device"]["id"]
    r.run("set_device_param", {"track_id": track, "device_id": utility, "param_id": "gain", "value": -6.0206})
    key = automation.device_key(utility, "gain")
    result = r.run("automate_shape", {"owner": track, "target": key, "start": 2.0, "end": 4.0, "shape": "swell",
                                      "to": 0.0}, AGENT)
    assert result["points"][0][:2] == [2.0, pytest.approx(-6.0206)]
    envelope = window.project.envelope(track, key)  # (normalized)
    own = automation.value_at(envelope, 0.0)
    assert automation.value_at(envelope, 1.0) == own == automation.value_at(envelope, 4.5)
    assert automation.value_at(envelope, 2.0) == own < automation.value_at(envelope, 3.0) \
        < automation.value_at(envelope, 3.99)
    out = render(window)[:, 0]  # 120 BPM: the swell is from 1 s to 2 s
    np.testing.assert_allclose(out[1000:SAMPLE_RATE - 100], 0.25, atol=1e-3)  # before: its own -6 dB
    np.testing.assert_allclose(out[2 * SAMPLE_RATE + 2000:3 * SAMPLE_RATE - 1000], 0.25, atol=1e-3)  # after (smoothed)
    assert out[2 * SAMPLE_RATE - 600] == pytest.approx(0.5, abs=0.02)  # up to 0 dB at its end
    assert 0.3 < out[SAMPLE_RATE + SAMPLE_RATE // 2] < 0.45


def test_a_batch_made_from_the_selection_means_the_same_when_the_selection_changes(window, tmp_path):
    r = window.intel.runner
    a = r.run("add_audio_track", {"name": "A"})["track"]["id"]
    b = r.run("add_audio_track", {"name": "B"})["track"]["id"]
    window.selection.set_time_range(4.0, 8.0, [a, b])
    selection = r.run("get_selection", {}, AGENT)
    assert selection["applies_to"] == "time_range"
    assert selection["time_range"]["start"] == 4.0 and selection["time_range"]["start_text"] == "2.1.1"
    assert [t["id"] for t in selection["time_range"]["tracks"]] == [a, b]
    span = selection["time_range"]
    calls = [{"op": "automate_shape", "args": {"owner": t["id"], "target": "mixer:volume", "start": span["start"],
                                               "end": span["end"], "shape": "ramp", "to": -12.0}}
             for t in span["tracks"]]
    window.selection.set_time_range(12.0, 16.0, [b])  # the user clicks on while the agent works
    r.run_batch(calls, "Fade Out", AGENT)
    for track in (a, b):
        assert [p.beat for p in window.project.envelope(track, "mixer:volume")][:2] == [4.0, 4.0] or \
            window.project.envelope(track, "mixer:volume")[0].beat == 4.0
    # The snapshot stays readable by its id; the current selection is another.
    assert r.run("get_selection", {"selection_id": selection["selection_id"]})["time_range"]["start"] == 4.0
    assert r.run("get_selection")["time_range"]["start"] == 12.0


def test_calls_from_other_threads_run_on_the_main_thread(window):
    intel = window.intel
    results, threads = {}, {}
    from substation.intel.ops.registry import operation
    main = QThread.currentThread()

    @operation(risk=Risk.READ, summary="test: which thread", name="_which_thread")
    def which_thread(ctx) -> dict:
        threads["ran"] = QThread.currentThread() == main
        return {}

    try:
        worker = threading.Thread(target=lambda: results.update(
            tracks=intel.call_from_thread("list_tracks", actor=AGENT),
            which=intel.call_from_thread("_which_thread")))
        worker.start()
        assert wait_until(lambda: "which" in results)
        worker.join(5.0)
    finally:
        del REGISTRY["_which_thread"]
    assert results["tracks"]["master"]["id"] == "master" and threads["ran"]


def test_a_main_thread_that_doesnt_answer_is_busy(window):
    errors = []
    worker = threading.Thread(target=lambda: errors.append(
        _call_or_error(lambda: window.intel.dispatcher.call(lambda: 1, timeout=0.2))))
    worker.start()
    worker.join(5.0)  # (the main thread is in join: it can't answer)
    assert errors == ["busy"]
    QTest.qWait(50)  # the call that timed out never runs


def test_a_call_that_starts_but_doesnt_finish_in_time_is_busy(window):
    errors = []
    started = threading.Event()

    def slow():
        started.set()
        QTest.qWait(400)  # (still on the main thread: the worker's second wait runs out)
        return 1

    worker = threading.Thread(target=lambda: errors.append(
        _call_or_error(lambda: window.intel.dispatcher.call(slow, timeout=0.15))))
    worker.start()
    assert wait_until(started.is_set)
    while worker.is_alive():
        QTest.qWait(10)
    assert errors == ["busy"]


def _call_or_error(call):
    try:
        call()
    except OpError as error:
        return error.code
    return None


def test_the_window_feeds_the_meter_history(window):
    facts = window.intel.engine
    window.bridge.meters["master"] = (0.3, 0.6)
    window.bridge.meters_updated.emit()
    stats = window.intel.run("get_meter_history", {"strip_id": "master", "seconds": 1.0})["meter"]
    assert stats["readings"] >= 1 and stats["peak"] >= 0.6
    assert facts.transport().playing is False
