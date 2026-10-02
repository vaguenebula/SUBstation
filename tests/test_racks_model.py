"""Racks in the model: grouping devices (Ctrl+G) and ungrouping, chains and
their mixers, devices moving into and out of racks, nesting limits, macros,
automation of nested devices and chain faders, sidechains into devices in
racks, undo, saving, presets (fresh ids, plug-in states, macro mappings); and
the engine taking it all through the bridge, rendering as the model says."""

import copy

import numpy as np
import pytest
from PySide6.QtGui import QUndoStack

from gilstudio import _engine as ge
from gilstudio.model import automation
from gilstudio.model.automation import AutomationPoint
from gilstudio.model.editor import ProjectEditor, device_is_instrument, device_name
from gilstudio.model.project import (
    MAX_RACK_DEPTH,
    PLUGIN_KIND,
    RACK_KIND,
    MacroMapping,
    PluginRef,
    Project,
    Sidechain,
    container_of,
    device_path,
    iter_devices,
    macro_param,
)
from gilstudio.model.serialization import (
    ProjectFileError,
    device_to_preset,
    load_into,
    load_preset,
    preset_device,
    project_to_dict,
    save_preset,
)

from .conftest import SAMPLE_RATE, TEST_PLUGINS

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM
needs_plugins = pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")


@pytest.fixture
def editor(app):
    return ProjectEditor(Project(), QUndoStack())


def utilities(editor, track, count):
    return [editor.add_device(track.id, "utility") for _ in range(count)]


def ids(devices):
    return [d.id for d in devices]


def test_grouping_devices_into_a_rack_and_back(editor):
    p, stack = editor.project, editor.undo_stack
    track = editor.add_audio_track()
    a, b, c = utilities(editor, track, 3)
    editor.set_envelope(track.id, automation.device_key(b.id, "gain"), [AutomationPoint(0.0, 0.5)])
    steps = stack.count()
    rack = editor.group_devices(track.id, [c.id, b.id])  # in chain order, where the first was
    assert stack.count() == steps + 1 and stack.undoText() == "Group Devices"
    devices = p.track(track.id).devices
    assert ids(devices) == [a.id, rack.id] and rack.is_rack and rack.kind == RACK_KIND
    assert len(rack.chains) == 1 and ids(rack.chains[0].devices) == [b.id, c.id]
    assert rack.chains[0].name == "Utility" and device_name(rack) == "Audio Effect Rack"
    assert rack.params == {macro_param(i): 0.0 for i in range(8)}
    # Nested devices are the track's: found wherever they are, their automation still theirs.
    assert p.device(track.id, c.id) is rack.chains[0].devices[1]
    assert device_path(devices, c.id) == (1, 0, 1) and container_of(devices, c.id) == rack.chains[0].id
    assert container_of(devices, a.id) is None and p.device_owner(c.id) == track.id
    assert p.envelope(track.id, automation.device_key(b.id, "gain"))
    assert editor.group_devices(track.id, [a.id, b.id]) is None  # not in one chain
    stack.undo()
    assert ids(p.track(track.id).devices) == [a.id, b.id, c.id]
    stack.redo()
    assert editor.ungroup_rack(track.id, rack.id)
    assert ids(p.track(track.id).devices) == [a.id, b.id, c.id] and stack.undoText() == "Ungroup Rack"
    assert not editor.ungroup_rack(track.id, a.id)
    stack.undo()
    assert ids(p.track(track.id).devices) == [a.id, rack.id]


