"""Quest games on this Windows PC through AXRB (tools/axrb.py): the "This PC" target's Android half.

Each game runs in one of AXRB's two backends (`choose_backend`): direct mode (AXRB-Emulatorless: axrb-direct.exe,
no emulator, Vulkan only) for Vulkan games, AXRB's Android emulator for the rest; the recipe's `pc.axrb_backend`
(or `frameport install --to pc --axrb-backend`) overrides it.

An install sets that AXRB up when needed (its installer + its Android runtime, downloaded only now) and records
<data>/pc/<pkg>/deployment.json with kind "android" and the backend. The game's build for this PC is OVRPort's
conversion without the Steam Frame fixes, signed with the game's key.
- emulator: the emulator starts, the build is installed with adb, its OBB data pushed.
- direct: no Android to install into: the build is copied to %LOCALAPPDATA%\\FramePort\\axrb\\games\\<pkg>, its
  data files are listed (Android path -> Windows file, mounted read-only by AXRB at the game's start) the way AXRB's
  launcher lists them, and the launch activity comes from the APK's manifest.
Play goes through a Steam shortcut that runs FramePort's launcher script for the backend, which hands the game to
AXRB's own run script (run_windows_game.ps1 / run_direct_game.ps1). See docs/PC_ANDROID.md.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import time
import zipfile
from pathlib import Path

from ..core import winhost
from ..core.events import Reporter
from ..core.models import Recipe, data_manifest
from ..tools import axrb, xr_sim

KIND = "android"
OBB_ROOT = "/sdcard/Android/obb"
BACKEND_PATCH = "pc.axrb_backend"


def powershell_win() -> str:
    return axrb.powershell_win()


def shortcut_fields(dep: dict) -> tuple[str, str, str]:
    """(Exe, StartDir, LaunchOptions) of the Steam shortcut: powershell.exe running FramePort's AXRB launcher."""
    launcher = dep["launcher_win"]
    start = '"' + launcher.rsplit("\\", 1)[0] + '\\"'
    return f'"{dep["powershell_win"]}"', start, axrb.launcher_args(launcher, dep["package"], dep["activity"],
                                                                     dep["title"], dep.get("flags") or ())


# ------------------------------------------------------------------------------------------ backend choice
def backend_params(recipe: Recipe | None) -> dict:
    return dict(((recipe.patches if recipe else {}) or {}).get(BACKEND_PATCH) or {})


def choose_backend(recipe: Recipe | None, graphics: str = "", override: str | None = None) -> str:
    """Which AXRB runs the game: `override` (CLI), else the recipe's pc.axrb_backend, else direct mode for Vulkan
    games and the emulator for OpenGL ES ones (direct mode has no GLES renderer; "unknown" counts as GLES)."""
    for choice in (override, backend_params(recipe).get("backend")):
        if choice:
            if choice not in axrb.BACKENDS:
                raise ValueError(f"unknown AXRB backend {choice!r} (direct or emulator)")
            return choice
    return axrb.DIRECT if (graphics or "").startswith("Vulkan") else axrb.EMULATOR


def launch_flags(backend: str, params: dict, profile: dict | None = None) -> list[str]:
    """The launcher switches for a game: the recipe's pc.axrb_backend parameters, plus AXRB's own compatibility
    profile's precomposition. AXRB's legacy-driver/Quest-identity options are only taken from the recipe (FramePort
    gives AXRB an OVRPort build, which doesn't need them)."""
    profile = profile or {}
    flags = []
    if backend == axrb.DIRECT:
        translator = params.get("translator") or ""
        if translator and translator not in axrb.TRANSLATORS:
            raise ValueError(f"unknown AXRB translator {translator!r} ({', '.join(axrb.TRANSLATORS)})")
        if translator and translator != axrb.TRANSLATORS[0]:
            flags += ["-Translator", translator]
    if params.get("precompose") or profile.get("precomposeProjectionLayers"):
        flags.append("-PrecomposeProjectionLayers")
    if backend == axrb.DIRECT:
        legacy = bool(params.get("legacy_oculus_driver"))
        if legacy:
            flags.append("-LegacyOculusDriver")
        if params.get("quest_identity"):
            flags.append("-QuestIdentity")
        if params.get("controllers_only") and legacy:  # AXRB applies it through the legacy driver only
            flags.append("-ControllersOnly")
    return flags


