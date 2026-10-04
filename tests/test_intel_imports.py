"""The intelligence layer's import rules, checked by walking its imports with
ast: it never imports the UI or QtWidgets; QtCore (and any PySide6) only in
intel/qt/; the engine (substation._engine) only in intel/qt/engine_facts.py.
So everything but qt/ is plain Python, testable without a window, and the
engine's names appear in one adapter."""

import ast
from pathlib import Path

import substation.intel

INTEL = Path(substation.intel.__file__).parent


def imports(path: Path) -> list[str]:
    """Every module a file imports, as absolute names."""
    # The package relative imports start from (a module's, or an __init__'s own).
    package = ("substation.intel." + ".".join(path.relative_to(INTEL).with_suffix("").parts)).rsplit(".", 1)[0]
    names = []
    for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
        if isinstance(node, ast.Import):
            names += [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom):
            if node.level:
                base = package.split(".")
                base = base[:len(base) - (node.level - 1)]
                module = ".".join(base + ([node.module] if node.module else []))
            else:
                module = node.module or ""
            names.append(module)
            names += [f"{module}.{alias.name}" for alias in node.names]
    return names


def test_the_layer_keeps_to_its_imports():
    files = sorted(INTEL.rglob("*.py"))
    assert len(files) > 10
    problems = []
    for path in files:
        where = path.relative_to(INTEL).as_posix()
        in_qt = where.startswith("qt/")
        for name in imports(path):
            if name == "substation.ui" or name.startswith("substation.ui."):
                problems.append(f"{where} imports the UI ({name})")
            if "QtWidgets" in name:
                problems.append(f"{where} imports QtWidgets")
            if name.startswith("PySide6") and not in_qt:
                problems.append(f"{where} imports {name} (Qt only in intel/qt/)")
            if name.startswith("substation._engine") and where != "qt/engine_facts.py":
                problems.append(f"{where} imports the engine (only qt/engine_facts.py may)")
    assert problems == []


def test_the_walk_sees_relative_imports():
    found = imports(INTEL / "qt" / "controller.py")
    assert "substation.audio.engine_bridge" in found and "substation.intel.ops" in found
    assert "PySide6.QtCore" in found
