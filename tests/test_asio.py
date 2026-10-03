"""ASIO, with the fake driver built alongside the engine (tests/asio_driver):
finding and opening drivers, sample rates and buffer sizes, channels, every
sample format, inputs, the driver's requests (resets, new latencies, another
clock), its control panel, errors, and the preferences in the application.

The driver runs in manual mode unless a test says otherwise: each
`driver.process(n)` is n buffer switches, made on the test's thread, so what
the engine plays can be checked sample by sample. Its outputs are read back as
the bytes the engine wrote, and decoded here with numpy."""

import ctypes
import time

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import SAMPLE_RATE, TEST_ASIO, TEST_ASIO_NAME

pytestmark = pytest.mark.skipif("ASIO" not in ge.driver_types() or not TEST_ASIO.exists(),
                                reason="built without the ASIO SDK")

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM
# asioMessage selectors (asio.h)
SELECTOR_SUPPORTED, ENGINE_VERSION, RESET_REQUEST, BUFFER_SIZE_CHANGE, RESYNC_REQUEST, LATENCIES_CHANGED = range(1, 7)
SUPPORTS_TIME_INFO, OVERLOAD = 7, 15
# ASIOSampleType -> (numpy dtype of the container, significant bits); 3-byte formats have no dtype
INT16_LSB, INT24_LSB, INT32_LSB, FLOAT32_LSB, FLOAT64_LSB = 16, 17, 18, 19, 20
FORMATS = {
    INT16_LSB: ("<i2", 16), INT24_LSB: ("<i3", 24), INT32_LSB: ("<i4", 32), FLOAT32_LSB: ("<f4", 0),
    FLOAT64_LSB: ("<f8", 0), 24: ("<i4", 16), 25: ("<i4", 18), 26: ("<i4", 20), 27: ("<i4", 24),
    0: (">i2", 16), 1: (">i3", 24), 2: (">i4", 32), 3: (">f4", 0),
}


def decode(raw: bytes, sample_type: int) -> np.ndarray:
    """A driver buffer's samples as floats."""
    dtype, bits = FORMATS[sample_type]
    if dtype.endswith("i3"):
        triples = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        if dtype[0] == ">":
            triples = triples[:, ::-1]
        values = triples[:, 0] | (triples[:, 1] << 8) | (triples[:, 2] << 16)
        values = np.where(values >= 1 << 23, values - (1 << 24), values)
    else:
        values = np.frombuffer(raw, dtype)
    return values.astype(np.float64) / (1 << (bits - 1)) if bits else values.astype(np.float64)


class Driver:
    """The test driver's hooks (exported from its DLL; see test_asio_driver.cpp)."""

    def __init__(self):
        dll = ctypes.CDLL(str(TEST_ASIO))  # the engine loads the same module
        signatures = {
            "Reset": (None, []), "SetSampleType": (None, [ctypes.c_long]),
            "SetBufferSizes": (None, [ctypes.c_long] * 4), "SetLatencies": (None, [ctypes.c_long] * 2),
            "SetManual": (None, [ctypes.c_int]), "SetInputLevel": (None, [ctypes.c_int, ctypes.c_double]), "SetLoopback": (None, [ctypes.c_int] * 2),
            "FailInit": (None, [ctypes.c_char_p]), "SetControlPanelChange": (None, [ctypes.c_long, ctypes.c_double]),
            "Process": (ctypes.c_int, [ctypes.c_int]),
            "ReadOutput": (ctypes.c_long, [ctypes.c_int, ctypes.c_void_p, ctypes.c_long]),
            "ClearOutput": (None, []), "SendMessage": (ctypes.c_long, [ctypes.c_long, ctypes.c_long]),
            "ChangeSampleRate": (None, [ctypes.c_double]), "Get": (ctypes.c_double, [ctypes.c_char_p]),
            "InitHandle": (ctypes.c_void_p, []),
        }
        for name, (restype, argtypes) in signatures.items():
            function = getattr(dll, "SubTestAsio_" + name)
            function.restype = restype
            function.argtypes = argtypes
            setattr(self, name, function)
        self.Reset()

    def get(self, key: str) -> float:
        return self.Get(key.encode())

    def output(self, channel: int) -> bytes:
        size = self.ReadOutput(channel, None, 0)
        buffer = ctypes.create_string_buffer(size)
        self.ReadOutput(channel, buffer, size)
        return buffer.raw[:size]

    def process(self, buffers: int) -> int:
        return self.Process(buffers)


