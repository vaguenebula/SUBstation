"""Sidechains in the engine: a device's aux input hearing another track's
signal, after its fader, before it or after one of its devices; lined up with
the signal at the device sample for sample, whatever the latency before the tap
and on the device's own track (the sidechain is delayed, or the track's signal
is, before the device); cycles refused; the source going; mute and solo.
Rendered offline with GIL Test Sidechain (its output: its input plus its
sidechain), so no audio device is needed."""

import numpy as np
import pytest

from gilstudio import _engine as ge

from .conftest import SAMPLE_RATE, TEST_PLUGINS

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM
PLUGINS = str(TEST_PLUGINS)
FX_GAIN, FX_LATENCY = 0, 1  # GIL Test Effect's parameters
KEY_SILENT = 0  # GIL Test Sidechain's (read-only)
CLICK = 0.25

pytestmark = pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


@pytest.fixture(scope="module")
def uids():
    return {d.name: d.uid for d in ge.scan_vst3(PLUGINS)} if TEST_PLUGINS.exists() else {}


@pytest.fixture
def click_wav(make_wav):
    click = np.zeros(1000)
    click[0] = CLICK
    return make_wav(click)


def clip_track(engine, path, start_beat=1.0, output=None, seconds=1000 / SAMPLE_RATE):
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, start_beat, seconds)])
    if output is not None:
        engine.set_track_output(track, output)
    return track


def effect(engine, uids, track, latency=0, gain=1.0):
    """GIL Test Effect: `gain` times its input, `latency` samples late."""
    pid = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.set_processor_param(pid, FX_GAIN, gain / 2)  # (0..1 is 0..2 times)
    engine.set_processor_param(pid, FX_LATENCY, latency)  # (in steps: samples)
    engine.idle()  # the plug-in asked for a restart to change its latency
    assert engine.processor_info(pid).latency == latency
    return pid


def keyed(engine, uids, track):
    """GIL Test Sidechain on a track (its output: its input plus its sidechain)."""
    return engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Sidechain"])


def render(engine, beats=2.0):
    return engine.render_offline(0.0, int(beats * SPB))[:, 0]


def clicks(out) -> dict[int, float]:
    return {int(i): round(float(out[i]), 6) for i in np.nonzero(np.abs(out) > 1e-7)[0]}


def test_a_device_hears_its_sidechain(engine, uids, click_wav):
    source = clip_track(engine, click_wav)
    track = engine.add_track()  # no clips: what its device puts out is what it hears on its sidechain
    pid = keyed(engine, uids, track)
    assert engine.processor_info(pid).has_sidechain
    assert engine.processor_sidechain(pid) is None
    assert clicks(render(engine)) == {SPB: CLICK}
    engine.set_processor_sidechain(pid, source)
    info = engine.processor_sidechain(pid)
    assert (info.track_id, info.tap, info.tap_processor_id) == (source, ge.SidechainTap.POST_FADER, 0)
    assert clicks(render(engine)) == {SPB: 2 * CLICK}  # the source, and the device's output
    engine.clear_processor_sidechain(pid)
    assert engine.processor_sidechain(pid) is None
    assert clicks(render(engine)) == {SPB: CLICK}


def test_devices_without_a_sidechain_input(engine, uids):
    source, track = engine.add_track(), engine.add_track()
    utility = engine.add_builtin_processor(engine.track_chain(track), "utility")
    fx = effect(engine, uids, track)
    for pid in (utility, fx):
        assert not engine.processor_info(pid).has_sidechain
        with pytest.raises(ValueError):
            engine.set_processor_sidechain(pid, source)


def test_taps(engine, uids, click_wav):
    """After the source's fader, before it, or after one of its devices."""
    source = clip_track(engine, click_wav)
    first, second = effect(engine, uids, source, gain=0.5), effect(engine, uids, source, gain=0.5)
    engine.set_track_gain(source, 0.5)
    pid = keyed(engine, uids, engine.add_track())
    heard = CLICK * 0.5 * 0.5 * 0.5  # the source itself: its devices, then its fader
    for tap, after, key in [(ge.SidechainTap.POST_FADER, 0, heard),
                            (ge.SidechainTap.PRE_FADER, 0, CLICK * 0.25),
                            (ge.SidechainTap.AFTER_DEVICE, first, CLICK * 0.5),
                            (ge.SidechainTap.AFTER_DEVICE, second, CLICK * 0.25)]:
        engine.set_processor_sidechain(pid, source, tap, after)
        assert engine.processor_sidechain(pid).tap_processor_id == after
        assert clicks(render(engine)) == {SPB: pytest.approx(heard + key)}, tap
    with pytest.raises(ValueError):  # not the source's device
        engine.set_processor_sidechain(pid, source, ge.SidechainTap.AFTER_DEVICE, pid)
    # A device switched off passes the signal on as it is.
    engine.set_processor_enabled(first, False)
    assert clicks(render(engine)) == {SPB: pytest.approx(CLICK * 0.5 * 0.5 + CLICK * 0.5)}