def test_chains_and_their_mixers(editor):
    p, stack = editor.project, editor.undo_stack
    track = editor.add_audio_track()
    (a,) = utilities(editor, track, 1)
    rack = editor.group_devices(track.id, [a.id])
    first = rack.chains[0]
    second = editor.add_rack_chain(track.id, rack.id)
    assert second.name == "Chain 2" and ids(p.device(track.id, rack.id).chains) == [first.id, second.id]
    inner = editor.add_device(track.id, "utility", chain=second.id)
    assert ids(p.chain(track.id, second.id).devices) == [inner.id] and p.chain_rack(track.id, second.id).id == rack.id
    editor.rename_chain(track.id, second.id, "Wet")
    for attr, value in (("volume_db", -6.0), ("pan", 0.5), ("mute", True), ("solo", True)):
        editor.set_chain_param(track.id, second.id, attr, value)
        assert getattr(p.chain(track.id, second.id), attr) == value
    assert p.chain(track.id, second.id).name == "Wet"
    editor.set_chain_param(track.id, second.id, "volume_db", 100.0)  # clamped like a fader
    assert p.chain(track.id, second.id).volume_db == automation.MAX_VOLUME_DB
    for _ in range(6):
        stack.undo()
    assert p.chain(track.id, second.id).volume_db == 0.0 and p.chain(track.id, second.id).name == "Chain 2"
    copy = editor.duplicate_rack_chain(track.id, second.id)  # new devices, the same settings
    assert copy.id != second.id and len(copy.devices) == 1 and copy.devices[0].id != inner.id
    editor.move_rack_chain(track.id, copy.id, 0)
    assert ids(p.device(track.id, rack.id).chains) == [copy.id, first.id, second.id]
    # A chain going takes its devices' automation, and its fader's, along.
    editor.set_envelope(track.id, automation.device_key(inner.id, "gain"), [AutomationPoint(0.0, 0.5)])
    fader = automation.chain_key(rack.id, second.id, automation.CHAIN_VOLUME)
    editor.set_envelope(track.id, fader, [AutomationPoint(0.0, 0.5)])
    assert automation.key_chain(fader) == second.id and automation.key_device(fader) == rack.id
    editor.remove_rack_chains(track.id, [second.id])
    assert ids(p.device(track.id, rack.id).chains) == [copy.id, first.id]
    assert not p.automation(track.id)
    stack.undo()
    assert len(p.automation(track.id)) == 2 and p.chain(track.id, second.id).devices[0].id == inner.id


def test_moving_devices_into_and_out_of_racks(editor):
    p = editor.project
    track = editor.add_audio_track()
    a, b, c = utilities(editor, track, 3)
    rack = editor.group_devices(track.id, [c.id])
    chain = rack.chains[0].id
    assert editor.move_devices(track.id, [a.id, b.id], 0, chain)  # in, before what is there
    assert ids(p.track(track.id).devices) == [rack.id] and ids(p.chain(track.id, chain).devices) == [a.id, b.id, c.id]
    assert editor.move_devices(track.id, [b.id], 5)  # out again, last
    assert ids(p.track(track.id).devices) == [rack.id, b.id]
    assert not editor.move_devices(track.id, [rack.id], 0, chain)  # not into itself
    inner = editor.group_devices(track.id, [a.id])  # a rack in the rack
    assert not editor.move_devices(track.id, [rack.id], 0, inner.chains[0].id)
    editor.move_device(track.id, a.id, 0)  # (alone in its chain: nothing)
    # Another track's rack chain, with the automation going along.
    other = editor.add_audio_track()
    other_rack = editor.group_devices(other.id, [editor.add_device(other.id, "utility").id])
    editor.set_envelope(track.id, automation.device_key(a.id, "gain"), [AutomationPoint(0.0, 0.5)])
    assert editor.move_devices_to_track(track.id, [inner.id], other.id, 0, other_rack.chains[0].id)
    assert ids(p.chain(other.id, other_rack.chains[0].id).devices)[0] == inner.id
    assert p.device(other.id, a.id) and not p.has_device(track.id, a.id)
    assert p.envelope(other.id, automation.device_key(a.id, "gain")) and not p.automation(track.id)


