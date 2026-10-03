"""Device presets: the user's library (saving by name, listing by device,
renaming), loading a preset into a device of its kind (one undo step), and
through the engine: plug-in presets render as the device they were saved from,
and a preset of a plug-in (or a built-in device) that isn't there loads as a
missing device, also inside a rack, without failing the rest."""

import numpy as np
import pytest
from PySide6.QtGui import QUndoStack

from substation import _engine as ge
from substation.model import automation
from substation.model.automation import AutomationPoint
from substation.model.devices import device_name, loads_into, new_device
from substation.model.editor import ProjectEditor
from substation.model.presets import (
    DEFAULTS,
    clear_default,
    default_device,
    default_path,
    file_name,
    has_default,
    list_presets,
    preset_path,
    rename_preset,
    save_default,
    save_to_library,
)
from substation.model.project import PLUGIN_KIND, Device, PluginRef, Project
from substation.model.serialization import (
    PRESET_EXTENSION,
    device_to_preset,
    load_into,
    load_preset,
    preset_device,
    project_to_dict,
)

from .conftest import TEST_PLUGINS
from .test_racks_model import noise_track, render


@pytest.fixture
def editor(app):
    return ProjectEditor(Project(), QUndoStack())


@pytest.fixture
def bridged(app):
    from substation.audio.engine_bridge import EngineBridge

    engine = ge.Engine()
    engine.set_clip_fade_ms(0)
    project = Project()
    editor = ProjectEditor(project, QUndoStack())
    bridge = EngineBridge(engine, project)
    yield editor, bridge, engine
    bridge.shutdown()
    engine.close_device()


@pytest.fixture
def uids_effect():
    if not TEST_PLUGINS.exists():
        pytest.skip("test plug-ins not built")
    uid = next(d.uid for d in ge.scan_vst3(str(TEST_PLUGINS)) if d.name == "SUB Test Effect")
    return PluginRef(format="VST3", uid=uid, name="SUB Test Effect", path=str(TEST_PLUGINS))


# --- The library -------------------------------------------------------------------------


def test_presets_are_saved_by_device_and_listed_by_group_then_name(editor, tmp_path):
    track = editor.add_audio_track()
    utility = editor.add_device(track.id, "utility")
    compressor = editor.add_device(track.id, "compressor")
    rack = editor.group_devices(track.id, [compressor.id])
    paths = [save_to_library(d, name, tmp_path) for d, name in
             ((utility, "quiet"), (utility, "Loud"), (rack, "Glue"))]
    assert paths[0] == tmp_path / "Utility" / f"quiet{PRESET_EXTENSION}"
    assert paths[2] == tmp_path / "Audio Effect Rack" / f"Glue{PRESET_EXTENSION}"
    (tmp_path / f"Loose{PRESET_EXTENSION}").write_bytes(paths[0].read_bytes())  # put there by hand
    (tmp_path / "Utility" / "notes.txt").write_text("not a preset")
    listed = [(p.group, p.name) for p in list_presets(tmp_path)]
    assert listed == [("", "Loose"), ("Audio Effect Rack", "Glue"), ("Utility", "Loud"), ("Utility", "quiet")]
    assert list_presets(tmp_path / "nothing here") == []
    # Saving under a name used replaces that preset.
    editor.set_device_param(track.id, utility.id, "gain", -12.0)
    save_to_library(editor.project.device(track.id, utility.id), "Loud", tmp_path)
    assert load_preset(paths[1]).params["gain"] == -12.0
    assert len(list_presets(tmp_path)) == 4


def test_preset_names_become_file_names(editor, tmp_path):
    assert file_name('  Lead: "Bright" / Wide?  ') == "Lead_ _Bright_ _ Wide_"
    for bad in ("", "   ", "...", "CON", "nul.txt"):
        with pytest.raises(ValueError):
            file_name(bad)
    device = Device(id="d", kind="utility")
    assert preset_path(device, "a/b", tmp_path) == tmp_path / "Utility" / f"a_b{PRESET_EXTENSION}"


