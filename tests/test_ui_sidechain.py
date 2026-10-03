"""Sidechains in the window: a device with a sidechain input has a sidechain
button in its title bar, lit while it has one; its menu lists the tracks,
groups and returns it can come from (greyed out where that would close a
cycle) and where it is taken, along the source's signal as in Ableton: Pre FX
(before its devices; after a MIDI track's instrument), after one of its
devices, Post FX (before its fader) or Post Mixer (after it)."""

import pytest

from substation.model.automation import MASTER
from substation.model.project import (
    PLUGIN_KIND,
    POST_FADER,
    PRE_FADER,
    PRE_FX,
    Sidechain,
)

from .conftest import TEST_PLUGINS
from .test_ui_plugins import installed

pytestmark = pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")


def actions(menu) -> dict:
    return {a.text(): a for a in menu.actions() if a.text()}


def test_the_sidechain_button(window, app):
    editor, p = window.editor, window.project
    refs = installed(window)
    kick = editor.add_audio_track(name="Kick")
    eq = editor.add_device(kick.id, "utility")
    bass = editor.add_audio_track(name="Bass")
    group = editor.group_tracks([bass.id])
    ret = editor.add_return_track()
    window.selection.select_track(bass.id)
    keyed = editor.add_device(bass.id, PLUGIN_KIND, plugin=refs["SUB Test Sidechain"])
    plain = editor.add_device(bass.id, PLUGIN_KIND, plugin=refs["SUB Test Effect"])
    app.processEvents()
    widget = window.devices.widgets[keyed.id]
    assert window.devices.widgets[plain.id].sidechain is None  # no sidechain input: no button
    button = widget.sidechain
    assert button.isVisible() and not button.isChecked() and "none" in button.toolTip()

    menu = widget.sidechain_menu()
    choices = actions(menu)
    assert list(choices) == ["No Sidechain", "Kick", f"{group.name} (this track feeds it)", ret.name]
    assert choices["No Sidechain"].isChecked() and not choices[f"{group.name} (this track feeds it)"].isEnabled()
    choices["Kick"].trigger()
    assert p.device(bass.id, keyed.id).sidechain == Sidechain(kick.id, POST_FADER)
    assert button.isChecked() and button.toolTip() == "Sidechain: Kick, Post Mixer"

    choices = actions(widget.sidechain_menu())  # now with the taps
    assert list(choices)[-4:] == ["Pre FX", "After Utility", "Post FX", "Post Mixer"]
    assert choices["Kick"].isChecked() and choices["Post Mixer"].isChecked()
    choices["Pre FX"].trigger()
    assert p.device(bass.id, keyed.id).sidechain == Sidechain(kick.id, PRE_FX)
    assert button.toolTip() == "Sidechain: Kick, Pre FX"
    assert actions(widget.sidechain_menu())["Pre FX"].isChecked()
    actions(widget.sidechain_menu())["After Utility"].trigger()
    assert p.device(bass.id, keyed.id).sidechain == Sidechain(kick.id, eq.id)
    assert button.toolTip() == "Sidechain: Kick, After Utility"
    editor.remove_device(kick.id, eq.id)  # before the fader, until it comes back
    app.processEvents()
    assert actions(widget.sidechain_menu())["Post FX"].isChecked()
    assert button.toolTip() == "Sidechain: Kick, Post FX"  # (the kick's chain changed, not the bass's)
    window.undo_stack.undo()
    app.processEvents()
    assert button.toolTip() == "Sidechain: Kick, After Utility"
    actions(widget.sidechain_menu())["Post FX"].trigger()
    assert p.device(bass.id, keyed.id).sidechain == Sidechain(kick.id, PRE_FADER)
    actions(widget.sidechain_menu())[ret.name].trigger()  # another source: taken in the same place
    assert p.device(bass.id, keyed.id).sidechain == Sidechain(ret.id, PRE_FADER)

    editor.rename_track(ret.id, "Verb")
    app.processEvents()
    assert button.toolTip() == "Sidechain: Verb, Post FX"  # it follows its source's name
    # The sidechain is a routing edge: the bass can't send to the return keying it.
    assert ret not in p.send_targets(bass.id)
    actions(widget.sidechain_menu())["No Sidechain"].trigger()
    assert p.device(bass.id, keyed.id).sidechain is None and not button.isChecked()
    assert ret in p.send_targets(bass.id)

    # Deleting the source turns it off.
    actions(widget.sidechain_menu())["Kick"].trigger()
    editor.delete_tracks([kick.id])
    app.processEvents()
    assert not button.isChecked() and p.device(bass.id, keyed.id).sidechain is None
    window.undo_stack.undo()
    app.processEvents()
    assert button.isChecked()

    # A MIDI track's own audio is its instrument's: Pre FX is after it, so it isn't listed.
    keys = editor.add_midi_track(name="Keys")
    actions(widget.sidechain_menu())["Keys"].trigger()
    assert list(actions(widget.sidechain_menu()))[-3:] == ["Pre FX", "Post FX", "Post Mixer"]
    editor.set_device_sidechain(bass.id, keyed.id, Sidechain(keys.id, keys.devices[0].id))
    app.processEvents()
    assert actions(widget.sidechain_menu())["Pre FX"].isChecked() and button.toolTip() == "Sidechain: Keys, Pre FX"


def test_the_masters_devices_take_any_track(window, app):
    editor = window.editor
    track = editor.add_audio_track(name="Kick")
    window.selection.select_track(MASTER)
    keyed = editor.add_device(MASTER, PLUGIN_KIND, plugin=installed(window)["SUB Test Sidechain"])
    app.processEvents()
    choices = actions(window.devices.widgets[keyed.id].sidechain_menu())
    assert list(choices) == ["No Sidechain", "Kick"] and choices["Kick"].isEnabled()
    choices["Kick"].trigger()
    assert window.project.master.devices[0].sidechain == Sidechain(track.id)