def test_racks_nest_at_most_max_rack_depth_deep(editor):
    track = editor.add_audio_track()
    (device,) = utilities(editor, track, 1)
    racks = []
    for _ in range(MAX_RACK_DEPTH):
        racks.append(editor.group_devices(track.id, [device.id if not racks else racks[-1].id]))
    assert all(racks)
    assert editor.group_devices(track.id, [racks[-1].id]) is None
    deepest = racks[0].chains[0].id  # the innermost rack's chain
    assert editor.add_device(track.id, RACK_KIND, chain=deepest) is None
    assert editor.add_device(track.id, "utility", chain=deepest) is not None


def test_instrument_racks(editor):
    p = editor.project
    keys = editor.add_midi_track()
    synth = keys.devices[0]
    rack = editor.group_devices(keys.id, [synth.id])
    assert device_is_instrument(rack) and device_name(rack) == "Instrument Rack"
    second = editor.add_rack_chain(keys.id, rack.id)
    layered = editor.add_device(keys.id, "synth", chain=second.id)  # an instrument in each chain
    assert layered is not None and ids(p.chain(keys.id, second.id).devices) == [layered.id]
    fx = editor.add_device(keys.id, "utility", index=0)  # effects go after the instrument rack
    assert ids(p.track(keys.id).devices) == [rack.id, fx.id]
    assert editor.add_device(editor.add_audio_track().id, "synth") is None  # not on an audio track, as ever


def test_macros_move_the_parameters_mapped_to_them(editor):
    p, stack = editor.project, editor.undo_stack
    track = editor.add_audio_track()
    a, b = utilities(editor, track, 2)
    rack = editor.group_devices(track.id, [a.id, b.id])
    editor.map_macro(track.id, rack.id, 0, a.id, "gain")  # -60..24 dB over the macro
    editor.map_macro(track.id, rack.id, 0, b.id, "width", 1.0, 0.5)  # 200..100 %, the other way round
    assert editor.macro_of(track.id, a.id, "gain") == (rack.id, 0)
    assert editor.macro_of(track.id, a.id, "pan") is None
    with pytest.raises(ValueError):  # not in the rack
        editor.map_macro(track.id, rack.id, 1, editor.add_device(track.id, "utility").id, "gain")
    editor.set_macro(track.id, rack.id, 0, 0.5, merge_key="drag")
    editor.set_macro(track.id, rack.id, 0, 0.75, merge_key="drag")  # one gesture: one step
    assert stack.undoText() == "Change Macro"
    assert p.device(track.id, rack.id).params[macro_param(0)] == 0.75
    assert p.device(track.id, a.id).params["gain"] == pytest.approx(-60 + 0.75 * 84)
    assert p.device(track.id, b.id).params["width"] == pytest.approx(200 - 0.75 * 100)
    stack.undo()
    assert p.device(track.id, rack.id).params[macro_param(0)] == 0.0
    assert p.device(track.id, a.id).params["gain"] == 0.0 and p.device(track.id, b.id).params["width"] == 100.0
    # A mapped device going takes its mapping along (in the same step).
    editor.remove_device(track.id, a.id)
    assert p.device(track.id, rack.id).macros == (MacroMapping(0, b.id, "width", 1.0, 0.5),)
    stack.undo()
    assert len(p.device(track.id, rack.id).macros) == 2
    editor.unmap_macro(track.id, rack.id, b.id, "width")
    assert p.device(track.id, rack.id).macros == (MacroMapping(0, a.id, "gain"),)


def test_sidechains_into_devices_in_racks_are_routing_edges(editor):
    p = editor.project
    kick, bass = editor.add_audio_track(), editor.add_audio_track()
    comp = editor.add_device(bass.id, "utility")
    rack = editor.group_devices(bass.id, [comp.id])
    editor.set_device_sidechain(bass.id, comp.id, Sidechain(kick.id))
    assert p.would_cycle(kick.id, kick.id) and p.input_would_cycle(kick.id, bass.id)  # bass is fed by kick
    with pytest.raises(ValueError):
        editor.set_track_input_track(kick.id, bass.id)
    # Grouping the source with the device's track drops a sidechain that would close a cycle.
    group = editor.group_tracks([kick.id])
    editor.set_device_sidechain(bass.id, comp.id, Sidechain(group.id))
    editor.move_tracks([bass.id], p.track_index(kick.id) + 1, group.id)
    assert p.device(bass.id, comp.id).sidechain is None
    editor.undo_stack.undo()
    assert p.device(bass.id, comp.id).sidechain == Sidechain(group.id)
    editor.delete_tracks([group.id])  # the source going takes it along
    assert p.device(bass.id, comp.id).sidechain is None and p.device(bass.id, rack.id)