def test_a_tap_after_a_device_that_leaves_the_source_is_before_the_fader(engine, uids, click_wav):
    source = clip_track(engine, click_wav)
    fx = effect(engine, uids, source, gain=0.5)
    engine.set_track_gain(source, 0.5)
    pid = keyed(engine, uids, engine.add_track())
    engine.set_processor_sidechain(pid, source, ge.SidechainTap.AFTER_DEVICE, fx)
    other = engine.add_track()
    engine.move_processor(fx, engine.track_chain(other))
    assert engine.processor_sidechain(pid).tap == ge.SidechainTap.AFTER_DEVICE  # as it was set
    assert clicks(render(engine)) == {SPB: pytest.approx(CLICK * 0.5 + CLICK)}  # but before the fader
    engine.move_processor(fx, engine.track_chain(source))
    assert clicks(render(engine)) == {SPB: pytest.approx(CLICK * 0.25 + CLICK * 0.5)}  # after it again


@pytest.mark.parametrize("before_tap, after_tap, before_device, after_device", [
    (300, 0, 0, 0),     # the sidechain comes late: the track's signal waits before the device
    (0, 0, 200, 0),     # the track's signal comes late: the sidechain waits
    (300, 100, 0, 0),   # latency after the tap doesn't count
    (300, 0, 200, 50),  # some of each
    (0, 450, 120, 0),
])
@pytest.mark.parametrize("tap", ["post", "pre", "device"])
def test_the_sidechain_lines_up_with_the_signal_at_its_device(engine, uids, click_wav, before_tap, after_tap,
                                                              before_device, after_device, tap):
    """The source and the device's track click on the same beat: at the device
    the clicks fall on one sample (whose output is two clicks), and at the
    master the source's click too."""
    source = clip_track(engine, click_wav)
    tapped = effect(engine, uids, source, before_tap)
    effect(engine, uids, source, after_tap)
    track = clip_track(engine, click_wav)
    effect(engine, uids, track, before_device)
    pid = keyed(engine, uids, track)
    effect(engine, uids, track, after_device)
    if tap == "device":
        engine.set_processor_sidechain(pid, source, ge.SidechainTap.AFTER_DEVICE, tapped)
    else:
        engine.set_processor_sidechain(pid, source, ge.SidechainTap.POST_FADER if tap == "post"
                                       else ge.SidechainTap.PRE_FADER)
    assert clicks(render(engine)) == {SPB: 3 * CLICK}


def test_a_sidechain_into_a_group_and_into_the_master(engine, uids, click_wav):
    source = clip_track(engine, click_wav)
    effect(engine, uids, source, 250)
    group = engine.add_track()
    clip_track(engine, click_wav, output=group)
    effect(engine, uids, group, 70)
    in_group = keyed(engine, uids, group)
    engine.set_processor_sidechain(in_group, source)
    assert clicks(render(engine)) == {SPB: 3 * CLICK}
    effect(engine, uids, ge.MASTER, 40)
    on_master = keyed(engine, uids, ge.MASTER)
    effect(engine, uids, ge.MASTER, 90)
    engine.set_processor_sidechain(on_master, source)
    assert clicks(render(engine)) == {SPB: 4 * CLICK}


def test_automation_after_a_device_waiting_for_its_sidechain_stays_in_time(engine, uids, make_wav):
    """A device after a sidechained one hears the timeline as late as the
    track's signal waited for the sidechain: its automation is as late."""
    source = engine.add_track()
    effect(engine, uids, source, 300)
    dc = make_wav(np.full((4 * SAMPLE_RATE, 2), 0.5))
    track = clip_track(engine, dc, start_beat=0.0, seconds=4.0)
    engine.set_processor_sidechain(keyed(engine, uids, track), source)
    utility = engine.add_builtin_processor(engine.track_chain(track), "utility")
    info = engine.processor_params(utility)[engine.processor_param_index(utility, "gain")]
    step = SPB + 100
    quiet, loud = info.to_normalized(-60.0), info.to_normalized(0.0)
    engine.set_track_automation(track, [ge.AutomationLane(utility, "gain", [
        ge.AutomationPoint(0.0, quiet), ge.AutomationPoint(step / SPB, quiet), ge.AutomationPoint(step / SPB, loud)])])
    out = engine.render_offline(0.0, step + 2000)[:, 0]
    assert np.abs(out[step - 200:step]).max() < 0.001  # settled at -60 dB, right up to the step
    assert out[step - 1] < out[step + 10] < out[step + 1500]  # rising from it