@pytest.fixture
def driver():
    return Driver()


@pytest.fixture
def engine(driver):
    e = ge.Engine()
    yield e
    e.close_device()
    assert driver.get("instances") == 0, "the driver was not released"


def open_asio(engine, **settings):
    engine.open_device(TEST_ASIO_NAME, driver="ASIO", **settings)


def wait_until(predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.005)
    return False


def test_lists_the_driver(engine):
    assert ge.driver_types() == ["WASAPI", "ASIO"]
    assert [d.name for d in engine.list_devices("ASIO")] == [TEST_ASIO_NAME]


def test_status_and_capabilities(engine, driver):
    driver.SetManual(1)
    open_asio(engine, sample_rate=48000, buffer_frames=128, window=0x1234)
    status = engine.device_status
    assert (status.open, status.backend, status.name) == (True, "ASIO", TEST_ASIO_NAME)
    assert (status.sample_rate, status.buffer_frames, engine.sample_rate) == (48000, 128, 48000.0)
    assert status.latency_ms == pytest.approx((128 + 64) / 48)  # the driver's figures
    assert status.input_latency_ms == pytest.approx((128 + 32) / 48)
    assert (status.input_channels, status.output_channels) == ([], [0, 1])
    caps = engine.device_capabilities
    assert caps.input_names == [f"Test In {i}" for i in range(1, 5)]
    assert caps.output_names == [f"Test Out {i}" for i in range(1, 7)]
    assert caps.sample_rates == [44100, 48000, 96000]
    assert caps.buffer_sizes == [32, 64, 128, 256, 512, 1024, 2048]
    assert (caps.preferred_buffer_frames, caps.has_control_panel) == (256, True)
    assert driver.InitHandle() == 0x1234  # the window its dialogs belong to
    assert driver.get("time_info") == 1  # the engine takes bufferSwitchTimeInfo
    assert (driver.get("inputs"), driver.get("outputs")) == (0, 2)  # only the open channels


@pytest.mark.parametrize("sample_type", sorted(FORMATS))
def test_output_in_every_sample_format(engine, driver, ramp_wav, sample_type):
    driver.SetManual(1)
    driver.SetSampleType(sample_type)
    track = engine.add_track()
    engine.load_source(ramp_wav)
    engine.set_clip_fade_ms(0)
    engine.set_track_clips(track, [ge.ClipDesc(ramp_wav, 0.0, 1.0)])
    open_asio(engine, sample_rate=SAMPLE_RATE, buffer_frames=256, output_channels=[2, 3])
    engine.play()
    assert driver.process(8) == 8
    expected = np.arange(8 * 256) / 32768  # both halves of the double buffer, in order
    bits = FORMATS[sample_type][1]
    tolerance = 1.5 / (1 << (bits - 1)) if bits else 1e-7
    for channel in (2, 3):
        np.testing.assert_allclose(decode(driver.output(channel), sample_type), expected, atol=tolerance)
    for channel in (0, 1, 4, 5):
        assert driver.output(channel) == b""  # never opened
    assert engine.position_beats * SPB == pytest.approx(8 * 256)
    assert driver.get("output_ready") == 1 + 8  # asked once whether it takes it, then after each buffer