def test_racks_are_saved_and_loaded(editor):
    p = editor.project
    track = editor.add_audio_track()
    a, b = utilities(editor, track, 2)
    rack = editor.group_devices(track.id, [a.id, b.id])
    wet = editor.add_rack_chain(track.id, rack.id, name="Wet")
    editor.move_devices(track.id, [b.id], 0, wet.id)
    editor.set_chain_param(track.id, wet.id, "volume_db", -3.0)
    editor.set_chain_param(track.id, wet.id, "solo", True)
    editor.map_macro(track.id, rack.id, 2, b.id, "pan", 0.25, 0.75)
    editor.set_envelope(track.id, automation.chain_key(rack.id, wet.id, automation.CHAIN_PAN),
                        [AutomationPoint(1.0, 0.25)])
    source = editor.add_audio_track()
    editor.set_device_sidechain(track.id, a.id, Sidechain(source.id))
    data = project_to_dict(p)
    loaded = Project()
    load_into(loaded, data)
    assert project_to_dict(loaded) == data
    again = loaded.device(track.id, rack.id)
    assert [c.name for c in again.chains] == [rack.chains[0].name, "Wet"]
    assert again.chains[1].volume_db == -3.0 and again.chains[1].solo
    assert again.macros == (MacroMapping(2, b.id, "pan", 0.25, 0.75),)
    assert loaded.device(track.id, a.id).sidechain == Sidechain(source.id)
    # A mapping to a device not in its rack (an edited file) goes; so does a sidechain closing a cycle.
    data["tracks"][0]["devices"][0]["macros"].append({"macro": 0, "device": "elsewhere", "param": "gain"})
    data["tracks"][0]["devices"][0]["chains"][0]["devices"][0]["sidechain"] = {"track": track.id, "tap": "post"}
    load_into(loaded, data)
    assert len(loaded.device(track.id, rack.id).macros) == 1
    assert loaded.device(track.id, a.id).sidechain is None


def test_presets_load_as_new_devices(editor, tmp_path):
    p = editor.project
    track = editor.add_audio_track()
    a, b = utilities(editor, track, 2)
    rack = editor.group_devices(track.id, [a.id, b.id])
    editor.map_macro(track.id, rack.id, 0, b.id, "gain")
    editor.set_device_sidechain(track.id, a.id, Sidechain(editor.add_audio_track().id))
    path = tmp_path / "Rack.gilpreset"
    save_preset(p.device(track.id, rack.id), path)
    first, second = load_preset(path), load_preset(path)
    all_ids = [{d.id for d in iter_devices([x])} | {c.id for c in x.chains} for x in (first, second, rack)]
    assert not (all_ids[0] & all_ids[1]) and not (all_ids[0] & all_ids[2])  # each load is new
    assert [len(x.chains[0].devices) for x in (first, second)] == [2, 2]
    assert first.macros[0].device_id == first.chains[0].devices[1].id  # mappings follow their devices
    assert all(d.sidechain is None for d in iter_devices([first]))  # sidechains name the project's tracks
    assert editor.insert_device(track.id, first) and editor.insert_device(track.id, second)
    assert len(p.track(track.id).devices) == 3
    with pytest.raises(ProjectFileError):
        preset_device({"format": "something else"})
    with pytest.raises(ProjectFileError):
        preset_device({**device_to_preset(rack), "version": 99})


# --- The engine, through the bridge ---------------------------------------------------------


