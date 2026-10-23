"""Files location discovery and capabilities, without a device or GUI."""
from types import SimpleNamespace

import pytest

from frameport.install import files
from frameport.ui.views.files import FilesView, drive_locations, game_location


def test_drive_locations():
    assert drive_locations([]) == []
    locs = drive_locations([
        {"path": "/home/x", "internal": True},
        {"path": "/media/SD", "label": "SD card", "free_bytes": 123},
        {"path": "/media/USB", "read_only": True, "usable": False},
        {"path": "/games", "reason": "mounted read-only"},
        {"path": "/fat", "usable": False, "reason": "exfat can't hold game data"},
        {},
    ])
    assert [x["path"] for x in locs] == ["/media/SD", "/media/USB", "/games", "/fat"]
    assert locs[0]["label"] == "SD card" and locs[0]["free_bytes"] == 123
    assert [x["read_only"] for x in locs] == [False, True, True, False]
    for loc in locs:
        assert files.inside(loc["path"], "mods") == loc["path"] + "/mods"
        with pytest.raises(ValueError):
            files.inside(loc["path"], "../outside")


@pytest.mark.parametrize("kind", ["pcvr", "rift", "linux"])
def test_game_install_locations(kind):
    dep = {"package": "game", "kind": kind}
    loc = game_location(dep, "Game", "/home/x")
    assert loc["path"] == "/home/x/Applications/quest-frame/game"
    assert not loc["android"]
    dep["drive"] = {"path": "/media/SD", "internal": False}
    assert game_location(dep, "Game", "/home/x")["path"] == "/media/SD/FramePort/game"
    dep["base"] = "/custom/deployed/game"
    loc = game_location(dep, "Game", "/home/x")
    assert loc["path"] == dep["base"]
    with pytest.raises(ValueError):
        files.inside(loc["path"], "/custom/deployed/other")
    assert "path" not in game_location({"package": "q", "kind": "quest"}, "Quest", "/home/x")


def test_background_discovery_cached_and_game_open_uses_base(monkeypatch):
    calls = []
    frame = SimpleNamespace(home="/home/x")
    app = SimpleNamespace(target=SimpleNamespace(frame=frame, drives=lambda: calls.append("drives") or [
        {"path": "/media/SD", "read_only": True}]), frame_info={"installed": [
        {"package": "linux", "kind": "linux", "base": "/media/SD/FramePort/linux"}]},
        run_bg=lambda fn, *args: fn(*args))
    v = FilesView(app)
    monkeypatch.setattr(v, "_render_locations", lambda: None)
    monkeypatch.setattr(v, "load", lambda: None)
    monkeypatch.setattr(files, "storage_targets", lambda f, *a: calls.append("storage") or [
        {"id": "videos", "path": "/home/x/Videos", "android": "/sdcard/Movies"}])
    v._load_locations("linux")
    assert v.loc["path"] == "/media/SD/FramePort/linux"
    assert v.loc["read_only"] and all(c.disabled for c in v.toolbar.controls[:3])
    v._load_locations(None)
    assert calls == ["storage", "drives"]
    with pytest.raises(ValueError):
        v.cd("../other")


def test_read_only_actions_and_quest_link_protection():
    v = FilesView(SimpleNamespace())
    v.loc = {"id": "drive:/sd", "path": "/sd", "read_only": True}
    v.path = "/sd"
    e = files.Entry("link", "/sd/link", True, 0, 0, link=True)
    v.entries = [e]
    assert not v._protected(e) and v._selectable(e)
    labels = [a[0] for a in v.menu_actions(e) if a]
    assert "Download to this PC…" in labels
    assert "Rename…" not in labels and "Delete…" not in labels
    assert "Upload files…" not in [a[0] for a in v.menu_actions(None) if a]
    # Handler guards also cover OS drops and stale callbacks.
    v.upload([])
    v.new_folder()
    v.rename(e)
    v.delete([e])
    v.loc = {"id": "app:q", "path": "/sd", "android": "/sdcard"}
    assert v._protected(e)
    v.loc["android"] = ""
    assert not v._protected(e)


def test_installed_games_include_all_kinds_sorted(monkeypatch):
    from frameport.core import library

    monkeypatch.setattr(library, "game", lambda pkg: {"title": "Alpha"} if pkg == "pc" else None)
    v = FilesView(SimpleNamespace(frame_info={"installed": [
        {"package": "q", "kind": "quest", "title": "Zulu"},
        {"package": "linux", "kind": "linux", "title": "Beta"},
        {"package": "pc", "kind": "pcvr", "title": "Fallback"},
    ]}))
    assert v._installed_games() == [("pc", "Alpha"), ("linux", "Beta"), ("q", "Zulu")]


def test_empty_drives_cached(monkeypatch):
    calls = []
    v = FilesView(SimpleNamespace(target=SimpleNamespace(frame=SimpleNamespace(home="/home/x"),
                                                     drives=lambda: calls.append(1) or []),
                                 frame_info={"installed": []}))
    monkeypatch.setattr(v, "_render_locations", lambda: None)
    monkeypatch.setattr(files, "storage_targets", lambda f: [])
    v._load_locations(None)
    v._load_locations(None)
    assert v.drives == [] and len(calls) == 1


def test_installed_games_are_grouped_by_kind():
    from frameport.ui.views.files import game_section

    assert [game_section(k) for k in ("quest", None, "rift", "pcvr", "linux")] == \
        ["quest", "quest", "pcvr", "pcvr", "linux"]


def test_back_to_a_folder_restores_its_scroll_and_backspace_goes_up(monkeypatch):
    """GitHub #411: going back up jumped to the top of the folder's list; Backspace now goes to the folder above."""
    v = FilesView(SimpleNamespace())
    loaded = []
    monkeypatch.setattr(v, "load", lambda: loaded.append((v.path, v._restore)))
    v.loc, v.path = {"id": "drive:/sd", "path": "/sd"}, "/sd"
    v._remember_scroll(SimpleNamespace(pixels=640.0))
    v.cd("/sd/movies")
    v._remember_scroll(SimpleNamespace(pixels=80.0))
    v.cd("/sd/movies/2026")
    assert loaded[-1] == ("/sd/movies/2026", None)  # into a folder: from the top
    assert v.go_up() and loaded[-1] == ("/sd/movies", 80.0)
    v.cd("/sd")  # a crumb two levels up
    assert loaded[-1] == ("/sd", 640.0)
    assert not v.go_up()  # the location's top
