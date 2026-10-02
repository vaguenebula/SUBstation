"""VST3 hosting in the engine, with the test plug-ins built alongside it
(tests/vst3_plugins): instruments and effects in the chain, parameters,
transport, latency compensation, state, and plug-in editors.

Rendered offline, so no audio device is needed. The editor tests open real
(briefly visible) Win32 windows and talk to the test plug-in's view with
window messages, as a user's clicks would reach it."""

import ctypes
from ctypes import wintypes

import numpy as np
import pytest

from gilstudio import _engine as ge

from .conftest import SAMPLE_RATE, TEST_PLUGINS

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM
PLUGINS = str(TEST_PLUGINS)
GAIN, WAVE, TEMPO, PLAYING, BEAT, LOOP, MACRO = range(7)  # GIL Test Synth's parameters (then Macros 2..10)
FX_GAIN, FX_LATENCY, FX_BYPASS = range(3)  # GIL Test Effect's
EDIT_GAIN, RESIZE, DIRTY = 0x401, 0x402, 0x403  # messages the effect's editor understands (WM_USER + n)

pytestmark = pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")

user32 = ctypes.windll.user32
user32.FindWindowW.restype = wintypes.HWND
user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
user32.GetWindow.restype = wintypes.HWND
user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                wintypes.UINT]


def client_size(hwnd) -> tuple[int, int]:
    rect = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    return rect.right - rect.left, rect.bottom - rect.top


def editor_window(title: str):
    return user32.FindWindowW("GILStudioPluginEditor", title)


@pytest.fixture(scope="module")
def uids():
    return {d.name: d.uid for d in ge.scan_vst3(PLUGINS)}


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


def plugin_track(engine, uids, name, notes=()):
    track = engine.add_track()
    processor = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids[name])
    if notes:
        engine.set_track_notes(track, [ge.NoteDesc(*n) for n in notes])
    return track, processor


def clip_track(engine, path, start_beat=0.0, duration_sec=1.0):
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, start_beat, duration_sec)])
    return track


def events_of(engine, kind):
    return [e for e in engine.take_processor_events() if e.type == kind]


def test_scan_lists_the_classes_of_a_module():
    found = {d.name: d for d in ge.scan_vst3(PLUGINS)}
    assert sorted(found) == ["GIL Test Effect", "GIL Test Mono", "GIL Test Sidechain", "GIL Test Synth"]  # not the controller class
    synth = found["GIL Test Synth"]
    assert synth.is_instrument and synth.category == "Instrument|Synth" and synth.vendor == "GIL Studio"
    assert not found["GIL Test Effect"].is_instrument and found["GIL Test Effect"].category == "Fx|Delay"
    assert len(synth.uid) == 32 and synth.format == "VST3" and synth.path == PLUGINS
    with pytest.raises(RuntimeError):
        ge.scan_vst3(str(TEST_PLUGINS.parent / "Missing.vst3"))


def test_an_instrument_plays_its_notes_sample_exactly(engine, uids):
    _, synth = plugin_track(engine, uids, "GIL Test Synth", [(1.0, 2.0, 60, 127)])
    engine.set_processor_param(synth, WAVE, 0)  # DC: velocity / 127 while the note is held
    out = engine.render_offline(0.0, 4 * SPB)
    assert np.all(out[:SPB] == 0.0)
    np.testing.assert_allclose(out[SPB : 3 * SPB], 1.0)
    assert np.all(out[3 * SPB :] == 0.0)


def test_pitch_velocity_and_chords(engine, uids):
    track, synth = plugin_track(engine, uids, "GIL Test Synth", [(0.0, 2.0, 69, 127)])
    out = engine.render_offline(0.0, SPB)[:, 0]
    spectrum = np.abs(np.fft.rfft(out * np.hanning(len(out))))
    assert np.argmax(spectrum) * SAMPLE_RATE / len(out) == pytest.approx(440.0, abs=2.0)  # A3
    engine.set_processor_param(synth, WAVE, 0)
    engine.set_track_notes(track, [ge.NoteDesc(0.0, 1.0, 60, 64), ge.NoteDesc(0.0, 1.0, 64, 64)])
    assert engine.render_offline(0.0, 100)[50, 0] == pytest.approx(2 * 64 / 127, abs=1e-6)