@pytest.fixture
def bridged(app):
    from gilstudio.audio.engine_bridge import EngineBridge

    engine = ge.Engine()
    engine.set_clip_fade_ms(0)
    project = Project()
    editor = ProjectEditor(project, QUndoStack())
    bridge = EngineBridge(engine, project)
    editor.set_param_info(bridge.device_param_info)
    yield editor, bridge, engine
    bridge.shutdown()
    engine.close_device()


def noise_track(editor, bridge, make_wav, seed=1):
    from gilstudio.model.project import Clip

    path = make_wav(np.random.default_rng(seed).uniform(-0.5, 0.5, (SAMPLE_RATE, 2)))
    bridge.engine.load_source(path)
    track = editor.add_audio_track()
    editor.project.set_clips(track.id, [Clip(id="c", path=path, name="noise", start_beat=0.0, duration_sec=1.0,
                                             source_duration_sec=1.0)])
    return track


def render(bridge, beats=1.0):
    bridge.wait_for_device_states()
    return bridge.engine.render_offline(0.0, int(beats * SPB))


def engine_tree(bridge, track_id):
    """The engine's devices on a track: processor ids, and for a rack its chains' trees."""
    engine = bridge.engine

    def chain(chain_id):
        out = []
        for pid in engine.chain_processors(chain_id):
            if engine.processor_info(pid).type_id == "rack":
                out.append((pid, [chain(c) for c in engine.rack_chains(pid)]))
            else:
                out.append(pid)
        return out

    return chain(engine.track_chain(bridge._track_ids[track_id]))


def test_the_engine_follows_racks(bridged, make_wav):
    editor, bridge, _engine = bridged
    p, stack = editor.project, editor.undo_stack
    track = noise_track(editor, bridge, make_wav)
    raw = render(bridge)  # the clip, nothing in its way
    a, b = utilities(editor, track, 2)
    editor.set_device_param(track.id, a.id, "gain", -6.0)
    render(bridge)  # (the gain glides to -6 dB, once)
    plain = render(bridge)
    pa, pb = bridge.engine_device_id(track.id, a.id), bridge.engine_device_id(track.id, b.id)
    rack = editor.group_devices(track.id, [a.id, b.id])  # the same processors, in the rack's chain
    pr = bridge.engine_device_id(track.id, rack.id)
    assert engine_tree(bridge, track.id) == [(pr, [[pa, pb]])]
    np.testing.assert_array_equal(render(bridge), plain)  # one chain: as without the rack
    wet = editor.add_rack_chain(track.id, rack.id)  # an empty chain beside it: its input
    np.testing.assert_allclose(render(bridge), plain + raw, atol=1e-6)
    editor.set_chain_param(track.id, wet.id, "mute", True)
    np.testing.assert_array_equal(render(bridge), plain)
    editor.set_chain_param(track.id, wet.id, "mute", False)
    editor.set_chain_param(track.id, wet.id, "solo", True)
    np.testing.assert_allclose(render(bridge), raw, atol=1e-6)
    editor.set_chain_param(track.id, wet.id, "solo", False)
    editor.set_chain_param(track.id, wet.id, "volume_db", -120.0)  # the faders' floor: silent
    np.testing.assert_allclose(render(bridge), plain, atol=1e-6)
    # Moving a device out of the rack, and undoing everything: the same processors throughout.
    editor.move_devices(track.id, [b.id], 2)
    assert engine_tree(bridge, track.id) == [(pr, [[pa], []]), pb]
    while p.has_device(track.id, rack.id):
        stack.undo()
    assert engine_tree(bridge, track.id) == [pa, pb]
    np.testing.assert_array_equal(render(bridge), plain)
    while stack.canRedo():
        stack.redo()
    pr = bridge.engine_device_id(track.id, rack.id)  # (a new rack: it wasn't kept)
    assert engine_tree(bridge, track.id) == [(pr, [[pa], []]), pb]
    # The rack to another track, with everything in it; then deleted, and back.
    other = editor.add_audio_track()
    editor.move_devices_to_track(track.id, [rack.id], other.id)
    assert engine_tree(bridge, other.id) == [(pr, [[pa], []])] and engine_tree(bridge, track.id) == [pb]
    editor.remove_device(other.id, rack.id)
    assert engine_tree(bridge, other.id) == [] and bridge.engine_device_id(other.id, a.id) is None
    stack.undo()
    pr, pa = bridge.engine_device_id(other.id, rack.id), bridge.engine_device_id(other.id, a.id)
    assert engine_tree(bridge, other.id) == [(pr, [[pa], []])]