def _analysis(package: str) -> dict:
    try:
        from ..core import library

        return (library.game(package) or {}).get("analysis") or {}
    except Exception:  # noqa: BLE001 - no library (tests, a test build): automatic choice from the APK alone
        return {}


# ------------------------------------------------------------------------------------------ emulator (adb)
def _adb_ok(r: subprocess.CompletedProcess, what: str) -> str:
    text = (r.stdout + r.stderr).strip()
    if r.returncode != 0 or "Failure" in text or "Error" in text and "Success" not in text:
        if "INSTALL_FAILED_UPDATE_INCOMPATIBLE" in text:
            raise RuntimeError(f"{what}: a copy of this game signed with another key is installed in AXRB. "
                               "Uninstall it there first (that deletes its Android saves), then install again.")
        raise RuntimeError(f"{what}: {text[-400:]}")
    return text


def resolve_activity(package: str) -> str:
    r = axrb.adb(["shell", "cmd", "package", "resolve-activity", "--brief", package], timeout=60)
    last = (r.stdout.strip().splitlines() or [""])[-1].strip()
    if not re.fullmatch(r"[\w.]+/[\w.$]+", last):
        raise RuntimeError(f"{package} has no launcher activity in Android (is it an OVRPort build?)")
    return last


def remote_sizes(package: str) -> dict[str, int]:
    """Sizes of the files already in the game's OBB folder in AXRB's Android (a re-install sends only the rest)."""
    r = axrb.adb(["shell", f"cd {OBB_ROOT}/{package} 2>/dev/null && find . -type f -exec stat -c '%s %n' {{}} +"],
                 timeout=120)
    out = {}
    for line in r.stdout.splitlines():
        size, _, name = line.strip().partition(" ")
        if size.isdigit() and name.startswith("./"):
            out[name[2:]] = int(size)
    return out


def push_data(package: str, data_dir: Path, files: list[str] | None, reporter: Reporter) -> int:
    manifest = data_manifest(data_dir, files)
    if not manifest:
        return 0
    have = remote_sizes(package)
    todo = {rel: size for rel, size in manifest.items() if have.get(rel) != size}
    total, done = sum(todo.values()) or 1, 0
    reporter.log(f"game data: {len(todo)} of {len(manifest)} files to copy ({total / 2**30:.1f} GiB)")
    for rel, size in todo.items():
        reporter.check_cancel()
        reporter.progress(done / total, f"copying game data: {rel}")
        r = axrb.adb(["push", winhost.to_windows(data_dir / rel), f"{OBB_ROOT}/{package}/{rel}"],
                     timeout=600 + size / 4e6)
        _adb_ok(r, f"copying {rel}")
        done += size
    reporter.progress(1.0, "game data copied")
    return len(todo)


# ------------------------------------------------------------------------------------------ AXRB setup
def ensure_axrb(reporter: Reporter, backend: str = axrb.EMULATOR, update: bool = False) -> axrb.Install:
    """AXRB for one backend and its Android runtime, installed/set up (or upgraded, see axrb.install) on first use."""
    label = "AXRB direct mode" if backend == axrb.DIRECT else "AXRB"
    have = axrb.find(backend)
    if not have or update or axrb.older(have.version, axrb.KNOWN_GOOD.get(backend)):
        reporter.stage(f"{'Update' if have else 'Download'} {label} (Android XR Bridge)")
    inst = axrb.install(lambda f: reporter.progress(f, f"downloading {label}"), reporter.check_cancel,
                        backend=backend, update=update)
    reporter.check(label, True, f"{inst.version or 'local build'} at {winhost.to_windows(inst.app)}")
    if backend == axrb.DIRECT:
        info = axrb.direct_requirements()
        problems = axrb.direct_requirement_problems(info)
        if problems:
            raise RuntimeError("This PC can't run Quest games in AXRB's direct mode: " + "; ".join(problems))
        reporter.check("PC requirements (AXRB direct mode)", True,
                       f"Vulkan, {info.get('memoryGB') or '?'} GB")
        state = axrb.direct_setup_state(inst)
        if state["missing_files"]:
            raise RuntimeError("AXRB's direct mode is incomplete (" + ", ".join(state["missing_files"][:4])
                               + "): reinstall or rebuild it")
        if not state["ready"]:
            reporter.stage(f"Set up AXRB's direct mode ({state['download_bytes'] / 2**30:.1f} GiB download)")
            axrb.setup_direct(inst, lambda f, name: reporter.progress(f, name), reporter.check_cancel)
        return inst
    info = axrb.requirements(inst.app)
    problems = axrb.requirement_problems(info)
    if problems:
        raise RuntimeError("This PC can't run Quest games through AXRB: " + "; ".join(problems))
    reporter.check("PC requirements (AXRB)", True, f"{info.get('gpu')}, {info.get('memoryGB')} GB")
    state = axrb.setup_state(app=inst.app)
    if state["components"] or not state["avd"]:
        reporter.stage(f"Set up AXRB's Android ({state['download_bytes'] / 2**30:.1f} GiB download)")
        axrb.setup_files(lambda f, name: reporter.progress(f, f"downloading {name}"), reporter.check_cancel,
                         app=inst.app)
    return inst