def test_renaming_a_preset(editor, tmp_path):
    utility = Device(id="d", kind="utility", params={"gain": -3.0})
    first = save_to_library(utility, "One", tmp_path)
    second = save_to_library(utility, "Two", tmp_path)
    with pytest.raises(FileExistsError):
        rename_preset(first, "Two")
    renamed = rename_preset(first, "Three")
    assert renamed == tmp_path / "Utility" / f"Three{PRESET_EXTENSION}" and not first.exists()
    assert rename_preset(second, "two").name == f"two{PRESET_EXTENSION}"  # a change of case is no clash
    assert [p.name for p in list_presets(tmp_path)] == ["Three", "two"]


# --- Loading into a device -------------------------------------------------------------------


def preset_of(device: Device) -> Device:
    """The device as a preset would load (a new device: fresh ids)."""
    return preset_device(device_to_preset(device))


def test_a_preset_loads_into_a_built_in_device_in_one_undo_step(editor):
    p = editor.project
    track = editor.add_audio_track()
    source, target = editor.add_device(track.id, "utility"), editor.add_device(track.id, "utility")
    editor.set_device_param(track.id, source.id, "gain", -9.0)
    editor.set_device_param(track.id, source.id, "pan", 0.5)
    editor.set_device_enabled(track.id, target.id, False)
    preset = preset_of(p.device(track.id, source.id))
    before = dict(p.device(track.id, target.id).params)
    count = editor.undo_stack.count()
    assert editor.load_preset_into(track.id, target.id, preset, "Load Preset Wide")
    loaded = p.device(track.id, target.id)
    assert loaded.params == p.device(track.id, source.id).params
    assert not loaded.enabled and loaded.id == target.id  # the device stays, with its switch
    assert editor.undo_stack.count() == count + 1 and editor.undo_stack.undoText() == "Load Preset Wide"
    editor.undo_stack.undo()
    assert p.device(track.id, target.id).params == before
    editor.undo_stack.redo()
    assert p.device(track.id, target.id).params["gain"] == -9.0


def test_a_preset_loads_only_into_a_device_of_its_kind(editor):
    p = editor.project
    track = editor.add_audio_track()
    utility, compressor = editor.add_device(track.id, "utility"), editor.add_device(track.id, "compressor")
    count = editor.undo_stack.count()
    assert not editor.load_preset_into(track.id, compressor.id, preset_of(p.device(track.id, utility.id)))
    assert editor.undo_stack.count() == count
    synth = PluginRef(format="VST3", uid="A", name="Synth A", instrument=True)
    effect = PluginRef(format="VST3", uid="B", name="Effect B")
    assert loads_into(Device(id="1", kind=PLUGIN_KIND, plugin=effect), Device(id="2", kind=PLUGIN_KIND, plugin=effect))
    assert not loads_into(Device(id="1", kind=PLUGIN_KIND, plugin=effect),
                          Device(id="2", kind=PLUGIN_KIND, plugin=synth))
    keys = editor.add_midi_track()
    instrument_rack = editor.group_devices(keys.id, [keys.devices[0].id])
    effect_rack = editor.group_devices(track.id, [utility.id])
    count = editor.undo_stack.count()
    assert not editor.load_preset_into(track.id, effect_rack.id, preset_of(p.device(keys.id, instrument_rack.id)))
    assert editor.undo_stack.count() == count


def test_a_rack_preset_loads_into_a_rack(editor):
    """Its chains (new devices) and macros replace the rack's; the rack stays,
    and the automation of the devices that went goes with them, in the same step."""
    p = editor.project
    track = editor.add_audio_track()
    a, b = editor.add_device(track.id, "utility"), editor.add_device(track.id, "utility")
    source = editor.group_devices(track.id, [a.id])
    editor.map_macro(track.id, source.id, 0, a.id, "gain")
    editor.set_macro(track.id, source.id, 0, 0.25)
    target = editor.group_devices(track.id, [b.id])
    key = automation.device_key(b.id, "gain")
    editor.set_envelope(track.id, key, (AutomationPoint(0.0, -6.0), AutomationPoint(4.0, 0.0)))
    before = p.device(track.id, target.id)
    count = editor.undo_stack.count()
    assert editor.load_preset_into(track.id, target.id, preset_of(p.device(track.id, source.id)))
    rack = p.device(track.id, target.id)
    (inner,) = rack.chains[0].devices
    assert inner.id not in (a.id, b.id) and inner.params == p.device(track.id, a.id).params
    assert [(m.macro, m.device_id) for m in rack.macros] == [(0, inner.id)]
    assert rack.params == p.device(track.id, source.id).params
    assert not p.envelope(track.id, key)
    assert editor.undo_stack.count() == count + 1
    editor.undo_stack.undo()
    assert p.device(track.id, target.id) == before and p.envelope(track.id, key)