def test_full_scale_is_clipped(engine, driver, make_wav):
    driver.SetManual(1)
    loud = make_wav(np.full(SAMPLE_RATE, 0.9))
    track = engine.add_track()
    engine.load_source(loud)
    engine.set_clip_fade_ms(0)
    engine.set_track_clips(track, [ge.ClipDesc(loud, 0.0, 1.0)])
    engine.set_track_gain(track, 4.0)  # 3.6: over full scale
    open_asio(engine, sample_rate=SAMPLE_RATE, buffer_frames=64)
    engine.play()
    driver.process(4)
    values = np.frombuffer(driver.output(0), "<i4")
    assert values.max() == 2**31 - 1  # not wrapped round to negative


def test_mono_output_mixes_the_master(engine, driver, make_wav):
    driver.SetManual(1)
    left = make_wav(np.column_stack([np.full(SAMPLE_RATE, 0.5), np.zeros(SAMPLE_RATE)]))
    track = engine.add_track()
    engine.load_source(left)
    engine.set_clip_fade_ms(0)
    engine.set_track_clips(track, [ge.ClipDesc(left, 0.0, 1.0)])
    open_asio(engine, sample_rate=SAMPLE_RATE, buffer_frames=64, output_channels=[5])
    engine.play()
    driver.process(2)
    assert engine.device_status.output_channels == [5]
    np.testing.assert_allclose(decode(driver.output(5), INT32_LSB), 0.25, atol=1e-6)


@pytest.mark.parametrize("sample_type", [INT16_LSB, INT24_LSB, INT32_LSB, FLOAT32_LSB, FLOAT64_LSB, 27, 2])
def test_open_inputs_are_metered(engine, driver, sample_type):
    driver.SetManual(1)
    driver.SetSampleType(sample_type)
    driver.SetInputLevel(1, -0.25)
    driver.SetInputLevel(3, 0.75)
    open_asio(engine, input_channels=[3, 1])
    assert engine.device_status.input_channels == [3, 1]
    assert driver.get("inputs") == 2
    driver.process(2)
    np.testing.assert_allclose(engine.take_input_meters(), [0.75, 0.25], atol=1e-4)
    assert engine.take_input_meters() == [0.0, 0.0]  # since the last call


def test_sample_rates(engine, driver):
    driver.SetManual(1)
    open_asio(engine)  # the rate the driver runs at
    assert engine.device_status.sample_rate == 44100 and engine.sample_rate == 44100.0
    open_asio(engine, sample_rate=96000)
    assert engine.device_status.sample_rate == 96000 and driver.get("rate") == 96000
    with pytest.raises(RuntimeError, match="SUB Test ASIO can't run at 22050 Hz"):
        open_asio(engine, sample_rate=22050)
    assert not engine.device_status.open


def test_buffer_sizes(engine, driver):
    driver.SetManual(1)
    for asked, used in [(0, 256), (100, 128), (128, 128), (3000, 2048), (10, 32)]:
        open_asio(engine, buffer_frames=asked)
        assert engine.device_status.buffer_frames == used, asked
        assert driver.get("buffer_size") == used
    # A driver whose size is set in its control panel only.
    driver.SetBufferSizes(512, 512, 512, 0)
    open_asio(engine, buffer_frames=128)
    assert engine.device_status.buffer_frames == 512
    assert engine.device_capabilities.buffer_sizes == [512]
    # Sizes in steps of 48.
    driver.SetBufferSizes(48, 480, 96, 48)
    open_asio(engine, buffer_frames=200)
    assert engine.device_status.buffer_frames == 192  # the nearest it takes
    assert engine.device_capabilities.buffer_sizes == [48, 96, 192, 384, 480]  # the usual ones among them


def test_reset_request_reopens_with_the_same_settings(engine, driver):
    driver.SetManual(1)
    open_asio(engine, sample_rate=48000, buffer_frames=128, output_channels=[4, 5])
    assert engine.take_device_event() == ""
    assert driver.SendMessage(RESET_REQUEST, 0) == 1
    assert engine.take_device_event() == "reset"
    assert engine.take_device_event() == ""
    engine.reopen_device()
    status = engine.device_status
    assert (status.sample_rate, status.buffer_frames, status.output_channels) == (48000, 128, [4, 5])
    assert driver.get("inits") == 2


