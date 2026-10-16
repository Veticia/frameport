"""Headset-free OpenXR on this Windows PC for unattended tests: Monado's simulated HMD (docs/PC_ANDROID.md "Testing
without a headset").

A portable Monado for Windows (an official CI build of monado/monado, unpacked by hand, no installer) runs as
monado-service.exe with the simulated builder. Only processes FramePort starts get it, through the OpenXR loader's
XR_RUNTIME_JSON: the system's active OpenXR runtime (SteamVR) and SteamVR itself are never changed or started.
Monado's compositor accepts D3D11 clients (XR_KHR_D3D11_enable) and asks them for the GPU it renders on (the
discrete one), which is what AXRB's host bridge needs.

Off unless asked for: `--sim` on `frameport test --to pc`, or env FRAMEPORT_PC_XR_SIM=1.
"""

from __future__ import annotations

import contextlib
import os
import re
import time
from dataclasses import dataclass
from pathlib import Path

from ..core import winhost

ENV_FLAG = "FRAMEPORT_PC_XR_SIM"
PIPE = "monado_comp_ipc"  # Monado's IPC pipe: \\.\pipe\<its runtime dir = %TEMP%>\monado_comp_ipc
FPS = 72  # the Quest's usual rate

# Monado's own settings for the service. The simulated builder only runs when asked for; "ml2" controllers are the
# simulated ones whose bindings cover the Vive wand profile, so OpenXR apps that suggest Touch/Index/Vive bindings
# (AXRB does) get two tracked controllers ("simple" and "wmr" ones stay inactive for them).
SERVICE_ENV = {"SIMULATED_ENABLE": "1", "SIMULATED_LEFT": "ml2", "SIMULATED_RIGHT": "ml2",
               "XRT_COMPOSITOR_DEFAULT_FRAMERATE": str(FPS)}
# No window at all (Monado's "null" compositor): frames are paced and accepted, nothing is shown or capturable.
HEADLESS_ENV = {"XRT_COMPOSITOR_NULL": "1", "XRT_COMPOSITOR_NULL_FPS": str(FPS)}
# Monado's qwerty builder instead: its head reports POSITION_TRACKED (the simulated head is valid but never tracked),
# so AXRB 1.0.4's host bridge (the emulator backend, which only knows AXRB_TEST_SYNTHETIC_HMD=1 and applies it only
# to a head without valid flags) sets its LOCAL origin and the guest gets tracked head poses. Its controllers are
# WMR ones, which AXRB's suggested bindings don't cover: the hands stay inactive.
TRACKED_HEAD_ENV = {"SIMULATED_ENABLE": "0", "QWERTY_ENABLE": "1"}
HEAD_ENV_FLAG = "FRAMEPORT_PC_XR_SIM_HEAD"  # "tracked" / "simulated": overrides the automatic choice
WINDOW_TITLE = "Monado"  # its compositor window: "Monado (Windowed)" = the simulated HMD's display, both eyes


NOT_FOUND = "Monado for Windows was not found (see docs/PC_ANDROID.md \"Testing without a headset\")"


class XrSimError(RuntimeError):
    pass


def enabled(flag: bool | None = None) -> bool:
    """`--sim` given, or FRAMEPORT_PC_XR_SIM=1."""
    return bool(flag) or os.environ.get(ENV_FLAG, "").strip().lower() in ("1", "true", "yes", "on")


def _dev_dir() -> Path | None:
    local = winhost.env_path("LOCALAPPDATA") if winhost.available() else None
    return local / "FramePort-dev" if local else None


def monado_dir() -> Path | None:
    """The portable Monado (folder with openxr_monado.json and bin/monado-service.exe): FRAMEPORT_MONADO_DIR, else
    %LOCALAPPDATA%\\FramePort-dev\\monado (its `install` folder as unpacked from the CI artifact). Local path."""
    bases = [Path(os.environ["FRAMEPORT_MONADO_DIR"])] if os.environ.get("FRAMEPORT_MONADO_DIR") else []
    dev = _dev_dir()
    if not bases and dev:
        bases = [dev / "monado"]
    for base in bases:
        for d in (base, base / "install"):
            if (d / "openxr_monado.json").is_file() and (d / "bin" / "monado-service.exe").is_file():
                return d
    return None