# --- Through the engine ------------------------------------------------------------------


def settled(bridge):
    """A render once parameters have got where they were set (they glide there on the first)."""
    render(bridge)
    return render(bridge)


def solo(editor, keep, tracks):
    for t in tracks:
        editor.set_track_param(t.id, "mute", t is not keep)


def test_a_plug_in_preset_renders_as_the_device_it_was_saved_from(bridged, make_wav, uids_effect, tmp_path):
    editor, bridge, engine = bridged
    p = editor.project
    original = noise_track(editor, bridge, make_wav)
    fx = editor.add_device(original.id, PLUGIN_KIND, plugin=uids_effect)
    engine.set_processor_param(bridge.engine_device_id(original.id, fx.id), 0, 0.3)  # in the plug-in only
    expected = render(bridge)
    bridge.store_plugin_states()  # (as the save button does)
    path = save_to_library(p.device(original.id, fx.id), "Quiet", tmp_path)

    # As a new device...
    added = noise_track(editor, bridge, make_wav)
    device = load_preset(path)
    assert editor.insert_device(added.id, device)
    assert device.id != fx.id
    assert engine.processor_param(bridge.engine_device_id(added.id, device.id), 0) == pytest.approx(0.3)
    solo(editor, added, p.tracks)
    np.testing.assert_allclose(render(bridge), expected, atol=1e-6)

    # ...and into a device of the same plug-in, undoably.
    into = noise_track(editor, bridge, make_wav)
    target = editor.add_device(into.id, PLUGIN_KIND, plugin=uids_effect)
    pid = bridge.engine_device_id(into.id, target.id)
    solo(editor, into, p.tracks)
    untouched = render(bridge)
    bridge.store_plugin_states({target.id})
    assert editor.load_preset_into(into.id, target.id, load_preset(path), "Load Preset Quiet")
    assert bridge.engine_device_id(into.id, target.id) == pid  # the same plug-in, with the preset's state
    assert engine.processor_param(pid, 0) == pytest.approx(0.3)
    np.testing.assert_allclose(render(bridge), expected, atol=1e-6)
    editor.undo_stack.undo()
    assert engine.processor_param(pid, 0) == pytest.approx(0.5)
    np.testing.assert_allclose(render(bridge), untouched, atol=1e-6)
    editor.undo_stack.redo()
    assert engine.processor_param(pid, 0) == pytest.approx(0.3)


def test_a_built_in_preset_loaded_into_a_device_reaches_the_engine(bridged, make_wav, tmp_path):
    editor, bridge, _engine = bridged
    p = editor.project
    track = noise_track(editor, bridge, make_wav)
    source = editor.add_device(track.id, "utility")
    editor.set_device_param(track.id, source.id, "gain", -20.0)
    expected = settled(bridge)
    path = save_to_library(p.device(track.id, source.id), "Down", tmp_path)
    editor.remove_device(track.id, source.id)
    target = editor.add_device(track.id, "utility")
    untouched = settled(bridge)
    assert editor.load_preset_into(track.id, target.id, load_preset(path))
    np.testing.assert_allclose(settled(bridge), expected, atol=1e-6)
    editor.undo_stack.undo()
    np.testing.assert_allclose(settled(bridge), untouched, atol=1e-6)