def test_cycles_are_refused(engine, uids):
    track, other = engine.add_track(), engine.add_track()
    group = engine.add_track()
    engine.set_track_output(track, group)
    pid = keyed(engine, uids, track)
    with pytest.raises(ValueError):
        engine.set_processor_sidechain(pid, track)  # its own track
    with pytest.raises(ValueError):
        engine.set_processor_sidechain(pid, group)  # what its track goes into
    with pytest.raises(ValueError):
        engine.set_processor_sidechain(pid, ge.MASTER)  # renders after everything
    with pytest.raises(ValueError):
        engine.set_processor_sidechain(pid, 999)
    on_group = keyed(engine, uids, group)
    engine.set_processor_sidechain(on_group, track)  # what goes into it: fine (no cycle)
    engine.set_processor_sidechain(pid, other)
    # The routes it would close a cycle with: outputs, sends and inputs.
    with pytest.raises(ValueError):
        engine.set_track_output(track, other)
    with pytest.raises(ValueError):
        engine.set_track_send(track, other, 1.0)
    with pytest.raises(ValueError):
        engine.set_track_input_track(other, track)
    # A move that would make it one.
    with pytest.raises(ValueError):
        engine.move_processor(pid, engine.track_chain(other))
    assert engine.processor_chain(pid) == engine.track_chain(track)
    master_chain = engine.track_chain(ge.MASTER)
    engine.move_processor(pid, master_chain)  # its sidechain goes with it
    assert engine.processor_chain(pid) == master_chain and engine.processor_sidechain(pid).track_id == other


def test_the_sidechain_goes_with_its_source(engine, uids, click_wav):
    source = clip_track(engine, click_wav)
    track = engine.add_track()
    pid = keyed(engine, uids, track)
    engine.set_processor_sidechain(pid, source)
    engine.remove_track(source)
    assert engine.processor_sidechain(pid) is None
    assert clicks(render(engine)) == {}
    other = clip_track(engine, click_wav)
    engine.set_processor_sidechain(pid, other)
    engine.remove_track(track)  # and the device with its track
    assert clicks(render(engine)) == {SPB: CLICK}


def test_mute_and_solo(engine, uids, click_wav):
    """A sidechain isn't heard on its own: soloing its source doesn't make the
    device's track heard, soloing that track keeps the source keying it; mute
    and solo silence it only after the source's fader."""
    source = clip_track(engine, click_wav)
    track = engine.add_track()
    pid = keyed(engine, uids, track)
    engine.set_processor_sidechain(pid, source)
    engine.set_track_solo(source, True)
    assert clicks(render(engine)) == {SPB: CLICK}  # the source alone
    engine.set_track_solo(source, False)
    engine.set_track_solo(track, True)
    assert clicks(render(engine)) == {SPB: CLICK}  # the track alone, keyed by the source
    engine.set_track_solo(track, False)
    engine.set_track_mute(source, True)
    assert clicks(render(engine)) == {}
    engine.set_processor_sidechain(pid, source, ge.SidechainTap.PRE_FADER)
    assert clicks(render(engine)) == {SPB: CLICK}  # before the fader: muting doesn't change it
    # Into the master's devices, the sidechain plays whatever is soloed.
    engine.set_track_mute(source, False)
    engine.set_processor_sidechain(pid, source)
    engine.move_processor(pid, engine.track_chain(ge.MASTER))
    engine.set_track_solo(engine.add_track(), True)  # something silent
    assert clicks(render(engine)) == {SPB: CLICK}  # the source isn't heard, but keys the master's device


def test_silence_is_flagged(engine, uids, dc_wav):
    pid = keyed(engine, uids, engine.add_track())
    engine.render_offline(0.0, 4096)
    engine.idle()  # the plug-in's output parameters
    assert engine.processor_param(pid, KEY_SILENT) == 1.0  # no source
    source = clip_track(engine, dc_wav, start_beat=0.0, seconds=1.0)
    engine.set_processor_sidechain(pid, source)
    engine.render_offline(0.0, 4096)
    engine.idle()
    assert engine.processor_param(pid, KEY_SILENT) == 0.0
    engine.set_track_mute(source, True)
    engine.render_offline(0.0, 4096)
    engine.idle()
    assert engine.processor_param(pid, KEY_SILENT) == 1.0  # silent after the source's fader


def test_the_source_renders_before_the_device_on_any_threads(engine, uids, click_wav):
    """Many tracks keyed by one source, rendered on several threads: each waits for it."""
    source = clip_track(engine, click_wav)
    effect(engine, uids, source, 33)
    for _ in range(12):
        track = clip_track(engine, click_wav)
        engine.add_builtin_processor(engine.track_chain(track), "utility")
        engine.set_processor_sidechain(keyed(engine, uids, track), source)
    for threads in (1, 4):
        engine.audio_threads = threads
        assert clicks(render(engine)) == {SPB: pytest.approx(25 * CLICK)}