# ------------------------------------------------------------------------------------------ install
def apk_launcher_activity(apk: Path) -> str:
    """The launch activity from the APK's own manifest (direct mode has no Android to ask)."""
    from ..apk import axml

    try:
        with zipfile.ZipFile(apk) as z:
            activity = axml.launcher_activity(z.read("AndroidManifest.xml"))
    except (OSError, KeyError, zipfile.BadZipFile, ValueError) as exc:
        raise RuntimeError(f"{apk.name}: the manifest could not be read ({exc})") from None
    if not activity:
        raise RuntimeError(f"{apk.name} has no launcher activity (is it an OVRPort build?)")
    return activity


def _apk_versions(apk: Path) -> tuple[str | None, str | None]:
    """(versionName, versionCode) for AXRB's compatibility profiles."""
    try:
        from pyaxmlparser import APK

        a = APK(str(apk))
        return a.version_name, a.version_code
    except Exception:  # noqa: BLE001 - only used to match AXRB's optional profiles
        return None, None


def asset_lines(package: str, data_dir: Path | None, files: list[str] | None, reporter: Reporter) -> list[str]:
    """AXRB's content-file list (core/direct.mjs writeAssetList): "Android/obb/<pkg>/<file>\\t<Windows path>" per
    data file. Files stay where they are when Windows can read them, else they are copied next to the APK."""
    manifest = data_manifest(data_dir, files) if data_dir else {}
    lines = []
    for rel in manifest:
        host = data_dir / rel
        try:
            host_win = winhost.to_windows(host)
        except ValueError:  # e.g. a WSL-only folder: give AXRB a copy it can read
            copy = axrb.game_dir(package) / "obb" / rel
            if not copy.is_file() or copy.stat().st_size != host.stat().st_size:
                reporter.log(f"copying {rel} to where Windows can read it")
                copy.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(host, copy)
            host_win = winhost.to_windows(copy)
        guest = f"Android/obb/{package}/{rel}"
        # prepare_art_session.py's rules: inside the app's storage, no empty/dot parts, no backslash; one line each
        if any(part in ("", ".", "..") for part in guest.split("/")) or "\\" in guest or \
                re.search(r"[\t\r\n]", guest + host_win):
            raise RuntimeError(f"unsupported content file name: {rel}")
        lines.append(f"{guest}\t{host_win}")
    return lines


def _install_emulator(inst: axrb.Install, package: str, apk: Path, data_dir: Path | None, apk_only: bool,
                      data_files: list[str] | None, reporter: Reporter) -> tuple[str, int, Path]:
    reporter.stage("Start Android (AXRB; the first start takes several minutes)")
    started = axrb.start_emulator(app=inst.app)
    try:
        axrb.ensure_runtime_apk(app=inst.app)
        reporter.stage("Install the game into AXRB's Android")
        _adb_ok(axrb.adb(["install", "--no-incremental", "-r", winhost.to_windows(apk)], timeout=900),
                "installing the game")
        activity = resolve_activity(package)
        sent = 0 if apk_only or not data_dir else push_data(package, Path(data_dir), data_files, reporter)
        axrb.adb(["shell", "sync"], timeout=120)
    finally:
        if started:
            reporter.stage("Stop Android")
            axrb.stop_emulator(app=inst.app)
    return activity, sent, axrb.write_launcher(app=inst.app)