def test_a_preset_of_a_missing_plug_in_loads_as_a_missing_device(bridged, make_wav, uids_effect):
    """Kept, and marked missing (as when loading a project); in a rack, the rest of the rack works."""
    editor, bridge, _engine = bridged
    p = editor.project
    track = noise_track(editor, bridge, make_wav)
    gone = PluginRef(format="VST3", uid="0" * 32, name="Long Gone", vendor="Nobody", path=r"C:\nowhere\Gone.vst3")
    missing = Device(id="m", kind=PLUGIN_KIND, plugin=gone, state="AAAA")
    device = preset_device(device_to_preset(missing))
    assert editor.insert_device(track.id, device)
    assert p.device(track.id, device.id).plugin == gone  # kept, as it was
    assert bridge.engine_device_id(track.id, device.id) is None
    assert "Long Gone is not installed" in bridge.plugin_errors[device.id]

    gain = editor.add_device(track.id, "utility")
    editor.set_device_param(track.id, gain.id, "gain", -20.0)
    unknown = editor.add_device(track.id, "utility")
    rack = editor.group_devices(track.id, [gain.id, unknown.id])
    data = device_to_preset(p.device(track.id, rack.id))
    data["device"]["chains"][0]["devices"].insert(1, device_to_preset(missing)["device"])
    data["device"]["chains"][0]["devices"][2]["kind"] = "a device of a later version"
    editor.remove_devices(track.id, [rack.id, device.id])
    expected_quiet = settled(bridge) * 0.1  # -20 dB
    loaded = preset_device(data)
    assert editor.insert_device(track.id, loaded)
    inner_gain, inner_missing, inner_unknown = loaded.chains[0].devices
    assert bridge.engine_device_id(track.id, loaded.id) is not None
    assert bridge.engine_device_id(track.id, inner_gain.id) is not None
    assert bridge.engine_device_id(track.id, inner_missing.id) is None
    assert bridge.engine_device_id(track.id, inner_unknown.id) is None
    assert "not a device this version" in bridge.plugin_errors[inner_unknown.id]
    np.testing.assert_allclose(settled(bridge), expected_quiet, atol=1e-4)


# --- Default presets ---------------------------------------------------------------------


@pytest.fixture
def with_defaults(editor, tmp_path):
    """The editor making new devices from the default presets in tmp_path."""
    editor.set_device_defaults(lambda kind, plugin: default_device(kind, plugin, tmp_path))
    return editor


def test_new_devices_start_as_their_default_preset(with_defaults, tmp_path):
    editor = with_defaults
    p = editor.project
    track = editor.add_audio_track()
    factory = editor.add_device(track.id, "utility")
    editor.set_device_param(track.id, factory.id, "gain", -4.0)
    path = save_default(p.device(track.id, factory.id), tmp_path)
    assert path == tmp_path / DEFAULTS / f"Utility{PRESET_EXTENSION}" and has_default("utility", root=tmp_path)
    assert list_presets(tmp_path) == []  # (the browser doesn't list defaults)

    first, second = editor.add_device(track.id, "utility"), editor.add_device(track.id, "utility")
    assert first.params == second.params == p.device(track.id, factory.id).params
    assert len({factory.id, first.id, second.id}) == 3
    assert editor.add_device(track.id, "compressor").params == new_device("compressor").params  # (no default)
    # A new MIDI track's instrument starts as its default too.
    keys = editor.add_midi_track()
    editor.set_device_param(keys.id, keys.devices[0].id, "volume", -9.0)
    save_default(p.track(keys.id).devices[0], tmp_path)
    assert editor.add_midi_track().devices[0].params["volume"] == -9.0

    assert clear_default("utility", root=tmp_path) and not clear_default("utility", root=tmp_path)
    assert editor.add_device(track.id, "utility").params == new_device("utility").params


