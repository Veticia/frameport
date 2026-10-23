"""FramePort on the Frame itself links game data into the install instead of copying it (GitHub #433)."""
import os
from types import SimpleNamespace

from frameport.core.events import Reporter
from frameport.install import installer


def test_data_is_linked_on_the_frame_itself(tmp_path, monkeypatch):
    src = tmp_path / "QuestGames" / "Deadpool"
    src.mkdir(parents=True)
    obb = src / "main.1.com.x.obb"
    obb.write_bytes(b"x" * 10)
    remote = tmp_path / "Applications" / "com.x" / "incoming"
    items = [(obb, "obb/main.1.com.x.obb", 10)]
    local_frame = SimpleNamespace(target=SimpleNamespace(host="127.0.0.1"))
    monkeypatch.setattr("frameport.frame.local.on_frame", lambda: True)
    rest, linked = installer._link_local(local_frame, items, str(remote), Reporter())
    dest = remote / "obb" / "main.1.com.x.obb"
    assert (rest, linked) == ([], 10) and os.path.samefile(dest, obb)
    assert installer._link_local(local_frame, items, str(remote), Reporter()) == ([], 10)  # already there: done
    # a PC (or another host): uploaded as before
    pc = SimpleNamespace(target=SimpleNamespace(host="10.0.0.5"))
    assert installer._link_local(pc, items, str(remote), Reporter()) == (items, 0)
    monkeypatch.setattr("frameport.frame.local.on_frame", lambda: False)
    assert installer._link_local(local_frame, items, str(remote), Reporter()) == (items, 0)


def test_files_that_cant_be_linked_are_uploaded(tmp_path, monkeypatch):
    obb = tmp_path / "a.obb"
    obb.write_bytes(b"x")
    monkeypatch.setattr("frameport.frame.local.on_frame", lambda: True)

    def no_link(a, b):
        raise OSError(18, "Invalid cross-device link")
    monkeypatch.setattr(installer.os, "link", no_link)
    items = [(obb, "obb/a.obb", 1)]
    frame = SimpleNamespace(target=SimpleNamespace(host="127.0.0.1"))
    assert installer._link_local(frame, items, str(tmp_path / "in"), Reporter()) == (items, 0)
