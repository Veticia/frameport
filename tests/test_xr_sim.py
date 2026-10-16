"""Headset-free OpenXR on the PC (tools/xr_sim.py: Monado's simulated HMD) against fakes; no Windows needed."""
import subprocess
from pathlib import Path

import pytest

from frameport.core import winhost
from frameport.core.events import Reporter
from frameport.tools import axrb, xr_sim


def fake_monado(base: Path) -> Path:
    d = base / "install"
    (d / "bin").mkdir(parents=True)
    (d / "bin" / "monado-service.exe").write_bytes(b"MZ")
    (d / "openxr_monado.json").write_text('{"runtime": {"library_path": "bin\\\\openxr_monado.dll"}}')
    return d


@pytest.fixture
def win(monkeypatch, tmp_path):
    monkeypatch.setattr(winhost, "available", lambda: True)
    monkeypatch.setattr(winhost, "to_windows", lambda p: "W:" + str(p).replace("/", "\\"))
    monkeypatch.setattr(winhost, "env_path", lambda name: tmp_path / "Local" if name == "LOCALAPPDATA" else None)
    monkeypatch.delenv("FRAMEPORT_MONADO_DIR", raising=False)
    monkeypatch.delenv(xr_sim.ENV_FLAG, raising=False)
    monkeypatch.setattr(xr_sim, "_active", None)
    return tmp_path


def test_enabled_by_flag_or_env(monkeypatch):
    monkeypatch.delenv(xr_sim.ENV_FLAG, raising=False)
    assert not xr_sim.enabled() and not xr_sim.enabled(False) and xr_sim.enabled(True)
    monkeypatch.setenv(xr_sim.ENV_FLAG, "1")
    assert xr_sim.enabled()


def test_finds_the_portable_monado(win, monkeypatch):
    assert xr_sim.monado_dir() is None
    d = fake_monado(win / "Local" / "FramePort-dev" / "monado")
    assert xr_sim.monado_dir() == d
    other = fake_monado(win / "elsewhere")
    monkeypatch.setenv("FRAMEPORT_MONADO_DIR", str(other))  # the folder with `install`, or `install` itself
    assert xr_sim.monado_dir() == other
    assert xr_sim.client_env() == {"XR_RUNTIME_JSON": "W:" + str(other / "openxr_monado.json").replace("/", "\\")}


def test_missing_monado_is_explained(win):
    with pytest.raises(xr_sim.XrSimError, match="Testing without a headset"):
        xr_sim.start()


def test_service_command_sets_only_monados_settings(win):
    d = fake_monado(win / "m")
    cmd = xr_sim.service_command(d, r"C:\logs\svc.log")
    assert "$env:SIMULATED_ENABLE = '1'" in cmd and "$env:SIMULATED_LEFT = 'ml2'" in cmd
    assert "$env:XRT_COMPOSITOR_DEFAULT_FRAMERATE = '72'" in cmd and "XRT_COMPOSITOR_NULL" not in cmd
    assert "monado-service.exe'" in cmd and r"-RedirectStandardError 'C:\logs\svc.log'" in cmd
    assert "XR_RUNTIME_JSON" not in cmd  # the service needs no loader; nothing about SteamVR either
    assert "steam" not in cmd.lower()
    assert "$env:XRT_COMPOSITOR_NULL = '1'" in xr_sim.service_command(d, "x", headless=True)


def test_launcher_prefix_and_steamvr_shim():
    assert xr_sim.launcher_prefix({}) == ""
    pre = xr_sim.launcher_prefix({"XR_RUNTIME_JSON": r"C:\M\openxr_monado.json", "A": "it's"})
    assert pre.startswith(r"$env:XR_RUNTIME_JSON = 'C:\M\openxr_monado.json'; $env:A = 'it''s'; ")
    assert "steamvr_app_identity" in pre and "function python" in pre
    assert "steamvr" not in xr_sim.launcher_prefix({"A": "1"}).lower()


def test_running_starts_registers_and_stops(win, monkeypatch):
    fake_monado(win / "Local" / "FramePort-dev" / "monado")
    monkeypatch.setenv("FRAMEPORT_HOME", str(win / "home"))
    calls, pipe = [], iter([False, False, True])
    monkeypatch.setattr(xr_sim, "pipe_open", lambda: next(pipe))
    monkeypatch.setattr(axrb, "spawn_detached", lambda cmd: calls.append(("spawn", cmd)) or 4242)
    monkeypatch.setattr(xr_sim.time, "sleep", lambda s: None)
    monkeypatch.setattr(winhost, "system32", lambda exe: exe)
    monkeypatch.setattr(winhost, "run_win", lambda args, timeout=30: calls.append(("run", args))
                        or subprocess.CompletedProcess(args, 0, "", ""))
    assert xr_sim.active_env() == {}
    with xr_sim.running(False):
        assert xr_sim.active_env() == {} and not calls
    with xr_sim.running(True, reporter=Reporter()) as svc:
        assert svc.pid == 4242
        env = xr_sim.active_env()
        assert env["XR_RUNTIME_JSON"].endswith(r"monado\install\openxr_monado.json")
        assert env["AXRB_CAPTURE_PREFIX"].startswith("W:") and env["AXRB_CAPTURE_FIRST_SEQUENCE"] == "300"
    assert xr_sim.active_env() == {}
    assert calls[0][0] == "spawn" and "SIMULATED_ENABLE" in calls[0][1]
    assert calls[-1] == ("run", ["taskkill.exe", "/PID", "4242", "/T", "/F"])