def _install_direct(inst: axrb.Install, package: str, apk: Path, data_dir: Path | None, apk_only: bool,
                    data_files: list[str] | None, reporter: Reporter) -> tuple[str, int, Path]:
    reporter.stage("Install the game for AXRB's direct mode")
    activity = apk_launcher_activity(apk)
    if activity.split("/")[0] != package:
        raise RuntimeError(f"{apk.name} is {activity.split('/')[0]}, not {package}")
    game = axrb.game_dir(package)
    game.mkdir(parents=True, exist_ok=True)
    target = game / f"{package}.apk"
    tmp = target.with_name(target.name + ".part")
    shutil.copyfile(apk, tmp)  # the converted copy in FramePort's output folder is removed after an install
    tmp.replace(target)
    listed = game / "assets.txt"
    if apk_only and listed.is_file():
        sent = 0
    else:
        lines = asset_lines(package, Path(data_dir) if data_dir else None, data_files, reporter)
        listed.write_text("".join(f"{line}\n" for line in lines), encoding="utf-8")
        sent = len(lines)
    return activity, sent, axrb.write_direct_launcher(inst)


def install(package: str, title: str, apk: Path, data_dir: Path | None, recipe: Recipe, reporter: Reporter,
            apk_only: bool = False, data_files: list[str] | None = None, record_dir: Path | None = None,
            appid=None, backend: str | None = None) -> dict:
    """`backend`: an explicit choice (CLI --axrb-backend), else choose_backend from the recipe and the analysis."""
    params = backend_params(recipe)
    why = "chosen for this install" if backend else "recipe" if params.get("backend") else "automatic"
    graphics = _analysis(package).get("graphics", "")
    backend = choose_backend(recipe, graphics, backend)
    reporter.log(f"AXRB backend: {backend} ({why}{', ' + graphics if why == 'automatic' else ''})")
    try:
        before = dep_backend(json.loads((record_dir / "deployment.json").read_text(encoding="utf-8")))
    except (OSError, ValueError, TypeError, AttributeError):
        before = backend
    if before != backend:  # the other backend's copy (and saves) stay where they are
        reporter.log(f"this game was installed for AXRB's {before} backend before; that copy and its saves stay "
                     "there (separate from this one)")
    inst = ensure_axrb(reporter, backend)
    name, code = _apk_versions(apk)
    profile = axrb.compatibility_runtime(inst, package, name, code)
    flags = launch_flags(backend, params, profile)
    install_one = _install_direct if backend == axrb.DIRECT else _install_emulator
    activity, sent, launcher = install_one(inst, package, apk, data_dir, apk_only, data_files, reporter)
    from ..build import sha256
    from ..patches.base import pc_selection

    dep = {"package": package, "kind": KIND, "backend": backend, "title": title, "activity": activity,
           "apk": str(apk), "sha256": sha256(apk), "launcher_win": winhost.to_windows(launcher),
           "powershell_win": powershell_win(), "flags": flags, "axrb_version": inst.version,
           "axrb_local": inst.local,
           "recipe": {"patches": sorted(pc_selection(recipe.patches)), "source": recipe.source}, "time": time.time()}
    if backend == axrb.DIRECT:
        dep["apk_win"] = winhost.to_windows(axrb.game_dir(package) / f"{package}.apk")
    dep["appid"] = appid(shortcut_fields(dep)[0], title) if appid else None
    if record_dir:
        record_dir.mkdir(parents=True, exist_ok=True)
        (record_dir / "deployment.json").write_text(json.dumps(dep, indent=2), encoding="utf-8")
    where = "AXRB's direct mode" if backend == axrb.DIRECT else "AXRB"
    verb = "listed" if backend == axrb.DIRECT else "copied"
    reporter.log(f"installed in {where} ({activity}); {sent} data files {verb}"
                 + (f"; launcher options {' '.join(flags)}" if flags else ""))
    return {"ok": True, "appid": dep["appid"], "activity": activity, "backend": backend}


def dep_backend(dep: dict) -> str:
    return dep.get("backend") or axrb.EMULATOR  # installs before direct mode were all emulator ones


