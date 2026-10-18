"""Save controls built headlessly: eligibility and dispatch, without starting Flet."""
from types import SimpleNamespace

import pytest

from frameport.ui import components as C
from frameport.ui.views.game import GameView


@pytest.mark.parametrize("kind,connected,state,visible", [
    (None, True, "installed", True),
    (None, True, "outdated", True),
    (None, True, "missing", False),
    (None, False, "installed", False),
    ("rift", True, "installed", False),
    ("linux", True, "installed", False),
])
def test_game_backup_action(kind, connected, state, visible, monkeypatch):
    clicked = []
    app = SimpleNamespace(
        jobs=SimpleNamespace(busy_with=lambda _: None),
        frame_state="connected" if connected else "offline", frame_info={},
        play_options=lambda _: [], install_options=lambda _: [], game_actions=lambda *a, **kw: [],
        backup_saves=clicked.append,
    )
    view = GameView.__new__(GameView)
    view.app, view.g, view.package = app, {"package": "com.example.game", "kind": kind}, "com.example.game"
    monkeypatch.setattr(C, "install_state", lambda *args: state)
    monkeypatch.setattr(C, "is_media_player", lambda _: False)
    buttons = []
    original = C.secondary

    def capture(label, icon=None, on_click=None, *args, **kwargs):
        buttons.append((label, on_click))
        return original(label, icon, on_click, *args, **kwargs)

    monkeypatch.setattr(C, "secondary", capture)
    view.actions()
    backup = [fn for label, fn in buttons if label == "Back up saves"]
    assert bool(backup) == visible
    if visible:
        backup[0](None)
        assert clicked == [view.package]


def test_settings_picker_is_awaited_and_reset_preserves_backups(tmp_path, monkeypatch):
    import asyncio

    import flet as ft

    from frameport.core import library
    from frameport.install import saves
    from frameport.ui.views.settings import SettingsView

    cloud = tmp_path / "cloud"
    calls = []

    class Picker:
        async def get_directory_path(self, **kwargs):
            calls.append("awaited")
            return str(cloud)

    monkeypatch.setattr(ft, "FilePicker", Picker)
    app = SimpleNamespace(run_bg=lambda fn: fn(), toast=lambda *a, **kw: pytest.fail(str(a)),
                          _sync_save_snapshots=lambda: calls.append("sync"))
    view = SettingsView(app)
    control = view.save_backups()
    folder_row = control.controls[2]
    asyncio.run(folder_row.controls[0].on_click(None))
    assert calls == ["awaited"] and saves.root_dir() == cloud
    folder_row.controls[1].on_click(None)
    assert saves.root_dir() == saves.default_root()
    assert cloud.exists()
    assert library.setting("saves.auto", False) is False
    switch = control.controls[4].controls[0].controls[1]
    switch.value = True
    switch.on_change(SimpleNamespace(control=switch))
    assert library.setting("saves.auto") is True and calls[-1] == "sync"
