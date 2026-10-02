"""Sidechains in the window: a device with a sidechain input has a sidechain
button in its title bar, lit while it has one; its menu lists the tracks,
groups and returns it can come from (greyed out where that would close a
cycle) and where it is taken: after the fader, before it, or after one of the
source's devices."""

import pytest

from gilstudio.model.automation import MASTER
from gilstudio.model.project import PLUGIN_KIND, POST_FADER, PRE_FADER, Sidechain

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
    keyed = editor.add_device(bass.id, PLUGIN_KIND, plugin=refs["GIL Test Sidechain"])
    plain = editor.add_device(bass.id, PLUGIN_KIND, plugin=refs["GIL Test Effect"])
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
    assert button.isChecked() and button.toolTip() == "Sidechain: Kick, post fader"

    choices = actions(widget.sidechain_menu())  # now with the taps
    assert list(choices)[-3:] == ["Post Fader", "Pre Fader", "After Utility"]
    assert choices["Kick"].isChecked() and choices["Post Fader"].isChecked()
    choices["After Utility"].trigger()
    assert p.device(bass.id, keyed.id).sidechain == Sidechain(kick.id, eq.id)
    assert button.toolTip() == "Sidechain: Kick, After Utility"
    editor.remove_device(kick.id, eq.id)  # before the fader, until it comes back
    app.processEvents()
    assert actions(widget.sidechain_menu())["Pre Fader"].isChecked()
    window.undo_stack.undo()
    actions(widget.sidechain_menu())["Pre Fader"].trigger()
    assert p.device(bass.id, keyed.id).sidechain == Sidechain(kick.id, PRE_FADER)
    actions(widget.sidechain_menu())[ret.name].trigger()  # another source: taken in the same place
    assert p.device(bass.id, keyed.id).sidechain == Sidechain(ret.id, PRE_FADER)

    editor.rename_track(ret.id, "Verb")
    app.processEvents()
    assert button.toolTip() == "Sidechain: Verb, pre fader"  # it follows its source's name
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


def test_the_masters_devices_take_any_track(window, app):
    editor = window.editor
    track = editor.add_audio_track(name="Kick")
    window.selection.select_track(MASTER)
    keyed = editor.add_device(MASTER, PLUGIN_KIND, plugin=installed(window)["GIL Test Sidechain"])
    app.processEvents()
    choices = actions(window.devices.widgets[keyed.id].sidechain_menu())
    assert list(choices) == ["No Sidechain", "Kick"] and choices["Kick"].isEnabled()
    choices["Kick"].trigger()
    assert window.project.master.devices[0].sidechain == Sidechain(track.id)