def test_automation_of_nested_devices_and_chain_faders(bridged, make_wav):
    editor, bridge, _engine = bridged
    track = noise_track(editor, bridge, make_wav)
    (a,) = utilities(editor, track, 1)
    rack = editor.group_devices(track.id, [a.id])
    chain = rack.chains[0]
    key = automation.chain_key(rack.id, chain.id, automation.CHAIN_VOLUME)
    assert bridge.can_automate(track.id, key) and bridge.can_automate(track.id, automation.device_key(a.id, "gain"))
    assert bridge.param_spec(track.id, key).name == f"{chain.name} Volume"
    assert bridge.own_value(track.id, key) == 0.0
    editor.set_envelope(track.id, key, [AutomationPoint(0.0, 0.0)])  # silent
    assert bridge.is_automated(track.id, key)
    np.testing.assert_array_equal(render(bridge), 0.0)
    editor.set_chain_param(track.id, chain.id, "volume_db", -6.0)  # by hand: its automation stops
    assert bridge.is_overridden(track.id, key)
    assert np.abs(render(bridge)).max() > 0.1
    bridge.re_enable_automation(track.id)
    np.testing.assert_array_equal(render(bridge), 0.0)
    editor.clear_envelope(track.id, key)
    gain = automation.device_key(a.id, "gain")
    editor.set_envelope(track.id, gain, [AutomationPoint(0.0, 0.0)])  # -60 dB
    assert np.abs(render(bridge)[SAMPLE_RATE // 20:]).max() < 0.01  # (after the utility's gain glides there)


def test_macros_reach_the_engine(bridged, make_wav, uids_effect):
    editor, bridge, engine = bridged
    track = noise_track(editor, bridge, make_wav)
    fx = editor.add_device(track.id, PLUGIN_KIND, plugin=uids_effect)
    rack = editor.group_devices(track.id, [fx.id])
    pid = bridge.engine_device_id(track.id, fx.id)
    gain = engine.processor_params(pid)[0]  # GIL Test Effect's gain (0..1: 0..2 times)
    editor.map_macro(track.id, rack.id, 0, fx.id, gain.id)
    editor.set_macro(track.id, rack.id, 0, 0.25)
    assert engine.processor_param(pid, 0) == pytest.approx(gain.from_normalized(0.25))
    editor.undo_stack.undo()
    assert engine.processor_param(pid, 0) == pytest.approx(0.5)  # as it was (the plug-in's default)


@pytest.fixture
def uids_effect():
    if not TEST_PLUGINS.exists():
        pytest.skip("test plug-ins not built")
    uid = next(d.uid for d in ge.scan_vst3(str(TEST_PLUGINS)) if d.name == "GIL Test Effect")
    return PluginRef(format="VST3", uid=uid, name="GIL Test Effect", path=str(TEST_PLUGINS))


def test_a_saved_rack_loads_and_renders_the_same(bridged, make_wav, uids_effect, tmp_path):
    """A preset of a rack: plug-in state, macros and all; loaded twice, two new racks."""
    editor, bridge, engine = bridged
    p = editor.project
    track = noise_track(editor, bridge, make_wav)
    fx = editor.add_device(track.id, PLUGIN_KIND, plugin=uids_effect)
    engine.set_processor_param(bridge.engine_device_id(track.id, fx.id), 0, 0.3)  # in the plug-in only
    (gain,) = utilities(editor, track, 1)
    rack = editor.group_devices(track.id, [fx.id])
    wet = editor.add_rack_chain(track.id, rack.id)
    editor.move_devices(track.id, [gain.id], 0, wet.id)
    editor.set_chain_param(track.id, wet.id, "pan", -0.5)
    editor.map_macro(track.id, rack.id, 0, gain.id, "gain")
    editor.set_macro(track.id, rack.id, 0, 0.6)
    original = render(bridge)
    bridge.store_plugin_states()  # (as saving does)
    path = tmp_path / "rack.gilpreset"
    save_preset(p.device(track.id, rack.id), path)
    loaded = []
    for _ in range(2):
        copy_track = noise_track(editor, bridge, make_wav)
        device = load_preset(path)
        assert editor.insert_device(copy_track.id, device)
        loaded.append((copy_track, device))
        for t in [track, *(t for t, _ in loaded)]:
            editor.set_track_param(t.id, "mute", t is not copy_track)
        np.testing.assert_allclose(render(bridge), original, atol=1e-6)  # alone, it sounds as the original
    (_t1, r1), (t2, r2) = loaded
    assert r1.id != r2.id and r1.id != rack.id
    # Its macros move its own devices.
    inner_gain = p.chain(t2.id, r2.chains[1].id).devices[0]
    editor.set_macro(t2.id, r2.id, 0, 0.0)
    assert p.device(t2.id, inner_gain.id).params["gain"] == -60.0
    assert p.device(track.id, gain.id).params["gain"] == pytest.approx(-60 + 0.6 * 84)


def test_ungrouping_layered_instruments_is_refused(editor):
    p = editor.project
    keys = editor.add_midi_track()
    rack = editor.group_devices(keys.id, [keys.devices[0].id])
    second = editor.add_rack_chain(keys.id, rack.id)
    editor.add_device(keys.id, "synth", chain=second.id)  # two instruments: a chain has one
    steps = editor.undo_stack.count()
    assert editor.ungroup_rack(keys.id, rack.id) is False
    assert editor.undo_stack.count() == steps and p.device(keys.id, rack.id).is_rack


def test_a_device_leaving_its_rack_leaves_its_macro_mapping(editor):
    p = editor.project
    track, other = editor.add_audio_track(), editor.add_audio_track()
    a, b, _c = utilities(editor, track, 3)
    rack = editor.group_devices(track.id, [a.id, b.id])
    editor.map_macro(track.id, rack.id, 0, a.id, "gain")
    editor.map_macro(track.id, rack.id, 1, b.id, "gain")
    assert editor.move_devices(track.id, [a.id], 0)  # out of the rack, onto the track's own chain
    assert p.device(track.id, rack.id).macros == (MacroMapping(1, b.id, "gain"),)
    editor.undo_stack.undo()
    assert len(p.device(track.id, rack.id).macros) == 2
    assert editor.move_devices_to_track(track.id, [b.id], other.id)  # to another track
    assert p.device(track.id, rack.id).macros == (MacroMapping(0, a.id, "gain"),)


@pytest.mark.parametrize("damage", [
    {"version": "x"},
    {"device": {"kind": "utility", "params": []}},
    {"device": {"kind": "rack", "macros": ["x"], "chains": []}},
])
def test_damaged_presets_are_project_file_errors(editor, damage):
    track = editor.add_audio_track()
    a, b = utilities(editor, track, 2)
    rack = editor.group_devices(track.id, [a.id, b.id])
    with pytest.raises(ProjectFileError):
        preset_device({**device_to_preset(rack), **damage})


def test_presets_nesting_too_deep_are_refused(editor):
    track = editor.add_audio_track()
    device = editor.add_device(track.id, "utility")
    for _ in range(MAX_RACK_DEPTH):
        device = editor.group_devices(track.id, [device.id])
    assert device is not None
    data = device_to_preset(device)
    wrapper = copy.deepcopy(data["device"])  # one more rack around the deepest allowed
    wrapper["chains"][0]["devices"] = [data["device"]]
    with pytest.raises(ProjectFileError, match="too deep"):
        preset_device({**data, "device": wrapper})