def test_parameters_as_the_plugin_describes_them(engine, uids):
    _, synth = plugin_track(engine, uids, "GIL Test Synth")
    params = engine.processor_params(synth)
    assert [p.name for p in params] == ["Gain", "Wave", "Tempo", "Playing", "Beat", "Loop"] + [
        f"Macro {i}" for i in range(1, 11)]
    assert [p.id for p in params] == [str(i) for i in range(16)]
    gain, wave = params[GAIN], params[WAVE]
    assert (gain.min_value, gain.max_value, gain.default_value, gain.steps) == (0.0, 1.0, 1.0, 0)
    assert gain.automatable and not gain.read_only and not gain.hidden
    assert wave.value_labels == ["DC", "Sine"] and wave.steps == 1 and wave.max_value == 1.0
    assert [p.read_only for p in params] == [False, False, True, True, True, True] + [False] * 10
    assert params[TEMPO].unit == "BPM"
    assert engine.processor_param_index(synth, "1") == WAVE and engine.processor_param_index(synth, "99") == -1
    # Values are plain: 0..1 when continuous, the step when stepped.
    engine.set_processor_param(synth, GAIN, 0.25)
    engine.set_processor_param(synth, WAVE, 0.0)
    assert engine.processor_param(synth, GAIN) == pytest.approx(0.25)
    assert engine.processor_param(synth, WAVE) == 0.0
    assert engine.processor_param_text(synth, WAVE, 1.0) == "Sine"  # the plug-in's own words
    assert engine.processor_param_text(synth, GAIN, 0.25).startswith("0.25")
    info = engine.processor_info(synth)
    assert info.name == "GIL Test Synth" and info.type_id == "vst3:" + uids["GIL Test Synth"]
    assert info.latency == 0 and info.has_editor  # it has a controller (its editor is asked for on opening)


def test_parameter_changes_reach_the_processor(engine, uids):
    _, synth = plugin_track(engine, uids, "GIL Test Synth", [(0.0, 4.0, 60, 127)])
    engine.set_processor_param(synth, WAVE, 0)
    engine.set_processor_param(synth, GAIN, 0.5)
    assert engine.render_offline(0.0, 100)[50, 0] == pytest.approx(0.5)


def test_transport_reaches_the_plugin(engine, uids):
    _, synth = plugin_track(engine, uids, "GIL Test Synth")
    engine.tempo = 90.0
    engine.render_offline(2.0, 4096)
    engine.idle()  # the plug-in's output parameters reach its controller
    assert engine.processor_param(synth, TEMPO) == pytest.approx(0.09)
    assert engine.processor_param_text(synth, TEMPO, engine.processor_param(synth, TEMPO)).startswith("90")
    assert engine.processor_param(synth, PLAYING) == 1.0
    beat_of_last_block = 2.0 + 3 * 1024 / (SAMPLE_RATE * 60 / 90)
    assert engine.processor_param(synth, BEAT) * 1000 == pytest.approx(beat_of_last_block, abs=1e-3)
    assert engine.processor_param(synth, LOOP) == 0.0
    assert ge.ProcessorEventType.PARAMS_CHANGED in [e.type for e in engine.take_processor_events()]


def test_blocks_are_split_where_the_loop_wraps(engine, uids):
    _, synth = plugin_track(engine, uids, "GIL Test Synth")
    engine.set_loop(True, 1.0, 2.0)
    # The 24th block runs from 47552 to 48576 on the timeline, across the loop end
    # (48000): the plug-in sees it as two blocks, the second starting at beat 1.
    engine.render_offline(1.0, 24 * 1024, loop=True)
    engine.idle()
    assert engine.processor_param(synth, BEAT) * 1000 == pytest.approx(1.0)
    assert engine.processor_param(synth, LOOP) == 1.0


