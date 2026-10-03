import os
import sys
import tempfile
import traceback
import wave
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

from substation import _engine, engine_mismatch

SAMPLE_RATE = 48000  # the engine's rate when no device is open

# The VST3 test plug-ins, built with the engine (tests/vst3_plugins). The tests see
# only these, never the plug-ins installed on the computer, and keep their own scan cache.
TEST_PLUGINS = Path(_engine.__file__).parent / "_testplugins" / "SUBTestPlugins.vst3"

if (_mismatch := engine_mismatch()) is not None:  # the tests would fail in confusing ways
    pytest.exit(_mismatch, returncode=1)
os.environ["SUBSTATION_VST3_PATH"] = str(TEST_PLUGINS.parent)
os.environ["SUBSTATION_PLUGIN_CACHE"] = str(Path(tempfile.mkdtemp(prefix="sub-plugin-cache-")) / "vst3-cache.json")
# Nor the browser's use counts or its index (each window gets its own; see `window`).
os.environ["SUBSTATION_LIBRARY"] = str(Path(tempfile.mkdtemp(prefix="sub-library-")) / "library.json")
os.environ["SUBSTATION_BROWSER_INDEX"] = str(Path(tempfile.mkdtemp(prefix="sub-index-")) / "browser-index.bin")
# Nor the user's preset library.
os.environ["SUBSTATION_PRESETS"] = str(Path(tempfile.mkdtemp(prefix="sub-presets-")) / "Presets")

# The fake ASIO driver (tests/asio_driver), built with the engine when it has the
# ASIO SDK. The tests see only it, never the drivers installed on the computer.
TEST_ASIO = Path(_engine.__file__).parent / "_testdrivers" / "SUBTestAsio.dll"
TEST_ASIO_NAME = "SUB Test ASIO"
os.environ["SUBSTATION_ASIO_DRIVERS"] = f"{TEST_ASIO_NAME}|{{5B2E8C1A-7F3D-4E6B-9C0A-1D2F3E4A5B6C}}|{TEST_ASIO}"


def write_wav(path: Path, samples: np.ndarray, sample_rate: int = SAMPLE_RATE) -> Path:
    """Write float samples in [-1, 1) as 16-bit PCM. `samples` is (frames,) or (frames, channels)."""
    if samples.ndim == 1:
        samples = samples[:, None]
    pcm = np.round(samples * 32768.0).clip(-32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as f:
        f.setnchannels(pcm.shape[1])
        f.setsampwidth(2)
        f.setframerate(sample_rate)
        f.writeframes(pcm.tobytes())
    return path


@pytest.fixture
def make_wav(tmp_path):
    counter = iter(range(10_000))

    def factory(samples: np.ndarray, sample_rate: int = SAMPLE_RATE) -> str:
        return str(write_wav(tmp_path / f"clip{next(counter)}.wav", samples, sample_rate))

    return factory


@pytest.fixture
def dc_wav(make_wav):
    """One second of constant 0.5 in both channels."""
    return make_wav(np.full((SAMPLE_RATE, 2), 0.5))


@pytest.fixture
def ramp_wav(make_wav):
    """Mono file whose sample i has the value i / 32768 (exact in 16-bit PCM)."""
    return make_wav(np.arange(SAMPLE_RATE) % 32768 / 32768.0)


@pytest.fixture(scope="module")
def app():
    from PySide6.QtCore import QCoreApplication
    from PySide6.QtWidgets import QApplication

    from substation import theme

    QCoreApplication.setOrganizationName("SUBstation Tests")  # keep tests out of the user's settings
    QCoreApplication.setApplicationName("SUBstation Tests")
    application = QApplication.instance() or QApplication([])
    theme.apply(application)
    yield application


@pytest.fixture
def window(app, tmp_path):
    """The real main window, offscreen, with no audio device."""
    from PySide6.QtCore import QSettings

    from substation import _engine as ge
    from substation.ui.main_window import MainWindow

    settings = QSettings()
    settings.clear()
    settings.setValue("browser/places", [str(tmp_path)])
    os.environ["SUBSTATION_LIBRARY"] = str(tmp_path / "library.json")
    os.environ["SUBSTATION_BROWSER_INDEX"] = str(tmp_path / "browser-index.bin")
    os.environ["SUBSTATION_PRESETS"] = str(tmp_path / "Presets")
    # Exceptions raised inside Qt slots are only printed; collect them instead.
    errors = []
    previous_hook = sys.excepthook
    sys.excepthook = lambda *exc_info: errors.append(exc_info)
    engine = ge.Engine()
    w = MainWindow(engine)
    w.resize(1400, 820)
    w.show()
    app.processEvents()
    yield w
    w.undo_stack.setClean()
    w.close()
    engine.close_device()
    w.bridge.shutdown()  # unloads the plug-ins
    w.deleteLater()
    app.processEvents()
    sys.excepthook = previous_hook
    assert not errors, "".join("".join(traceback.format_exception(*e)) for e in errors)
