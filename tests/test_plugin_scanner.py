"""Plug-in scanning: files are read in child processes, so a plug-in that
crashes or hangs costs only itself; results are cached per file."""

import os
import shutil
import time

import pytest

from substation.plugins.scanner import (
    PluginScanner,
    _friendly,
    find_plugin_files,
    search_paths,
)

from .conftest import TEST_PLUGINS

pytestmark = pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")


@pytest.fixture
def plugins(tmp_path):
    """A copy of the test bundle, and two files that are not plug-ins."""
    bundle = tmp_path / "VST3" / "SUBTestPlugins.vst3"
    shutil.copytree(TEST_PLUGINS, bundle)
    junk = tmp_path / "VST3" / "Junk.vst3"
    junk.write_bytes(b"not a plug-in")
    more_junk = tmp_path / "VST3" / "More Junk.vst3"
    more_junk.write_bytes(b"nor this")
    return str(bundle), str(junk), str(more_junk)


def test_scan_reads_plugins_and_caches_them(tmp_path, plugins):
    bundle, junk, _ = plugins
    scanner = PluginScanner(cache_file=tmp_path / "cache.json")
    read = []
    result = scanner.scan([bundle, junk], progress=lambda done, total, path: read.append((done, total, path)))
    assert read == [(0, 2, bundle), (1, 2, junk)]
    assert [(p.name, p.vendor, p.category, p.instrument) for p in result.plugins] == [
        ("SUB Test Effect", "SUBstation", "Fx|Delay", False),
        ("SUB Test Mono", "SUBstation", "Fx", False),
        ("SUB Test Sidechain", "SUBstation", "Fx|Dynamics", False),
        ("SUB Test Synth", "SUBstation", "Instrument|Synth", True),
    ]
    assert all(p.path == bundle and p.format == "VST3" and len(p.uid) == 32 for p in result.plugins)
    [failure] = result.failures
    assert failure.path == junk and failure.reason == "Windows could not load it: it is not a 64-bit Windows plug-in."

    # Unchanged files are not read again...
    read.clear()
    again = scanner.scan([bundle, junk], progress=lambda *args: read.append(args))
    assert read == [] and again.plugins == result.plugins and again.failures == result.failures
    # ...changed ones are...
    binary = os.path.join(bundle, "Contents", "x86_64-win", "SUBTestPlugins.vst3")
    later = time.time() + 10
    os.utime(binary, (later, later))
    scanner.scan([bundle, junk], progress=lambda *args: read.append(args))
    assert [args[2] for args in read] == [bundle]
    # ...and a rescan reads everything.
    read.clear()
    scanner.scan([bundle, junk], rescan=True, progress=lambda *args: read.append(args))
    assert len(read) == 2


def test_a_crashing_plugin_costs_only_itself(tmp_path, plugins, monkeypatch):
    bundle, junk, more_junk = plugins
    monkeypatch.setenv("SUB_TEST_PLUGIN_CRASH", "1")  # the test plug-ins kill the process as they load
    result = PluginScanner(cache_file=tmp_path / "cache.json").scan([junk, bundle, more_junk])
    assert result.plugins == []
    reasons = {os.path.basename(f.path): f.reason for f in result.failures}
    assert reasons["SUBTestPlugins.vst3"] == "The plug-in crashed while loading."
    # The files before and after it were read, the latter by a new worker.
    assert reasons["Junk.vst3"].startswith("Windows could not load it")
    assert reasons["More Junk.vst3"].startswith("Windows could not load it")


def test_a_hanging_plugin_times_out(tmp_path, plugins, monkeypatch):
    bundle, junk, _ = plugins
    monkeypatch.setenv("SUB_TEST_PLUGIN_HANG", "1")
    started = time.monotonic()
    result = PluginScanner(cache_file=tmp_path / "cache.json", timeout=2.0).scan([bundle, junk])
    assert time.monotonic() - started < 10
    reasons = {os.path.basename(f.path): f.reason for f in result.failures}
    assert reasons == {"SUBTestPlugins.vst3": "The plug-in timed out.",
                       "Junk.vst3": "Windows could not load it: it is not a 64-bit Windows plug-in."}


def test_finding_plugin_files(tmp_path, monkeypatch):
    (tmp_path / "A" / "Vendor").mkdir(parents=True)
    (tmp_path / "A" / "Vendor" / "Single.vst3").write_bytes(b"")
    bundle = tmp_path / "A" / "Bundle.vst3" / "Contents" / "x86_64-win"
    bundle.mkdir(parents=True)
    (bundle / "Bundle.vst3").write_bytes(b"")  # inside the bundle: not listed on its own
    (tmp_path / "A" / "readme.txt").write_text("hello")
    (tmp_path / "B").mkdir()
    files = find_plugin_files([tmp_path / "A", tmp_path / "B", tmp_path / "Missing"])
    assert [os.path.relpath(f, tmp_path) for f in files] == [os.path.join("A", "Bundle.vst3"),
                                                             os.path.join("A", "Vendor", "Single.vst3")]
    # A vendor folder that is a link to somewhere else (as some installers make)
    # is looked in; a link back up to the root doesn't loop.
    (tmp_path / "Elsewhere").mkdir()
    (tmp_path / "Elsewhere" / "Linked.vst3").write_bytes(b"")
    try:
        os.symlink(tmp_path / "Elsewhere", tmp_path / "B" / "Vendor", target_is_directory=True)
        os.symlink(tmp_path / "B", tmp_path / "Elsewhere" / "Back", target_is_directory=True)
    except OSError:
        pass  # creating links needs Developer Mode or admin on Windows
    else:
        linked = find_plugin_files([tmp_path / "B"])
        assert [os.path.relpath(f, tmp_path) for f in linked] == [os.path.join("B", "Vendor", "Linked.vst3")]
    monkeypatch.setenv("SUBSTATION_VST3_PATH", os.pathsep.join([str(tmp_path / "A"), str(tmp_path / "B")]))
    assert search_paths() == [tmp_path / "A", tmp_path / "B"]
    # The user's own folders come after the standard ones, each folder once.
    assert search_paths([str(tmp_path / "C"), str(tmp_path / "a") + os.sep]) == [
        tmp_path / "A", tmp_path / "B", tmp_path / "C"]
    assert search_paths([str(tmp_path / "C")]) == [tmp_path / "A", tmp_path / "B", tmp_path / "C"]
    monkeypatch.setenv("SUBSTATION_VST3_PATH", "")
    assert search_paths() == []


def test_friendly_messages():
    assert _friendly("LoadLibraryW failed for path C:\\x\\A.vst3: A DLL initialization routine failed.\r\n\r\n") \
        == "Windows could not load it: A DLL initialization routine failed."
    assert _friendly("LoadLibraryW failed with error number: 126 for path C:\\a b\\B.vst3") \
        == "Windows could not load it: a file it needs is missing."
    assert _friendly("LoadLibraryW failed with error number: 5 for path x") == "Windows could not load it: error 5."
    assert _friendly("The plug-in crashed while loading.") == "The plug-in crashed while loading."