def capture_exe() -> Path | None:
    """xr_sim_capture.exe (native/xrsimprobe, built by hand): FRAMEPORT_XR_SIM_CAPTURE, else next to the dev probe."""
    if os.environ.get("FRAMEPORT_XR_SIM_CAPTURE"):
        p = Path(os.environ["FRAMEPORT_XR_SIM_CAPTURE"])
        return p if p.is_file() else None
    dev = _dev_dir()
    p = dev / "xr-sim-probe" / "xr_sim_capture.exe" if dev else None
    return p if p and p.is_file() else None


def client_env(monado: Path | None = None) -> dict[str, str]:
    """What a process FramePort starts needs to use the simulator instead of the system's runtime."""
    monado = monado or monado_dir()
    if not monado:
        raise XrSimError(NOT_FOUND)
    # AXRB_TEST_SYNTHETIC_HMD=2 (AXRB's host bridge, test-only): a fixed standing head and a valid LOCAL origin.
    # Monado's simulated head has no POSITION_TRACKED bit, so the bridge never sets up its LOCAL origin and reports
    # every head pose in LOCAL with flags 0; VrApi games (The Climb 2) then never get a tracked head and only submit
    # empty frames.
    return {"XR_RUNTIME_JSON": winhost.to_windows(monado / "openxr_monado.json"), "AXRB_TEST_SYNTHETIC_HMD": "2"}


def tracked_head(auto: bool = False) -> bool:
    """Run Monado's qwerty head (TRACKED_HEAD_ENV): FRAMEPORT_PC_XR_SIM_HEAD=tracked|simulated, else `auto` (the
    caller's choice: games on AXRB's emulator backend, whose host bridge has no synthetic head for a valid pose)."""
    value = os.environ.get(HEAD_ENV_FLAG, "").strip().lower()
    if value in ("tracked", "qwerty"):
        return True
    if value in ("simulated", "sim"):
        return False
    return auto


def service_env(headless: bool = False, tracked: bool = False) -> dict[str, str]:
    return {**SERVICE_ENV, **(TRACKED_HEAD_ENV if tracked else {}), **(HEADLESS_ENV if headless else {})}


def _ps_quote(s: str) -> str:
    return "'" + str(s).replace("'", "''") + "'"


def ps_env(env: dict[str, str]) -> str:
    """PowerShell statements that set `env` for the processes the rest of the command starts."""
    return "".join(f"$env:{k} = {_ps_quote(v)}; " for k, v in env.items())


# AXRB's run script registers the game with SteamVR (steamvr_app_identity.py, an OpenVR utility client that starts
# SteamVR) whenever the system's active OpenXR runtime is SteamVR, whatever XR_RUNTIME_JSON says. Under the simulator
# that call is answered "failed" instead (the script only warns), so SteamVR is never started or written to.
NO_STEAMVR_SHIM = ("function python { if (\"$args\" -match 'steamvr_app_identity\\.py') "
                   "{ Write-Output 'FramePort xr-sim: SteamVR registration skipped'; "
                   "$global:LASTEXITCODE = 1; return }; & python.exe @args }; ")


def launcher_prefix(env: dict[str, str]) -> str:
    """PowerShell to put before a PC launcher call so the processes it starts use the simulator (empty without)."""
    if not env:
        return ""
    return ps_env(env) + (NO_STEAMVR_SHIM if "XR_RUNTIME_JSON" in env else "")


def service_command(monado: Path, log_win: str, headless: bool = False, tracked: bool = False) -> str:
    """The PowerShell command that runs monado-service.exe and waits for it; its log (Monado logs to stderr) goes to
    log_win, stdout next to it."""
    exe = winhost.to_windows(monado / "bin" / "monado-service.exe")
    return (ps_env(service_env(headless, tracked))
            + f"$p = Start-Process -FilePath {_ps_quote(exe)} -WindowStyle Hidden -PassThru -Wait "
            f"-RedirectStandardError {_ps_quote(log_win)} -RedirectStandardOutput {_ps_quote(log_win + '.out')}; "
            "exit $p.ExitCode")


def pipe_open() -> bool:
    """Monado's service is up and takes clients."""
    r = winhost.powershell(f"[bool]([System.IO.Directory]::GetFiles('\\\\.\\pipe\\') -like {_ps_quote('*' + PIPE)})",
                           timeout=30)
    return r.stdout.strip().lower().endswith("true")


def log_path() -> Path:
    from ..core.paths import user_data_dir

    return user_data_dir() / "logs" / "xr-sim-service.log"