def test_buffer_size_change_request(engine, driver):
    driver.SetManual(1)
    open_asio(engine, buffer_frames=128)
    assert driver.SendMessage(BUFFER_SIZE_CHANGE, 512) == 1
    assert engine.take_device_event() == "reset"
    engine.reopen_device()
    assert engine.device_status.buffer_frames == 512


def test_driver_changes_its_clock(engine, driver):
    driver.SetManual(1)
    open_asio(engine, sample_rate=44100)
    driver.ChangeSampleRate(96000)  # the hardware follows an external clock
    assert engine.take_device_event() == "reset"
    engine.reopen_device()
    assert engine.device_status.sample_rate == 96000 and engine.sample_rate == 96000.0


def test_new_latencies(engine, driver):
    driver.SetManual(1)
    open_asio(engine, sample_rate=48000, buffer_frames=128)
    driver.SetLatencies(16, 480)
    assert driver.SendMessage(LATENCIES_CHANGED, 0) == 1
    assert engine.take_device_event() == "latency"
    assert engine.device_status.latency_ms == pytest.approx((128 + 480) / 48)
    assert engine.device_status.input_latency_ms == pytest.approx((128 + 16) / 48)


def test_questions_drivers_ask(engine, driver):
    open_asio(engine)
    assert driver.SendMessage(ENGINE_VERSION, 0) == 2
    assert driver.SendMessage(SUPPORTS_TIME_INFO, 0) == 1
    for selector in (RESET_REQUEST, BUFFER_SIZE_CHANGE, RESYNC_REQUEST, LATENCIES_CHANGED, SUPPORTS_TIME_INFO):
        assert driver.SendMessage(SELECTOR_SUPPORTED, selector) == 1
    assert driver.SendMessage(SELECTOR_SUPPORTED, OVERLOAD) == 0
    assert driver.SendMessage(RESYNC_REQUEST, 0) == 1  # nothing to redo
    assert engine.take_device_event() == ""


def test_control_panel(engine, driver):
    driver.SetManual(1)
    assert not engine.show_device_control_panel()  # no device
    open_asio(engine, buffer_frames=128)
    assert engine.show_device_control_panel()
    assert driver.get("control_panels") == 1
    assert engine.take_device_event() == ""  # nothing changed
    # The user sets another buffer size in it; the driver doesn't ask for a reset.
    driver.SetControlPanelChange(1024, 0)
    assert engine.show_device_control_panel()
    assert engine.take_device_event() == "reset"
    engine.reopen_device()
    assert engine.device_status.buffer_frames == 1024


def test_errors_leave_no_driver_open(engine, driver):
    driver.FailInit(b"the interface is unplugged")
    with pytest.raises(RuntimeError, match="^SUB Test ASIO could not be started: the interface is unplugged$"):
        open_asio(engine)
    driver.FailInit(None)
    assert not engine.device_status.open and driver.get("instances") == 0
    with pytest.raises(RuntimeError, match="^ASIO driver not found: Nope$"):
        engine.open_device("Nope", driver="ASIO")
    with pytest.raises(RuntimeError, match="^SUB Test ASIO has no output 7$"):
        open_asio(engine, output_channels=[6])
    with pytest.raises(RuntimeError, match="^SUB Test ASIO has no input 5$"):
        open_asio(engine, input_channels=[4])
    with pytest.raises(RuntimeError, match="listed twice"):
        open_asio(engine, output_channels=[1, 1])
    with pytest.raises(RuntimeError, match="Unknown driver type: CoreAudio"):
        engine.open_device(driver="CoreAudio")
    assert driver.get("instances") == 0
    with pytest.raises(RuntimeError, match="No audio device has been opened"):
        engine.reopen_device()