# ------------------------------------------------------------------------------------------ FrameBridge settings
SETTINGS_FILE = "framebridge.conf"  # FrameBridge reads /sdcard/Android/data/<pkg>/files/framebridge.conf after
SETTING_KEY = re.compile(r"[a-z_]+")  # the build's libframe_settings.so (native/adapter/frame_adapter.c)
SETTING_VALUE = re.compile(r"-?(?:[0-9]+\.?[0-9]*|\.[0-9]+)")


def merge_settings(current: dict, changes: dict) -> dict:
    """The agent's rules (cmd_set_settings): numbers only, checked before anything is written; an empty value
    removes the key (back to the build's value)."""
    out = dict(current)
    for k, v in changes.items():
        v = str(v).strip()
        if not SETTING_KEY.fullmatch(k) or (v and not SETTING_VALUE.fullmatch(v)):
            raise ValueError(f"bad setting {k}={v}")
        if v:
            out[k] = v
        else:
            out.pop(k, None)
    return out


def settings_text(settings: dict) -> str:
    return "".join(f"{k}={v}\n" for k, v in settings.items())


def set_settings(dep: dict, changes: dict) -> dict:
    """Change FrameBridge settings of a game in AXRB without a rebuild: framebridge.conf in the game's external
    files folder overrides the build's settings at the next start. Direct mode: written into AXRB's runtime folder
    (direct\\sdcard\\Android\\data\\<pkg>\\files); emulator: pushed with adb (Android is started for it when it
    isn't running, and stopped again). `dep["settings"]` is updated; the caller saves the record."""
    package = dep["package"]
    if not re.fullmatch(r"[A-Za-z0-9_.]+", package):
        raise ValueError(f"unexpected package name {package!r}")
    current = merge_settings(dep.get("settings") or {}, changes)
    text = settings_text(current)
    if dep_backend(dep) == axrb.DIRECT:
        root = axrb.direct_runtime_root()
        if not root:
            raise RuntimeError("AXRB's direct-mode runtime folder was not found")
        files = root / "direct" / "sdcard" / "Android" / "data" / package / "files"
        files.mkdir(parents=True, exist_ok=True)
        (files / SETTINGS_FILE).write_text(text, encoding="utf-8", newline="\n")
        where = str(files / SETTINGS_FILE)
    else:
        remote = f"/sdcard/Android/data/{package}/files"
        local = axrb.game_dir(package) / SETTINGS_FILE  # a Windows path adb.exe can read
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_text(text, encoding="utf-8", newline="\n")
        started = axrb.start_emulator()
        try:
            axrb.adb(["shell", "mkdir", "-p", remote], timeout=60)
            if current:
                _adb_ok(axrb.adb(["push", winhost.to_windows(local), f"{remote}/{SETTINGS_FILE}"], timeout=120),
                        "copying the settings")
            else:
                axrb.adb(["shell", "rm", "-f", f"{remote}/{SETTINGS_FILE}"], timeout=60)
        finally:
            if started:
                axrb.stop_emulator()
        where = f"{remote}/{SETTINGS_FILE}"
    dep["settings"] = current
    return {"settings": current, "file": where}


# ------------------------------------------------------------------------------------------ logs + tests
EMULATOR_LOGS = (("session.json", "game/session.json", 1 << 16), ("host.err", "game/host.err", 1 << 20),
                 ("host.log", "game/host.log", 1 << 20), ("guest.log", "game/guest.log", 2 << 20),
                 ("emulator.stderr.log", "emulator/emulator.stderr.log", 1 << 18),
                 ("frameport-emulator-start.log", "frameport-emulator-start.log", 1 << 18))
# run_direct_game.ps1: the runner's stdout (AXRB_AOSP_* lifecycle lines, the game's Java output) and stderr, its
# own log (AXRB_DIRECT_LOG: "<s> <tid> E/tag: ..."), the bridge, the session record
DIRECT_LOGS = (("session.json", "game/session.json", 1 << 16), ("host.err", "game/host.err", 1 << 20),
               ("host.log", "game/host.log", 1 << 20), ("direct.log", "game/direct.log", 4 << 20),
               ("direct.out", "game/direct.out", 2 << 20), ("direct.err", "game/direct.err", 1 << 20),
               ("frameport-launch-test.log", "frameport-launch-test.log", 1 << 16))


