"""A packaged app's own file list (frameport.core.bundle_files, GitHub #476)."""
from pathlib import Path

from frameport.core import bundle_files


def _package(tmp_path: Path) -> Path:
    pkg = tmp_path / "app/frameport"
    (pkg / "patches/frame").mkdir(parents=True)
    (pkg / "__init__.pyc").write_bytes(b"x" * 10)
    (pkg / "patches/frame/langpack.pyc").write_bytes(b"x" * 20)
    (pkg / "_data").mkdir()
    (pkg / "_data/tool.exe").write_bytes(b"unsigned")
    assert bundle_files.write(pkg) == 3
    return pkg


def test_no_list_means_nothing_to_check(tmp_path):
    assert bundle_files.check(tmp_path) is None


def test_matching_install_is_clean(tmp_path):
    pkg = _package(tmp_path)
    (pkg / "_data/tool.exe").write_bytes(b"signed later: other size")  # signing after packaging is fine
    result = bundle_files.check(pkg)
    assert result.stale == [] and result.mismatched == [] and not result.broken


def test_leftovers_of_another_version_are_removed(tmp_path):
    pkg = _package(tmp_path)
    old = pkg / "patches/frame/removed_patch.pyc"
    old.write_bytes(b"old")
    result = bundle_files.check(pkg)
    assert result.stale == ["patches/frame/removed_patch.pyc"] == result.removed
    assert not old.exists() and not result.broken and not bundle_files.is_stale(old, pkg)


def test_mixed_versions_are_reported_and_left_alone(tmp_path):
    pkg = _package(tmp_path)
    (pkg / "patches/frame/langpack.pyc").write_bytes(b"older build")  # the update didn't replace it
    extra = pkg / "patches/frame/ovr_org_scoped_id.pyc"
    extra.write_bytes(b"new")
    result = bundle_files.check(pkg)
    assert result.mismatched == ["patches/frame/langpack.pyc"] and result.broken
    assert extra.exists() and result.removed == []  # the list may not belong to the code: delete nothing
    assert bundle_files.is_stale(extra, pkg)  # but never import it


def test_missing_module_is_reported(tmp_path):
    pkg = _package(tmp_path)
    (pkg / "__init__.pyc").unlink()
    assert bundle_files.check(pkg).mismatched == ["__init__.pyc"]


def test_patch_loader_skips_stale_modules(monkeypatch):
    import importlib

    from frameport.patches import base, frame

    folder = Path(frame.__path__[0])
    name = sorted(p.stem for p in folder.glob("*.py") if p.stem != "__init__")[0]
    monkeypatch.setattr(bundle_files, "_stale", {f"patches/frame/{name}.py"})
    imported = []
    monkeypatch.setattr(importlib, "import_module", lambda n, *a: imported.append(n))
    base._import_modules()
    assert imported and f"{frame.__name__}.{name}" not in imported