def test_one_asio_device_per_program(engine, driver):
    driver.SetManual(1)
    open_asio(engine)
    other = ge.Engine()
    with pytest.raises(RuntimeError, match="Another ASIO device is open"):
        open_asio(other)
    engine.close_device()
    open_asio(other)  # now it can
    other.close_device()


def test_plays_on_the_drivers_thread(engine, driver, dc_wav):
    track = engine.add_track()
    engine.load_source(dc_wav)
    engine.set_track_clips(track, [ge.ClipDesc(dc_wav, 0.0, 1.0)])
    for _ in range(3):  # open and close while the driver's thread runs
        open_asio(engine, sample_rate=SAMPLE_RATE, buffer_frames=64)
        engine.play()
        assert wait_until(lambda: engine.position_beats * SPB > 20 * 64)
        engine.stop()
        engine.position_beats = 0.0
        engine.close_device()
        assert driver.get("running") == 0
    assert driver.get("starts") == 3
    samples = decode(driver.output(0), INT32_LSB)
    assert np.isclose(samples, 0.5, atol=1e-6).any()


def test_preferences_open_asio(window, driver):
    from substation.audio.settings import AudioSettings
    from substation.ui.dialogs import PreferencesDialog

    prefs = PreferencesDialog(window.bridge, window)
    prefs.driver.setCurrentIndex(prefs.driver.findData("ASIO"))
    engine = window.engine
    status = engine.device_status
    assert (status.open, status.backend, status.name) == (True, "ASIO", TEST_ASIO_NAME)
    assert status.buffer_frames == 256  # the driver's preferred size
    assert driver.InitHandle() == int(window.winId())
    assert prefs.control_panel.isEnabled()
    assert [prefs.outputs.itemText(i) for i in range(prefs.outputs.count())] == [
        "1/2 · Test Out 1, Test Out 2", "3/4 · Test Out 3, Test Out 4", "5/6 · Test Out 5, Test Out 6"]
    assert [prefs.sample_rate.itemData(i) for i in range(prefs.sample_rate.count())] == [44100, 48000, 96000]

    prefs.outputs.setCurrentIndex(1)
    assert engine.device_status.output_channels == [2, 3]
    prefs.buffer.setCurrentIndex(prefs.buffer.findData(128))
    prefs.sample_rate.setCurrentIndex(prefs.sample_rate.findData(48000))
    status = engine.device_status
    assert (status.buffer_frames, status.sample_rate, status.output_channels) == (128, 48000, [2, 3])
    assert AudioSettings.load() == AudioSettings("ASIO", TEST_ASIO_NAME, 48000, 128, False, (2, 3), ())
    assert "SUB Test ASIO" in window.transport.device.text()

    prefs.control_panel.click()
    assert driver.get("control_panels") == 1

    # The driver asks for a reset: the application opens it again with the same settings.
    inits = driver.get("inits")
    driver.SendMessage(RESET_REQUEST, 0)
    window.bridge._poll_meters()
    assert driver.get("inits") == inits + 1
    assert engine.device_status.output_channels == [2, 3]
    prefs.reject()

    # Next time the application starts with it.
    engine.close_device()
    window.start_audio()
    status = engine.device_status
    assert (status.backend, status.name, status.sample_rate, status.buffer_frames) == ("ASIO", TEST_ASIO_NAME,
                                                                                       48000, 128)


def test_start_audio_falls_back_to_the_drivers_own_settings(window, driver):
    from substation.audio.settings import AudioSettings

    AudioSettings("ASIO", TEST_ASIO_NAME, 22050, 128, False, (2, 3), ()).save()  # a rate it no longer runs at
    window.start_audio()
    status = window.engine.device_status
    assert (status.backend, status.sample_rate, status.buffer_frames, status.output_channels) == (
        "ASIO", 44100, 256, [0, 1])
    assert "Using the device's own settings instead" in window.statusBar().currentMessage()