def game_logs(backend: str | None = None) -> dict[str, str]:
    """AXRB's session logs (newest session) for triage and diagnostics. Both backends when `backend` is None: the
    direct ones then under "direct/"."""
    out = {}
    for b, files in ((axrb.EMULATOR, EMULATOR_LOGS), (axrb.DIRECT, DIRECT_LOGS)):
        if backend not in (None, b):
            continue
        root = axrb.runtime_root() if b == axrb.EMULATOR else axrb.direct_runtime_root()
        if not root:
            continue
        logs = axrb.log_dir(root)
        prefix = "direct/" if b == axrb.DIRECT and backend is None else ""
        for name, sub, limit in files:
            p = logs / sub
            if p.is_file():
                out[prefix + name] = axrb.read_log(p, limit)
    return out


def launch_test(dep: dict, reporter: Reporter, seconds: int = 45, triage=None, extra_env: dict | None = None):
    """Start the game through FramePort's launcher (= AXRB's run script), watch its Android process, stop it again.
    extra_env: environment for the launcher's processes; default = the simulated headset's while one runs
    (tools/xr_sim.py, `frameport test --to pc --sim`)."""
    if dep_backend(dep) == axrb.DIRECT:
        return _launch_test_direct(dep, reporter, seconds, triage, extra_env)
    reporter.stage("Launch test (this PC, AXRB)")
    package = dep["package"]
    if axrb.emulator_online() and _pid(package):
        raise RuntimeError(f"{dep['title']} is already running")
    log = axrb.log_dir() / "frameport-launch-test.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.unlink(missing_ok=True)
    before = time.time()
    q = axrb._ps_quote
    # --- xr-sim hook (tools/xr_sim.py): env + SteamVR-registration shim before the launcher call
    sim = xr_sim.launcher_prefix(xr_sim.active_env() if extra_env is None else extra_env)
    pid = axrb.spawn_detached(sim + axrb.logged(
        f"& {q(dep['launcher_win'])} -Package {q(package)} -Activity {q(dep['activity'])} "
        f"-GameName {q(dep['title'])} -CaptureGuestLog " + " ".join(dep.get("flags") or ()),
        winhost.to_windows(log)))
    seen, state, boot_deadline = False, "NEVER_STARTED", time.time() + 900  # the emulator boot comes first
    watch_end = None
    while time.time() < boot_deadline and axrb.pid_running(pid):
        reporter.check_cancel()
        time.sleep(3)
        running = axrb.emulator_online() and bool(_pid(package))
        if running and not seen:
            seen, watch_end = True, time.time() + seconds
        if seen and not running:
            state = "EXITED"
            break
        if watch_end and time.time() >= watch_end:
            state = "RUNNING"
            break
    if seen and state == "NEVER_STARTED":
        state = "EXITED"
    if axrb.pid_running(pid):
        try:  # AXRB's script sees the game stop, cleans up (and shuts Android down if it started it)
            axrb.adb(["shell", "am", "force-stop", package], timeout=30)
        except (axrb.AxrbError, OSError, subprocess.TimeoutExpired):
            pass
        end = time.time() + 300
        while time.time() < end and axrb.pid_running(pid):
            time.sleep(3)
    logs = game_logs(axrb.EMULATOR)
    text = "\n".join(f"===== {name}\n{body}" for name, body in logs.items()
                     if name != "frameport-emulator-start.log")
    text += "\n===== launcher\n" + axrb.read_log(log, 1 << 16)
    return _report(text, state, package, before, reporter, triage, "incl. Android start")


# direct.out lines AppProcess prints once the game's activity exists / once it ended (AXRB's ART bootstrap)
DIRECT_STARTED = re.compile(r"^AXRB_AOSP_(?:ACTIVITY_CREATED|LIFECYCLE) ", re.M)


def direct_state(out_text: str, launcher_running: bool) -> str | None:
    """RUNNING-so-far / EXITED / None (not started yet) from direct.out and whether the run script still runs."""
    if DIRECT_STARTED.search(out_text):
        return "RUNNING" if launcher_running else "EXITED"
    return None if launcher_running else "NEVER_STARTED"