def test_a_default_preset_is_per_plug_in_and_keeps_where_the_plug_in_is(with_defaults, tmp_path):
    editor = with_defaults
    a = PluginRef(format="VST3", uid="AAAA", name="Same Name", path=r"C:\old\A.vst3")
    b = PluginRef(format="VST3", uid="BBBB", name="Same Name")
    save_default(Device(id="d", kind=PLUGIN_KIND, plugin=a, state="c3RhdGU="), tmp_path)
    assert default_path(PLUGIN_KIND, a, tmp_path).name == f"Same Name (AAAA){PRESET_EXTENSION}"
    track = editor.add_audio_track()
    moved = PluginRef(format="VST3", uid="AAAA", name="Same Name", path=r"D:\new\A.vst3")
    device = editor.add_device(track.id, PLUGIN_KIND, plugin=moved)
    assert device.state == "c3RhdGU=" and device.plugin == moved
    assert editor.add_device(track.id, PLUGIN_KIND, plugin=b).state is None
    assert default_path("rack", root=tmp_path) is None
    with pytest.raises(ValueError):
        save_default(Device(id="r", kind="rack"), tmp_path)


def test_an_unreadable_default_preset_is_ignored(with_defaults, tmp_path):
    editor = with_defaults
    path = default_path("utility", root=tmp_path)
    path.parent.mkdir(parents=True)
    path.write_text("not a preset")
    track = editor.add_audio_track()
    assert editor.add_device(track.id, "utility").params == new_device("utility").params
    save_default(Device(id="c", kind="compressor"), tmp_path)
    default_path("compressor", root=tmp_path).replace(path)  # a compressor where the utility's goes
    assert editor.add_device(track.id, "utility").params == new_device("utility").params


def test_a_new_plug_in_starts_with_its_default_state(bridged, uids_effect, tmp_path):
    editor, bridge, engine = bridged
    editor.set_device_defaults(lambda kind, plugin: default_device(kind, plugin, tmp_path))
    track = editor.add_audio_track()
    fx = editor.add_device(track.id, PLUGIN_KIND, plugin=uids_effect)
    engine.set_processor_param(bridge.engine_device_id(track.id, fx.id), 0, 0.3)
    bridge.store_plugin_states({fx.id})
    save_default(editor.project.device(track.id, fx.id), tmp_path)
    new = editor.add_device(track.id, PLUGIN_KIND, plugin=uids_effect)
    assert engine.processor_param(bridge.engine_device_id(track.id, new.id), 0) == pytest.approx(0.3)


# --- Rack names ----------------------------------------------------------------------------


def test_a_rack_is_named_as_the_preset_it_comes_from(editor, tmp_path):
    p = editor.project
    track = editor.add_audio_track()
    rack = editor.group_devices(track.id, [editor.add_device(track.id, "utility").id])
    assert device_name(rack) == "Audio Effect Rack"
    path = save_to_library(p.device(track.id, rack.id), "Glue", tmp_path)
    assert path.parent.name == "Audio Effect Rack"  # (grouped by kind, whatever its name)
    loaded = load_preset(path)
    assert loaded.name == "Glue" and device_name(loaded) == "Glue"
    path = rename_preset(path, "Bus Glue")
    assert load_preset(path).name == "Bus Glue"  # (the file's name, as renamed)
    # Loaded into a rack, the rack takes the name; saved as a preset, the name is kept in the file and projects.
    assert editor.load_preset_into(track.id, rack.id, load_preset(path))
    assert p.device(track.id, rack.id).name == "Bus Glue"
    editor.undo_stack.undo()
    assert p.device(track.id, rack.id).name is None
    editor.rename_rack(track.id, rack.id, "Mine")
    assert device_name(p.device(track.id, rack.id)) == "Mine"
    assert save_to_library(p.device(track.id, rack.id), "Other", tmp_path).parent.name == "Audio Effect Rack"
    copy = Project()
    load_into(copy, project_to_dict(p))
    assert copy.device(track.id, rack.id).name == "Mine"
    data = project_to_dict(p)
    del data["tracks"][0]["devices"][0]["name"]
    data["version"] = 12  # (from before rack names)
    load_into(copy, data)
    assert copy.device(track.id, rack.id).name is None
    editor.rename_rack(track.id, rack.id, "")
    assert device_name(p.device(track.id, rack.id)) == "Audio Effect Rack"