def test_offline_renders_are_repeatable(engine, uids):
    plugin_track(engine, uids, "GIL Test Synth", [(0.0, 16.0, 60, 100)])
    first = engine.render_offline(0.0, 2 * SPB)
    engine.render_offline(0.0, SPB // 2)  # stops in the middle of the note
    assert np.abs(engine.render_offline(4.0, SPB)).max() == 0.0  # no hanging note
    np.testing.assert_array_equal(engine.render_offline(0.0, 2 * SPB), first)


def test_an_effect_processes_the_track(engine, uids, dc_wav):
    track = clip_track(engine, dc_wav)
    effect = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Effect"])
    params = engine.processor_params(effect)
    assert [p.name for p in params] == ["Gain", "Latency", "Bypass"]
    assert params[FX_BYPASS].hidden  # the device's own on/off switch stands for it
    assert params[FX_LATENCY].steps == 4096 and params[FX_LATENCY].value_labels == []
    np.testing.assert_allclose(engine.render_offline(0.0, 1000), 0.5)  # Gain 0.5 is unity
    engine.set_processor_param(effect, FX_GAIN, 0.25)
    np.testing.assert_allclose(engine.render_offline(0.0, 1000), 0.25)
    engine.set_processor_enabled(effect, False)  # switched off: the audio passes by
    np.testing.assert_allclose(engine.render_offline(0.0, 1000), 0.5)


def test_automation_reaches_the_plugin_and_its_controller(engine, uids, dc_wav):
    track = clip_track(engine, dc_wav)
    effect = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.take_processor_events()
    engine.set_track_automation(track, [ge.AutomationLane(effect, str(FX_GAIN), [ge.AutomationPoint(0.0, 0.25)])])
    np.testing.assert_allclose(engine.render_offline(0.0, 1000), 0.25)  # automated, as if set to 0.25
    engine.idle()  # the automated value reaches the controller (the plug-in's editor) and the UI
    assert engine.processor_param(effect, FX_GAIN) == pytest.approx(0.25)
    assert ge.ProcessorEventType.PARAMS_CHANGED in [e.type for e in engine.take_processor_events()]
    engine.set_track_automation(track, [])
    engine.set_processor_param(effect, FX_GAIN, 0.5)
    np.testing.assert_allclose(engine.render_offline(0.0, 1000), 0.5)


def test_a_mono_plugin_on_a_stereo_track(engine, uids, make_wav):
    wav = make_wav(np.tile([0.5, 0.25], (SAMPLE_RATE, 1)))
    track = clip_track(engine, wav)
    mono = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Mono"])
    assert engine.processor_params(mono) == [] and not engine.processor_info(mono).has_editor  # no controller
    out = engine.render_offline(0.0, 1000)
    np.testing.assert_allclose(out, 0.5 * (0.5 + 0.25) / 2, atol=1e-4)  # mixed to mono, halved, on both sides


def test_latency_is_compensated(engine, uids, make_wav):
    click = np.zeros(1000)
    click[0] = 0.5
    wav = make_wav(click)
    late = clip_track(engine, wav, start_beat=1.0)
    direct = clip_track(engine, wav, start_beat=1.0)
    effect = engine.add_plugin_processor(engine.track_chain(late), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.set_processor_param(effect, FX_LATENCY, 100)
    engine.idle()  # the plug-in asked for a restart to change its latency
    assert [e.type for e in engine.take_processor_events()].count(ge.ProcessorEventType.LATENCY_CHANGED) == 1
    assert engine.processor_info(effect).latency == 100
    out = engine.render_offline(0.0, 2 * SPB)[:, 0]
    # Both clicks land on beat 1: the other track waits for the late one, and the
    # render starts that much earlier.
    assert np.nonzero(out)[0].tolist() == [SPB]
    assert out[SPB] == pytest.approx(1.0, abs=1e-4)

    # So does the metronome.
    engine.set_track_clips(late, [])
    engine.set_track_clips(direct, [])
    with_latency = engine.render_offline(0.0, SPB, metronome=True)
    engine.set_processor_enabled(effect, False)
    np.testing.assert_allclose(engine.render_offline(0.0, SPB, metronome=True), with_latency, atol=1e-6)


def test_state_restores_a_plugin(engine, uids):
    _, synth = plugin_track(engine, uids, "GIL Test Synth", [(0.0, 1.0, 60, 127)])
    engine.set_processor_param(synth, GAIN, 0.25)
    engine.set_processor_param(synth, WAVE, 0)
    engine.set_processor_param(synth, MACRO, 0.7)  # kept by the controller alone
    state = engine.processor_state(synth)
    assert state[:4] == b"VST3"  # a .vstpreset
    _, copy = plugin_track(engine, uids, "GIL Test Synth", [(0.0, 1.0, 60, 127)])
    engine.set_processor_state(copy, state)
    assert engine.processor_param(copy, GAIN) == pytest.approx(0.25)  # the controller knows...
    assert engine.processor_param(copy, WAVE) == 0.0
    assert engine.processor_param(copy, MACRO) == pytest.approx(0.7)
    assert ge.ProcessorEventType.PARAMS_CHANGED in [e.type for e in engine.take_processor_events()]
    np.testing.assert_allclose(engine.render_offline(0.0, 100)[50], 0.25 + 0.25)  # both play DC at gain 0.25

    # ...and so does the processor: a single-component plug-in's state too.
    _, effect = plugin_track(engine, uids, "GIL Test Effect")
    engine.set_processor_param(effect, FX_GAIN, 0.75)
    effect_state = engine.processor_state(effect)
    engine.set_processor_param(effect, FX_GAIN, 0.1)
    engine.set_processor_state(effect, effect_state)
    assert engine.processor_param(effect, FX_GAIN) == pytest.approx(0.75)
    with pytest.raises(RuntimeError, match="not for GIL Test Synth"):
        engine.set_processor_state(synth, effect_state)  # another plug-in's
    with pytest.raises(RuntimeError):
        engine.set_processor_state(synth, b"garbage")


def test_load_errors(engine, uids):
    track = engine.add_track()
    with pytest.raises(RuntimeError):
        engine.add_plugin_processor(engine.track_chain(track), "VST3", str(TEST_PLUGINS.parent / "Missing.vst3"), uids["GIL Test Synth"])
    with pytest.raises(RuntimeError, match="does not contain"):
        engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, "0" * 32)
    with pytest.raises(ValueError):
        engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, "not a uid")
    with pytest.raises(ValueError):
        engine.add_plugin_processor(engine.track_chain(track), "CLAP", PLUGINS, uids["GIL Test Synth"])
    with pytest.raises(ValueError):
        engine.add_plugin_processor(engine.track_chain(track) + 99, "VST3", PLUGINS, uids["GIL Test Synth"])