def test_a_running_service_is_used_and_left_alone(win, monkeypatch):
    fake_monado(win / "Local" / "FramePort-dev" / "monado")
    monkeypatch.setenv("FRAMEPORT_HOME", str(win / "home"))
    monkeypatch.setattr(xr_sim, "pipe_open", lambda: True)
    monkeypatch.setattr(axrb, "spawn_detached", lambda cmd: pytest.fail("started a second service"))
    monkeypatch.setattr(winhost, "run_win", lambda *a, **k: pytest.fail("stopped someone else's service"))
    with xr_sim.running(True, captures=False):
        assert set(xr_sim.active_env()) == {"XR_RUNTIME_JSON"}


def test_failed_start_is_cleaned_up(win, monkeypatch):
    fake_monado(win / "Local" / "FramePort-dev" / "monado")
    monkeypatch.setenv("FRAMEPORT_HOME", str(win / "home"))
    monkeypatch.setattr(xr_sim, "pipe_open", lambda: False)
    killed = []

    def spawn(cmd):
        xr_sim.log_path().write_text("ERROR [p_create_system] no system\nServer exiting: '-1'\n")
        return 7

    monkeypatch.setattr(axrb, "spawn_detached", spawn)
    monkeypatch.setattr(winhost, "system32", lambda exe: exe)
    monkeypatch.setattr(winhost, "run_win", lambda args, timeout=30: killed.append(args)
                        or subprocess.CompletedProcess(args, 0, "", ""))
    with pytest.raises(xr_sim.XrSimError, match="Server exiting"):
        xr_sim.start()
    assert killed and killed[0][1:3] == ["/PID", "7"]


def test_pc_launch_test_passes_the_simulator_on(tmp_path, monkeypatch, win):
    from frameport.targets import pc_android

    commands = []
    monkeypatch.setattr(axrb, "spawn_detached", lambda cmd: commands.append(cmd) or 1)
    monkeypatch.setattr(axrb, "pid_running", lambda pid: False)
    monkeypatch.setattr(axrb, "emulator_online", lambda *a, **k: False)
    monkeypatch.setattr(axrb, "log_dir", lambda *a, **k: tmp_path / "logs")
    monkeypatch.setattr(pc_android, "game_logs", lambda *a, **k: {})
    dep = {"package": "com.x.game", "title": "Game", "activity": "com.x.game/.Main",
           "launcher_win": r"C:\fp\fp-axrb-run.ps1"}
    pc_android.launch_test(dep, Reporter(), seconds=1)
    assert "XR_RUNTIME_JSON" not in commands[-1]  # no simulator: the launcher call is unchanged
    monkeypatch.setattr(xr_sim, "_active", xr_sim.Service(None, tmp_path / "l", fake_monado(tmp_path / "m")))
    pc_android.launch_test(dep, Reporter(), seconds=1)
    assert commands[-1].startswith("$env:XR_RUNTIME_JSON = 'W:") and "steamvr_app_identity" in commands[-1]
    assert commands[-1].index("XR_RUNTIME_JSON") < commands[-1].index("fp-axrb-run.ps1")
    pc_android.launch_test(dep, Reporter(), seconds=1, extra_env={"FOO": "1"})
    assert commands[-1].startswith("$env:FOO = '1'; ") and "XR_RUNTIME_JSON" not in commands[-1]


def test_running_launch_tests_save_the_simulated_headset_view(tmp_path, monkeypatch, win):
    from frameport.targets import pc_android

    shots = []
    monkeypatch.setattr(xr_sim, "capture_exe", lambda: tmp_path / "cap.exe")
    monkeypatch.setattr(xr_sim, "capture_dir", lambda: tmp_path / "caps")
    monkeypatch.setattr(xr_sim, "capture", lambda out: shots.append(out) or out)
    assert pc_android.sim_capture({}, "com.x.game", Reporter()) is None and not shots  # no simulator
    path = pc_android.sim_capture({"XR_RUNTIME_JSON": "W:\\m.json"}, "com.x.game", Reporter())
    assert shots == [path] and path.parent == tmp_path / "caps" and path.name.endswith("-com.x.game-headset.bmp")
    monkeypatch.setattr(xr_sim, "capture", lambda out: (_ for _ in ()).throw(xr_sim.XrSimError("no window")))
    assert pc_android.sim_capture({"XR_RUNTIME_JSON": "W:\\m.json"}, "com.x.game", Reporter()) is None
    monkeypatch.setattr(xr_sim, "capture_exe", lambda: None)
    assert pc_android.sim_capture({"XR_RUNTIME_JSON": "W:\\m.json"}, "com.x.game", Reporter()) is None
