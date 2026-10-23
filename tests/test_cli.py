"""Command line: error output and exit codes."""
import pytest


def test_errors_are_one_line_on_stderr(monkeypatch, capsys):
    from paramiko.ssh_exception import NoValidConnectionsError

    from frameport import cli

    def boom():
        raise NoValidConnectionsError({("10.0.0.9", 22): OSError("refused")})

    monkeypatch.delenv("FRAMEPORT_DEBUG", raising=False)
    monkeypatch.setattr(cli, "app", boom)
    with pytest.raises(SystemExit) as e:
        cli.main()
    err = capsys.readouterr().err
    assert e.value.code == 1 and err.startswith("Error: Can't reach your Frame") and "Traceback" not in err
    monkeypatch.setenv("FRAMEPORT_DEBUG", "1")
    with pytest.raises(NoValidConnectionsError):
        cli.main()


@pytest.mark.parametrize("mode", ["now", "later", "off"])
def test_steam_library_setting_cli(monkeypatch, mode):
    from types import SimpleNamespace

    from typer.testing import CliRunner

    from frameport import cli
    from frameport.core import library

    calls = []
    frame = SimpleNamespace(agent=lambda command, **kw: calls.append((command, kw)) or {})
    monkeypatch.setattr(cli, "_target", lambda _: SimpleNamespace(connect=lambda: SimpleNamespace(frame=frame)))
    result = CliRunner().invoke(cli.app, ["frame", "steam-library", mode])
    assert result.exit_code == 0, result.output
    assert library.setting("install.steam_library") == mode
    assert calls == [("library_mode", {"mode": mode})]


def test_restart_steam_cli_confirmation(monkeypatch):
    from types import SimpleNamespace

    from typer.testing import CliRunner

    from frameport import cli

    calls = []
    def agent(command, **kw):
        calls.append(kw)
        return {} if kw.get("confirm") else {"confirmation_required": True}
    frame = SimpleNamespace(agent=agent)
    monkeypatch.setattr(cli, "_target", lambda _: SimpleNamespace(connect=lambda: SimpleNamespace(frame=frame)))
    result = CliRunner().invoke(cli.app, ["frame", "restart-steam"], input="n\n")
    assert result.exit_code != 0 and calls == [{"confirm": False}]
    calls.clear()
    result = CliRunner().invoke(cli.app, ["frame", "restart-steam"], input="y\n")
    assert result.exit_code == 0 and calls == [{"confirm": False}, {"confirm": True}]


def test_steam_library_defaults_to_later_on_the_frame(monkeypatch):
    """GitHub #425: with Now, FramePort on the Frame waited for itself to close before Steam could restart."""
    from frameport.core import library
    from frameport.install import installer

    monkeypatch.setattr("frameport.frame.local.on_frame", lambda: True)
    assert installer.steam_library_mode() == "later"
    monkeypatch.setattr("frameport.frame.local.on_frame", lambda: False)
    assert installer.steam_library_mode() == "now"
    library.set_setting("install.steam_library", "now")
    monkeypatch.setattr("frameport.frame.local.on_frame", lambda: True)
    assert installer.steam_library_mode() == "now"  # a choice the user made wins