def test_chain_order(engine, uids):
    track, synth = plugin_track(engine, uids, "GIL Test Synth", [(0.0, 1.0, 60, 127)])
    engine.set_processor_param(synth, WAVE, 0)
    effect = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.set_processor_param(effect, FX_GAIN, 1.0)  # doubles
    np.testing.assert_allclose(engine.render_offline(0.0, 100), 2.0)
    # An instrument writes its output over what comes in: first the effect, then the synth.
    engine.set_chain_order(engine.track_chain(track), [effect, synth])
    np.testing.assert_allclose(engine.render_offline(0.0, 100), 1.0)
    for bad in ([effect], [effect, effect], [effect, synth, 999]):
        with pytest.raises(ValueError):
            engine.set_chain_order(engine.track_chain(track), bad)
    engine.remove_processor(effect)
    engine.remove_track(track)
    engine.idle()  # releases the plug-ins, on this thread


def test_a_plugin_moves_to_another_track_as_it_is(engine, uids, make_wav):
    click = np.zeros(1000)
    click[0] = 0.5
    wav = make_wav(click)
    a = clip_track(engine, wav, start_beat=1.0)
    b = clip_track(engine, wav, start_beat=1.0)
    effect = engine.add_plugin_processor(engine.track_chain(a), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.set_processor_param(effect, FX_GAIN, 1.0)  # doubles
    engine.set_processor_param(effect, FX_LATENCY, 100)
    engine.idle()
    state = engine.processor_state(effect)

    engine.move_processor(effect, engine.track_chain(b))
    assert engine.processor_chain(effect) == engine.track_chain(b)
    # The same processor, with its parameters and state: nothing loaded again.
    assert engine.processor_param(effect, FX_GAIN) == 1.0
    assert engine.processor_state(effect) == state
    engine.set_track_clips(a, [])
    out = engine.render_offline(0.0, 2 * SPB)[:, 0]
    assert np.nonzero(out)[0].tolist() == [SPB]  # compensated on its new track
    assert out[SPB] == pytest.approx(1.0, abs=1e-4)

    # Its old track can go without it.
    engine.remove_track(a)
    engine.idle()
    assert engine.processor_param(effect, FX_GAIN) == 1.0
    engine.remove_track(b)
    engine.idle()
    with pytest.raises(ValueError):
        engine.processor_info(effect)


def test_editor_edits_resizes_and_closes(engine, uids, dc_wav):
    track = clip_track(engine, dc_wav)
    effect = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Effect"])
    assert engine.open_editor(effect, 0, "GIL Test Effect - Audio")
    assert engine.is_editor_open(effect)
    frame = editor_window("GIL Test Effect - Audio")
    view = user32.GetWindow(frame, 5)  # GW_CHILD: the plug-in's own window
    assert frame and view and client_size(frame) == (400, 300)
    assert engine.open_editor(effect, 0, "Renamed")  # already open: raised, retitled
    assert editor_window("Renamed") == frame

    # A knob drag in the plug-in's editor: one gesture, heard at once, reported for undo.
    assert user32.SendMessageW(view, EDIT_GAIN, 0, 0)
    edits = events_of(engine, ge.ProcessorEventType.PARAM_EDITED)
    assert [(e.param_index, round(e.value, 6), e.old_value) for e in edits] == [(FX_GAIN, 0.25, 0.5),
                                                                                 (FX_GAIN, 0.3, 0.5)]
    assert edits[0].gesture == edits[1].gesture != 0 and edits[0].processor_id == effect
    assert engine.processor_param(effect, FX_GAIN) == pytest.approx(0.3)
    np.testing.assert_allclose(engine.render_offline(0.0, 100), 0.3, atol=1e-6)  # 0.5 * 2 * 0.3

    # The plug-in resizes its editor; the window follows.
    assert user32.SendMessageW(view, RESIZE, 500, 350)
    assert client_size(frame) == (500, 350) and client_size(view) == (500, 350)

    # The user drags the window smaller than the plug-in allows (200 x 150): it stops there,
    # and the view follows the window.
    outer = wintypes.RECT()
    user32.GetWindowRect(frame, ctypes.byref(outer))
    border = (outer.right - outer.left - 500, outer.bottom - outer.top - 350)
    wanted = wintypes.RECT(outer.left, outer.top, outer.left + 50, outer.top + 50)
    user32.SendMessageW(frame, 0x0214, 8, ctypes.addressof(wanted))  # WM_SIZING, dragging the bottom right corner
    assert (wanted.right - wanted.left, wanted.bottom - wanted.top) == (200 + border[0], 150 + border[1])
    user32.SetWindowPos(frame, None, 0, 0, 260 + border[0], 180 + border[1], 0x0002 | 0x0004)  # no move, z-order
    assert client_size(view) == (260, 180)

    assert user32.SendMessageW(view, DIRTY, 0, 0)
    engine.idle()
    assert events_of(engine, ge.ProcessorEventType.STATE_DIRTY)

    # Closing the window closes the editor.
    user32.SendMessageW(frame, 0x0010, 0, 0)  # WM_CLOSE
    engine.idle()
    assert not engine.is_editor_open(effect)
    assert events_of(engine, ge.ProcessorEventType.EDITOR_CLOSED)
    assert not editor_window("Renamed")

    # Removing the device closes its editor too.
    assert engine.open_editor(effect, 0, "Again")
    engine.remove_processor(effect)
    assert not editor_window("Again")


def test_plugins_without_an_editor(engine, uids):
    _, synth = plugin_track(engine, uids, "GIL Test Synth")
    assert not engine.open_editor(synth, 0, "x") and not engine.is_editor_open(synth)
    _, mono = plugin_track(engine, uids, "GIL Test Mono")
    assert not engine.open_editor(mono, 0, "x")


def test_a_latent_plugin_on_the_master(engine, uids, make_wav, tmp_path):
    click = np.zeros(1000)
    click[0] = 0.5
    track = clip_track(engine, make_wav(click), start_beat=1.0)
    effect = engine.add_plugin_processor(engine.track_chain(ge.MASTER), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.set_processor_param(effect, FX_LATENCY, 100)
    engine.idle()  # the plug-in asked for a restart to change its latency
    # The click lands on beat 1 still: the render starts that much earlier.
    out = engine.render_offline(0.0, 2 * SPB)[:, 0]
    assert np.nonzero(out)[0].tolist() == [SPB] and out[SPB] == pytest.approx(0.5, abs=1e-4)
    target = tmp_path / "mix.wav"
    engine.export_wav(str(target), 0.0, 2.0, bit_depth=32)
    with open(target, "rb") as f:
        data = f.read()
    exported = np.frombuffer(data[data.index(b"data") + 8:], dtype="<f4").reshape(-1, 2)[:, 0]
    assert np.nonzero(exported)[0].tolist() == [SPB]

    # The metronome is mixed after the master's devices: it waits for them too.
    engine.set_track_clips(track, [])
    with_latency = engine.render_offline(0.0, SPB, metronome=True)
    assert np.abs(with_latency).max() > 0.1
    engine.set_processor_enabled(effect, False)
    np.testing.assert_allclose(engine.render_offline(0.0, SPB, metronome=True), with_latency, atol=1e-6)


def test_master_device_automation_plays_in_time(engine, uids, make_wav):
    """A master device hears the timeline as late as the slowest track, plus the
    master's devices before it: its automation is as late."""
    track = clip_track(engine, make_wav(np.full((2 * SAMPLE_RATE, 2), 0.5)), duration_sec=2.0)
    late = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.set_processor_param(late, FX_LATENCY, 100)
    first = engine.add_plugin_processor(engine.track_chain(ge.MASTER), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.set_processor_param(first, FX_LATENCY, 50)
    engine.idle()
    utility = engine.add_builtin_processor(engine.track_chain(ge.MASTER), "utility")
    gain = engine.processor_params(utility)[engine.processor_param_index(utility, "gain")]
    quiet, loud = gain.to_normalized(-60.0), gain.to_normalized(0.0)
    step = SPB + 400  # not on a block boundary of the render (which starts 150 samples early)
    engine.set_track_automation(ge.MASTER, [ge.AutomationLane(utility, "gain", [
        ge.AutomationPoint(0.0, quiet), ge.AutomationPoint(step / SPB, quiet), ge.AutomationPoint(step / SPB, loud)])])
    out = engine.render_offline(0.0, step + 2000)[:, 0]
    assert np.abs(out[step - 300 : step]).max() < 0.001  # still at -60 dB right up to the step
    assert out[step + 40] > 0.01  # and rising from it at once (smoothed)