@dataclass
class Service:
    pid: int | None  # the hidden PowerShell that holds monado-service.exe; None = one we didn't start
    log: Path
    monado: Path
    tracked: bool = False  # qwerty head (TRACKED_HEAD_ENV); False for a service we didn't start (unknown)
    capture_prefix: str | None = None  # Windows path prefix for AXRB's host bridge frame captures

    def env(self) -> dict[str, str]:
        env = client_env(self.monado)
        if self.capture_prefix:  # AXRB's opt-in readback of the layers the game submits (4 PPMs, 5 s apart)
            env.update({"AXRB_CAPTURE_PREFIX": self.capture_prefix, "AXRB_CAPTURE_FIRST_SEQUENCE": "300"})
        return env


_active: Service | None = None


def active_env() -> dict[str, str]:
    """XR_RUNTIME_JSON for Monado while a simulator started by `running()` is up in this process, else {}: the hook
    the PC launchers read (targets/pc_android.py)."""
    return _active.env() if _active else {}


def start(headless: bool = False, wait: float = 60, tracked: bool = False) -> Service:
    """Start Monado's service hidden (nothing inherited from FramePort) and wait until its pipe is open. A service
    that is already running is used as it is (and left running by stop())."""
    monado = monado_dir()
    if not monado:
        raise XrSimError(NOT_FOUND)
    log = log_path()
    log.parent.mkdir(parents=True, exist_ok=True)
    if pipe_open():
        return Service(None, log, monado)
    log.unlink(missing_ok=True)
    from . import axrb  # its WMI start: the service must not hold FramePort's (or WSL's interop) pipes

    pid = axrb.spawn_detached(service_command(monado, winhost.to_windows(log), headless, tracked))
    end = time.time() + wait
    while time.time() < end:
        if pipe_open():
            return Service(pid, log, monado, tracked)
        text = log.read_text(errors="replace") if log.is_file() else ""
        if re.search(r"Server exiting|ERROR \[p_create_system\]", text):
            break
        time.sleep(1)
    stop(Service(pid, log, monado))
    tail = (log.read_text(errors="replace") if log.is_file() else "").strip().splitlines()[-5:]
    raise XrSimError("Monado's service didn't start" + (": " + " | ".join(tail) if tail else ""))


def stop(service: Service) -> None:
    """End the service (with its PowerShell and cmd parents) if we started it."""
    if service.pid:
        winhost.run_win([winhost.system32("taskkill.exe"), "/PID", str(service.pid), "/T", "/F"], timeout=30)


def capture_dir() -> Path:
    from ..core.paths import user_data_dir

    return user_data_dir() / "logs" / "xr-sim"


@contextlib.contextmanager
def running(on: bool, headless: bool = False, reporter=None, captures: bool = True, tracked: bool = False):
    """Monado up for the `with` block when `on` (callers decide with enabled()), else nothing; active_env() names it
    meanwhile. With captures, AXRB's host bridge also saves a few frames of what the game submits to capture_dir().
    `tracked`: Monado's qwerty head instead of the simulated one (see tracked_head())."""
    global _active
    if not on:
        yield None
        return
    if reporter:
        reporter.stage("Starting the simulated headset (Monado)")
    service = start(headless, tracked=tracked)
    if captures:
        capture_dir().mkdir(parents=True, exist_ok=True)
        service.capture_prefix = winhost.to_windows(capture_dir()) + "\\" + time.strftime("%Y%m%d-%H%M%S-")
    _active = service
    if reporter:
        reporter.log(f"Monado: {service.monado} (log {service.log})"
                     + ("; head: qwerty (tracked)" if service.tracked else "")
                     + (f"; frame captures: {capture_dir()}" if captures else ""))
    try:
        yield service
    finally:
        _active = None
        stop(service)


def capture(out: Path, title: str = WINDOW_TITLE) -> Path:
    """Save Monado's compositor window (the simulated HMD's display: both eyes as the headset gets them) as a BMP.
    Needs the windowed compositor (not headless) and xr_sim_capture.exe."""
    exe = capture_exe()
    if not exe:
        raise XrSimError("xr_sim_capture.exe was not found (native/xrsimprobe)")
    out.parent.mkdir(parents=True, exist_ok=True)
    r = winhost.run_win([str(exe), winhost.to_windows(out), title], timeout=30)
    if r.returncode != 0 or not out.is_file():
        raise XrSimError((r.stdout + r.stderr).strip() or "capture failed")
    return out
