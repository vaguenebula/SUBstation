import json

import pytest

from gilstudio.model.project import PluginRef
from gilstudio.ui.browser.browser_models import BrowserItem
from gilstudio.ui.browser.library import HALF_LIFE_DAYS, Library
from gilstudio.ui.browser.search import find, match_quality

DAY = 86400.0


class Clock:
    def __init__(self):
        self.now = 1_000_000_000.0

    def __call__(self) -> float:
        return self.now


@pytest.fixture
def clock():
    return Clock()


@pytest.fixture
def library(tmp_path, clock):
    return Library(tmp_path / "library.json", clock)


def audio(name: str, folder: str = "Drums") -> BrowserItem:
    return BrowserItem(name, f"C:/Samples/{folder}/{name}", "audio", folder)


def names(items: list[BrowserItem]) -> list[str]:
    return [i.name for i in items]


def test_keys_identify_items():
    assert audio("Kick.wav").key == BrowserItem("Kick.wav", "c:/samples/drums/KICK.WAV", "audio").key
    ref = PluginRef(format="VST3", uid="abc", name="Synth", vendor="V", path="C:/a.vst3")
    assert BrowserItem("Synth", "C:/a.vst3", "plugin", "V", ref).key == "plugin:VST3:abc"
    assert BrowserItem("Utility", "utility", "device").key == "device:utility"


def test_uses_decay_and_persist(library, clock, tmp_path):
    library.record_use(["audio:a"])
    library.record_use(["audio:a"])
    assert library.uses("audio:a") == 2 and library.rank("audio:a") == pytest.approx(2.0)
    clock.now += HALF_LIFE_DAYS * DAY
    assert library.rank("audio:a") == pytest.approx(1.0)
    library.record_use(["audio:a"])
    assert library.rank("audio:a") == pytest.approx(2.0)
    assert library.rank("audio:never") == 0.0
    again = Library(tmp_path / "library.json", clock)
    assert again.uses("audio:a") == 3 and again.rank("audio:a") == pytest.approx(2.0)


def test_unknown_fields_are_kept(tmp_path, clock):
    path = tmp_path / "library.json"
    path.write_text(json.dumps({"version": 1, "items": {"audio:a": {"hidden": True}}}), encoding="utf-8")
    library = Library(path, clock)
    library.record_use(["audio:a"])
    assert json.loads(path.read_text(encoding="utf-8"))["items"]["audio:a"]["hidden"] is True


def test_bad_file_is_ignored(tmp_path, clock):
    path = tmp_path / "library.json"
    path.write_text("{not json", encoding="utf-8")
    assert Library(path, clock).records == {}


def test_match_quality_prefers_name_starts():
    terms = ["kick"]
    assert match_quality(audio("kick.wav"), terms) > match_quality(audio("Kick 01.wav"), terms) \
        > match_quality(audio("Big_Kick.wav"), terms) > match_quality(audio("Bigkick.wav"), terms) \
        > match_quality(audio("Snare.wav", "Kicks"), terms)


def test_rank_puts_used_items_first(library, clock):
    items = [audio("Kick A.wav"), audio("Big Kick.wav"), audio("Kick B.wav"), audio("Snare.wav")]
    assert names(find(items, "kick", library)) == ["Kick A.wav", "Kick B.wav", "Big Kick.wav"]
    library.record_use([items[2].key])
    clock.now += DAY
    library.record_use([items[1].key, items[1].key])
    assert names(find(items, "kick", library)) == ["Big Kick.wav", "Kick B.wav", "Kick A.wav"]
    # Without a search, only the used items move.
    assert names(find(items, "", library)) == ["Big Kick.wav", "Kick B.wav", "Kick A.wav", "Snare.wav"]
    assert names(find(items, "kick", library, "name")) == ["Big Kick.wav", "Kick A.wav", "Kick B.wav"]
