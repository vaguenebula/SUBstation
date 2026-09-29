import wave
from pathlib import Path

import numpy as np
import pytest

SAMPLE_RATE = 48000  # the engine's rate when no device is open


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