def _launch_test_direct(dep: dict, reporter: Reporter, seconds: int, triage, extra_env: dict | None = None):
    reporter.stage("Launch test (this PC, AXRB direct mode)")
    inst = axrb.find(axrb.DIRECT)
    if not inst:
        raise RuntimeError("AXRB's direct mode is not installed on this PC any more")
    package = dep["package"]
    logs = axrb.log_dir(inst.runtime)
    log = logs / "frameport-launch-test.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.unlink(missing_ok=True)
    out = logs / "game" / "direct.out"
    before = time.time()
    q = axrb._ps_quote
    sim = xr_sim.launcher_prefix(xr_sim.active_env() if extra_env is None else extra_env)
    pid = axrb.spawn_detached(sim + axrb.logged(
        f"& {q(dep['launcher_win'])} -Package {q(package)} -Activity {q(dep['activity'])} "
        f"-GameName {q(dep['title'])} " + " ".join(dep.get("flags") or ()), winhost.to_windows(log)))
    state, deadline, watch_end = "NEVER_STARTED", time.time() + 300, None  # ART's first start is slow (no JIT)
    while time.time() < deadline:
        reporter.check_cancel()
        time.sleep(3)
        alive = axrb.pid_running(pid)
        fresh = out.is_file() and out.stat().st_mtime >= before - 5  # the run script rewrites it every session
        now = direct_state(axrb.read_log(out, 1 << 20) if fresh else "", alive)
        if now in ("EXITED", "NEVER_STARTED"):
            state = now
            break
        if now == "RUNNING" and not watch_end:
            watch_end, deadline = time.time() + seconds, time.time() + seconds + 5
        if watch_end and time.time() >= watch_end:
            state = "RUNNING"
            break
    if axrb.pid_running(pid):  # AXRB's own Stop: the bridge window closes, the game pauses, stops and saves
        try:
            axrb.stop_game(inst, pid)
        except (OSError, subprocess.TimeoutExpired):
            pass
        end = time.time() + 60
        while time.time() < end and axrb.pid_running(pid):
            time.sleep(3)
    text = "\n".join(f"===== {name}\n{body}" for name, body in game_logs(axrb.DIRECT).items()
                     if name != "frameport-launch-test.log")
    text += "\n===== launcher\n" + axrb.read_log(log, 1 << 16)
    return _report(text, state, package, before, reporter, triage, "direct mode")


def _report(text: str, state: str, package: str, before: float, reporter: Reporter, triage, note: str):
    result = triage(text, state, package, kind="axrb") if triage else None
    reporter.check("Game process", state == "RUNNING", f"{state} ({round(time.time() - before)}s, {note})")
    if result:
        for m in result.milestones:
            reporter.check(m, True)
        for f in result.findings:
            reporter.check(f.id, None if f.severity in ("warning", "info") else False,
                           f"{f.diagnosis} [{f.evidence[:160]}]")
    return result, text


def _pid(package: str) -> str:
    try:
        return axrb.adb(["shell", "pidof", package], timeout=20).stdout.strip()
    except (axrb.AxrbError, OSError, subprocess.TimeoutExpired):
        return ""


def uninstall(dep: dict, keep_data: bool = True) -> bool:
    """Remove the game from AXRB (saves kept unless keep_data is False). False if AXRB isn't there."""
    if dep_backend(dep) == axrb.DIRECT:
        return _uninstall_direct(dep, keep_data)
    if not axrb.app_dir() or not axrb.adb_exe():
        return False
    started = axrb.start_emulator()
    try:
        args = ["shell", "pm", "uninstall", "-k", dep["package"]] if keep_data else ["uninstall", dep["package"]]
        r = axrb.adb(args, timeout=240)
        return "Success" in r.stdout
    finally:
        if started:
            axrb.stop_emulator()


def _uninstall_direct(dep: dict, keep_data: bool) -> bool:
    """Direct mode: FramePort's copy of the APK and its content list; with keep_data False also the game's Android
    data in AXRB's runtime folder (direct\\data\\data\\<pkg> = /data/data, direct\\sdcard\\Android\\data\\<pkg>)."""
    package = dep["package"]
    try:
        shutil.rmtree(axrb.game_dir(package), ignore_errors=True)
    except axrb.AxrbError:
        return False
    root = axrb.direct_runtime_root()
    if not keep_data and root:
        direct = root / "direct"
        for d in (direct / "data" / "data" / package, direct / "sdcard" / "Android" / "data" / package):
            shutil.rmtree(d, ignore_errors=True)
    return True
