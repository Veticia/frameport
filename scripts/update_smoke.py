#!/usr/bin/env python3
"""CI smoke test of FramePort's self-update on this OS, with the archive this job just built:

    python scripts/update_smoke.py FramePort-<os>.<zip|tar.gz>

It unpacks the archive twice (an "installed" copy, plus an older-looking copy of it with stale files), then runs the
real update path: frameport.updates._extract (ditto on macOS) + the real swap script (PowerShell on Windows, /bin/sh
elsewhere), without relaunching. It checks the installed copy now has the new files and none of the old version's
leftovers, and on Windows that the signer check sees the same certificate on both copies.

Windows also runs the update of GitHub #476 first: a program file stays locked, so the copy fails half-way; the
rollback must leave exactly the old version (no file the new version added, every old file back), and a user's own
file next to FramePort.exe is never touched. Exits non-zero on any failure.
"""
from __future__ import annotations

import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))


def _lock(path: Path):
    """Windows: open `path` for reading and let others only read it, like a program that holds it open: the backup
    copy can read it, Copy-Item can't write over it."""
    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                wintypes.DWORD, wintypes.HANDLE]
    # GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING
    handle = k32.CreateFileW(str(path), 0x80000000, 1, None, 3, 0, None)
    if handle in (None, wintypes.HANDLE(-1).value):
        raise OSError(ctypes.get_last_error(), f"couldn't lock {path}")
    return lambda: k32.CloseHandle(handle)


def _run(updates, new: Path, installed: Path, platform: str, log_file: Path) -> str:
    """The same launch as the app (detached; apply() checks the script started), then wait for it to finish."""
    start = log_file.stat().st_size if log_file.exists() else 0
    script = updates.apply(new, installed, relaunch=False, pid=999999, platform=platform)
    log = ""
    for _ in range(240):
        log = log_file.read_bytes()[start:].decode(errors="replace") if log_file.exists() else ""
        if "not relaunching" in log:
            break
        time.sleep(0.5)
    print(script.read_text()[:300], "...\n--- update.log ---\n" + log)
    return log


def main() -> int:
    import logging

    logging.basicConfig(level=logging.INFO)
    archive = Path(sys.argv[1]).resolve()
    home = Path(tempfile.mkdtemp(prefix="fp-update-smoke-"))
    os.environ["FRAMEPORT_HOME"] = str(home / "data")
    from frameport import updates

    platform = sys.platform
    new = updates._extract(archive, home / "staged", platform)
    installed_parent = home / "Programs"
    installed_parent.mkdir()
    installed = installed_parent / new.name
    old = updates._extract(archive, home / "old", platform)
    shutil.move(str(old), str(installed))
    stale = installed / ("Contents/stale.txt" if platform == "darwin" else "stale.txt")
    stale.write_text("from the old version")
    marker = new / ("Contents/new-version.txt" if platform == "darwin" else "new-version.txt")
    marker.write_text("new")
    sub = next(p for p in sorted(marker.parent.iterdir()) if p.is_dir() and not p.is_symlink())
    nested = sub / "new-version.txt"  # files in existing subfolders must be replaced too (Windows merges folders)
    nested.write_text("new")
    # a module only the old version has, in the bundle's own folders (GitHub #476: Python imports whatever it finds)
    leftover = installed / sub.relative_to(new) / "old_only_module.pyc"
    leftover.write_text("old")
    if platform == "win32" and os.environ.get("HAS_CERT") == "true":
        a = updates._signer_thumbprint(installed / "FramePort.exe")
        b = updates._signer_thumbprint(new / "FramePort.exe")
        print(f"signer thumbprints: installed {a} new {b}")
        assert a and a == b, "signature check failed"
    log_file = home / "data/logs/update.log"
    installed_nested = installed / nested.relative_to(new)
    if platform == "win32":
        # a file copied after the app folder is held open: the copy fails half-way and is rolled back
        last_dir = sorted((p for p in new.iterdir() if p.is_dir()), key=lambda p: p.name.lower())[-1]
        held = next(p for p in sorted((installed / last_dir.name).rglob("*"))
                    if p.is_file() and p.suffix.lower() not in (".exe", ".dll", ".pyd"))
        print(f"holding {held.relative_to(installed)} open")
        release = _lock(held)
        try:
            log = _run(updates, new, installed, platform, log_file)
        finally:
            release()
        assert "update FAILED" in log, "the update should have failed on the locked file"
        assert not installed_nested.exists(), "a file the new version added was left after the rollback (#476)"
        assert leftover.read_text() == "old", "the rollback didn't bring back the old version's file"
        assert stale.read_text() == "from the old version", "a file next to FramePort.exe was touched"
    log = _run(updates, new, installed, platform, log_file)
    installed_marker = installed / marker.relative_to(new)
    assert installed_marker.read_text() == "new", "the new files weren't installed"
    assert installed_nested.read_text() == "new", "a subfolder wasn't updated"
    assert "installed" in log and "FAILED" not in log, "the update script reported a failure"
    assert not leftover.exists(), "a module of the old version is still there (#476)"
    exe = {"win32": installed / "FramePort.exe", "darwin": installed / "Contents/MacOS"}.get(platform,
                                                                                         installed / "FramePort")
    assert exe.exists(), f"{exe} missing after the update"
    if platform == "win32":  # Windows copies over the old files (the zip has no folder of its own)
        assert stale.read_text() == "from the old version", "a file next to FramePort.exe was removed"
        assert (home / "previous" / leftover.relative_to(installed)).exists(), "the removed file isn't kept"
    else:  # others swap the whole folder
        assert not stale.exists(), "the old version's files are still there"
    print("update smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
