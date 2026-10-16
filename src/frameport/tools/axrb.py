"""AXRB (Android XR Bridge) as an external tool: Quest games on this Windows PC.

AXRB (github.com/Android-XR-Bridge/AXRB) runs Android OpenXR apps on Windows and shows them through the PC's OpenXR
runtime (SteamVR, ...). It has two backends, which are two separate programs (an `Install` each):

- "emulator" (AXRB 1.x, %LOCALAPPDATA%\\Programs\\axrb-launcher): an Android 16 emulator (x86_64 image, arm64 code
  through Android's ARM translation). Its runtime (Android SDK pieces + the AVD) is set up the way AXRB's own launcher
  does it, in AXRB's managed folder (%LOCALAPPDATA%\\AXRB Runtime), so AXRB's launcher sees the same runtime and
  games: the pinned downloads come from the installed AXRB's own component list (resources/app.asar
  core/components.json), the AVD values from its core/setup.mjs.
- "direct" (AXRB-Emulatorless 2.x, %LOCALAPPDATA%\\Programs\\axrb-emulatorless-launcher): axrb-direct.exe runs the
  game's arm64 code and AOSP's ART without an emulator (Dynarmic JIT; Vulkan only, no GLES renderer). Its runtime is
  Google's Android 16 arm64 system image turned into a system root by AXRB's own make_sysroot.py
  (%LOCALAPPDATA%\\AXRB-Emulatorless Runtime\\direct\\sysroot), pinned values from the installed core/direct.mjs.
  FRAMEPORT_AXRB_DIRECT_DIR points at a local build instead (AXRB's source tree after scripts/build/build_direct.ps1,
  or a folder laid out like the installed resources\\runtime).

FramePort never bundles or modifies AXRB: the official installer is downloaded only when a user installs a Quest
game on this PC, and FramePort talks to it through its public interfaces (its PowerShell and Python scripts, adb).
See docs/PC_ANDROID.md.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path

from ..core import cache, winhost
from ..core.paths import write_atomic

AXRB_RELEASE = "https://api.github.com/repos/Android-XR-Bridge/AXRB/releases/latest"
AXRB_TAG = "https://api.github.com/repos/Android-XR-Bridge/AXRB/releases/tags/v{version}"
EMULATOR, DIRECT = "emulator", "direct"
BACKENDS = (EMULATOR, DIRECT)
# The release FramePort was tested with, per backend: installed when the latest release has no installer for that
# backend (AXRB's main line became the emulator-less 2.x), and the floor an older install is upgraded to.
KNOWN_GOOD = {EMULATOR: "1.0.4"}
SETUP_ASSET = {EMULATOR: r"AXRB-Setup-[\w.]+\.exe", DIRECT: r"AXRB-Emulatorless-Setup-[\w.]+\.exe"}
APP_FOLDER = "axrb-launcher"  # electron-builder installs under %LOCALAPPDATA%\Programs\<package name>
APP_FOLDERS = {EMULATOR: APP_FOLDER, DIRECT: "axrb-emulatorless-launcher"}
APP_EXES = {EMULATOR: "AXRB.exe", DIRECT: "AXRB-Emulatorless.exe"}
RUNTIME_FOLDER = "AXRB Runtime"
DIRECT_RUNTIME_FOLDER = "AXRB-Emulatorless Runtime"  # core/build-profile.mjs runtimeDirectory
DIRECT_PROFILE = "AXRB-Emulatorless"  # %APPDATA%\<productName>\library.json: the launcher's chosen runtime folder
AVD = "axrb-managed-api36"  # fallbacks for what the installed core/setup.mjs says
API_LEVEL, IMAGE_TAG, IMAGE_ABI = 36, "google_apis", "x86_64"
PORT = 5584  # emulator console port of AXRB's managed AVD -> adb serial emulator-5584
SERIAL = f"emulator-{PORT}"
ADB_PORT = "5038"  # AXRB's own adb server, away from Android Studio's 5037
RUNTIME_PKG = "com.axrb.openxrruntime"
RUNTIME_APK = "out/android/runtime-arm64-v8a/axrb-openxr-runtime-debug.apk"
DOWNLOAD_HOST = "https://dl.google.com/android/repository/"
MIN_MEMORY_GB = 12
DIRECT_MIN_MEMORY_GB = 6  # core/build-profile.mjs minimumMemoryGB
MEMORY_MB, CPU_CORES, STORAGE_GB = 8192, 4, 32  # AXRB's launcher defaults
LAUNCHER_NAME = "fp-axrb-run.ps1"
DIRECT_LAUNCHER_NAME = "fp-axrb-direct.ps1"
DIRECT_EXE = "out/direct/axrb-direct.exe"
DIRECT_SCRIPT = "scripts/run/run_direct_game.ps1"
MAKE_SYSROOT = "direct/tools/make_sysroot.py"
STOP_SCRIPT = "scripts/run/stop_game.ps1"
HOST_EXE = "out/host/bin/Release/axrb-host-bridge.exe"
TRANSLATORS = ("dynarmic", "digitalis")  # run_direct_game.ps1 -Translator; AXRB's default first
# Fallbacks for what core/direct.mjs pins (read from the installed AXRB when it is there): Google's Android 16 arm64
# system image, the sysroot receipt's format, the files a complete sysroot has.
DIRECT_IMAGE = {"id": "direct-system", "name": "Android 16 ARM64 system libraries",
                "url": DOWNLOAD_HOST + "sys-img/android/arm64-v8a-36_r02.zip", "size": 810736863,
                "sha1": "62ad6714df790f89c8a8ad32552ffe20bb16fe87"}
SYSROOT_FORMAT = 2
SYSROOT_FILES = ("system/bin/app_process64", "system/lib64/libandroid.so", "system/framework/framework.jar",
                 "system/framework/framework-res.apk", "system/etc/classpaths/bootclasspath.pb",
                 "apex/com.android.art/lib64/libart.so", "apex/com.android.art/javalib/core-oj.jar",
                 "apex/com.android.i18n/javalib/core-icu4j.jar")
SYSROOT_RECEIPT = ".axrb-sysroot.json"
# core/direct-runtime-files.cjs (what every direct installation has, relative to AXRB's root) + the bridge and the
# script helpers run_direct_game.ps1 needs
DIRECT_RUNTIME_FILES = (
    DIRECT_EXE, "out/direct/art/axrb-bootstrap.dex",
    *(f"out/direct/art/lib64/{n}" for n in ("libandroid.so", "libaxrbndk.so", "libmediandk.so", "libaxrbmndk.so",
                                             "libaxrb_art_bridge.so", "libOpenSLES.so")),
    *(f"out/direct/guest/lib64/{n}" for n in ("libandroid.so", "libvulkan.so", "libEGL.so", "libGLESv1_CM.so",
                                               "libGLESv2.so", "libGLESv3.so", "libaaudio.so", "libOpenSLES.so",
                                               "libopenxr_runtime.so")),
    "out/direct/guest/axrb-systemdriver.apk", DIRECT_SCRIPT, MAKE_SYSROOT, "direct/tools/prepare_art_session.py",
    "scripts/paths.ps1", HOST_EXE)


class AxrbError(RuntimeError):
    pass


# ------------------------------------------------------------------------------------------ where things are
def local_appdata() -> Path | None:
    return winhost.env_path("LOCALAPPDATA") if winhost.available() else None


def app_dir() -> Path | None:
    """The installed AXRB (folder with AXRB.exe): FRAMEPORT_AXRB_DIR, else the per-user install."""
    if os.environ.get("FRAMEPORT_AXRB_DIR"):
        d = Path(os.environ["FRAMEPORT_AXRB_DIR"])
        return d if (d / "resources" / "runtime").is_dir() else None
    local = local_appdata()
    d = local / "Programs" / APP_FOLDER if local else None
    return d if d and (d / "AXRB.exe").is_file() and (d / "resources" / "runtime").is_dir() else None


def resources(app: Path | None = None) -> Path | None:
    app = app or app_dir()
    return app / "resources" / "runtime" if app else None


def runtime_root() -> Path | None:
    """AXRB's managed emulator folder (SDK, AVD, logs)."""
    if os.environ.get("FRAMEPORT_AXRB_RUNTIME"):
        return Path(os.environ["FRAMEPORT_AXRB_RUNTIME"])
    local = local_appdata()
    return local / RUNTIME_FOLDER if local else None


def adb_exe(root: Path | None = None) -> Path | None:
    root = root or runtime_root()
    p = root / "sdk" / "platform-tools" / "adb.exe" if root else None
    return p if p and p.is_file() else None


# ------------------------------------------------------------------------------------------ app.asar
def asar_read(asar: Path, name: str) -> bytes:
    """One file from an Electron asar archive (header: pickle sizes + a JSON index; data after the header)."""
    with open(asar, "rb") as f:
        _, header_size, _, json_len = struct.unpack("<4I", f.read(16))
        index = json.loads(f.read(json_len))
        node = index
        for part in name.split("/"):
            node = (node.get("files") or {}).get(part)
            if node is None:
                raise KeyError(name)
        if "offset" not in node:
            raise KeyError(f"{name} is unpacked or a folder")
        f.seek(8 + header_size + int(node["offset"]))
        return f.read(node["size"])


def installed_version(app: Path | None = None) -> str | None:
    app = app or app_dir()
    if not app:
        return None
    try:
        return json.loads(asar_read(app / "resources" / "app.asar", "package.json")).get("version")
    except (OSError, KeyError, ValueError, struct.error):
        return None


def components(app: Path | None = None) -> list[dict]:
    """The emulator pieces the installed AXRB pins (emulator, platform-tools, build-tools, system image)."""
    app = app or app_dir()
    if not app:
        raise AxrbError("AXRB is not installed")
    items = json.loads(asar_read(app / "resources" / "app.asar", "core/components.json"))
    for c in items:  # same rule as AXRB's launcher: only Google's official repository
        if not str(c.get("url", "")).startswith(DOWNLOAD_HOST) or not (c.get("sha256") or c.get("sha1")):
            raise AxrbError(f"unexpected AXRB component download: {c.get('url')}")
    return items


def launcher_source(app: Path | None, name: str) -> str:
    """One of the AXRB launcher's own files (e.g. core/setup.mjs) as text: from the installed app.asar, or from
    launcher/ in a source tree (a local build). "" when there is none."""
    if not app:
        return ""
    for asar in (app / "resources" / "app.asar", app.parent / "app.asar"):
        if asar.is_file():
            try:
                return asar_read(asar, name).decode("utf-8", errors="replace")
            except (OSError, KeyError, ValueError, struct.error):
                return ""
    try:
        return (app / "launcher" / name).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def emulator_values(app: Path | None = None) -> dict:
    """The managed AVD's name, API level and system image (tag, ABI) the installed AXRB uses (core/setup.mjs
    `managedAvd`, `systemImageDirectory`), else 1.0.4's."""
    text = launcher_source(app or app_dir(), "core/setup.mjs")
    avd = re.search(r"managedAvd\s*=\s*'([\w.-]+)'", text)
    image = re.search(r"system-images/android-(\d+)/(\w+)/(\w+)", text)
    return {"avd": avd[1] if avd else AVD, "api": int(image[1]) if image else API_LEVEL,
            "tag": image[2] if image else IMAGE_TAG, "abi": image[3] if image else IMAGE_ABI}


def _source(where, name: str) -> str:
    """launcher_source for an Install (its app folder, else its resources: a local build's source tree) or a folder."""
    if isinstance(where, Install):
        return launcher_source(where.app, name) or launcher_source(where.resources, name)
    return launcher_source(where, name)


def direct_values(where) -> dict:
    """What the installed direct-mode AXRB (an Install or its folder) pins (core/direct.mjs): {image: {url, size,
    sha1, ...}, format, files}. Only Google's repository with a SHA-1, like AXRB's own officialDownload rule; without
    the file FramePort's copy of 2.0.0's values."""
    text = _source(where, "core/direct.mjs")
    block = re.search(r"directSystemImage\s*=\s*\{(.*?)\}", text, re.S)
    image = dict(DIRECT_IMAGE)
    if block:
        url = re.search(r"url:\s*'([^']+)'", block[1])
        size = re.search(r"size:\s*([\d_]+)", block[1])
        sha1 = re.search(r"sha1:\s*'([0-9a-fA-F]{40})'", block[1])
        if url and size and sha1:
            if not url[1].startswith(DOWNLOAD_HOST):
                raise AxrbError(f"unexpected AXRB system image download: {url[1]}")
            image.update(url=url[1], size=int(size[1].replace("_", "")), sha1=sha1[1].lower())
    fmt = re.search(r"SYSROOT_FORMAT\s*=\s*(\d+)", text)
    files = re.search(r"directSystemFiles\s*=\s*\[(.*?)\]", text, re.S)
    return {"image": image, "format": int(fmt[1]) if fmt else SYSROOT_FORMAT,
            "files": tuple(re.findall(r"'([^']+)'", files[1])) if files else SYSROOT_FILES}


def compatibility_runtime(where, package: str, version_name: str | None = None,
                          version_code: str | None = None) -> dict:
    """The runtime options of the installed AXRB's (an Install or its folder) own compatibility profile for this
    game (core/game-compatibility.json, matched like compatibility.mjs: package, then every version field a rule
    names), {} when it has none."""
    try:
        profiles = json.loads(_source(where, "core/game-compatibility.json") or "{}").get("profiles") or []
    except (ValueError, AttributeError):
        return {}
    for p in profiles:
        if not isinstance(p, dict) or p.get("package") != package:
            continue
        rules = p.get("versions") or [{}]
        if any(isinstance(r, dict) and ("versionCode" not in r or str(version_code) == r["versionCode"])
               and ("versionName" not in r or str(version_name) == r["versionName"]) for r in rules):
            return dict(p.get("runtime") or {})
    return {}


# ------------------------------------------------------------------------------------------ installs
@dataclass(frozen=True)
class Install:
    """One installed AXRB backend. `resources` is AXRB's own root (scripts/, out/, tools/python: what its scripts
    call $AxrbRoot), `runtime` its managed data folder (emulator: SDK + AVD + logs; direct: sysroot + app data +
    logs). `local` = a local build named by an environment variable (never replaced by an installer)."""
    backend: str
    version: str | None
    app: Path
    resources: Path
    runtime: Path | None
    local: bool = False

    @property
    def scripts(self) -> Path:
        return self.resources / "scripts"

    @property
    def python(self) -> Path | None:
        """AXRB's embedded Python (its scripts' interpreter), None when this build has none."""
        p = self.resources / "tools" / "python" / "python.exe"
        return p if p.is_file() else None

    def describe(self) -> dict:
        return {"backend": self.backend, "version": self.version, "app": str(self.app),
                "resources": str(self.resources), "runtime": str(self.runtime) if self.runtime else None,
                "local": self.local}


def direct_resources(folder: Path) -> Path | None:
    """AXRB's root in a direct-mode folder: an installed app (resources/runtime), a source tree or staged copy with
    out/direct/axrb-direct.exe, or out/direct itself."""
    for root in (folder / "resources" / "runtime", folder, folder.parent.parent):
        if (root / DIRECT_EXE).is_file() and (root / DIRECT_SCRIPT).is_file():
            return root
    return None


def direct_app_dir() -> Path | None:
    """Folder of the direct-mode AXRB: FRAMEPORT_AXRB_DIRECT_DIR, else the per-user AXRB-Emulatorless install."""
    if os.environ.get("FRAMEPORT_AXRB_DIRECT_DIR"):
        d = Path(os.environ["FRAMEPORT_AXRB_DIRECT_DIR"])
        return d if direct_resources(d) else None
    local = local_appdata()
    d = local / "Programs" / APP_FOLDERS[DIRECT] if local else None
    return d if d and (d / APP_EXES[DIRECT]).is_file() and direct_resources(d) else None


def direct_runtime_root() -> Path | None:
    """The direct backend's data folder: FRAMEPORT_AXRB_DIRECT_RUNTIME, else the one AXRB's launcher was set up
    with (its library.json settings.managedDirectory), else %LOCALAPPDATA%\\AXRB-Emulatorless Runtime."""
    if os.environ.get("FRAMEPORT_AXRB_DIRECT_RUNTIME"):
        return Path(os.environ["FRAMEPORT_AXRB_DIRECT_RUNTIME"])
    roaming = winhost.env_path("APPDATA") if winhost.available() else None
    if roaming:
        try:
            chosen = json.loads((roaming / DIRECT_PROFILE / "library.json").read_text(encoding="utf-8"))
            managed = ((chosen.get("settings") or {}).get("managedDirectory") or "").strip()
            if managed:
                return Path(managed) if winhost.is_windows() else winhost.to_local(managed)
        except (OSError, ValueError, AttributeError):
            pass
    local = local_appdata()
    return local / DIRECT_RUNTIME_FOLDER if local else None


def _direct_version(app: Path, resources: Path) -> str | None:
    text = launcher_source(app, "package.json") or launcher_source(resources, "package.json")
    try:
        return json.loads(text).get("version") if text else None
    except ValueError:
        return None


def find(backend: str = EMULATOR) -> Install | None:
    """The installed AXRB for one backend, None when it isn't there."""
    if backend == EMULATOR:
        app = app_dir()
        return Install(EMULATOR, installed_version(app), app, resources(app), runtime_root(),
                       local=bool(os.environ.get("FRAMEPORT_AXRB_DIR"))) if app else None
    if backend == DIRECT:
        app = direct_app_dir()
        if not app:
            return None
        res = direct_resources(app)
        return Install(DIRECT, _direct_version(app, res), app, res, direct_runtime_root(),
                       local=bool(os.environ.get("FRAMEPORT_AXRB_DIRECT_DIR")))
    raise ValueError(f"unknown AXRB backend {backend!r}")


def installs() -> dict[str, Install]:
    return {b: i for b in BACKENDS if (i := find(b))}


# ------------------------------------------------------------------------------------------ install AXRB
def release_info(rel: dict | None, backend: str = EMULATOR) -> dict | None:
    """{version, setup, setup_url, sums_url} when a GitHub release has this backend's installer + checksums."""
    if not rel or rel.get("draft") or rel.get("prerelease"):
        return None
    assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
    setup = next((n for n in assets if re.fullmatch(SETUP_ASSET[backend], n)), None)
    sums = next((n for n in assets if n.startswith("SHA256SUMS") and n.endswith(".txt")), None)
    if not setup or not sums:
        return None
    return {"version": rel.get("tag_name", "").lstrip("v"), "setup": setup, "setup_url": assets[setup],
            "sums_url": assets[sums]}


def latest(backend: str = EMULATOR) -> dict | None:
    """The release to install for a backend: AXRB's latest when it has that backend's installer, else the known-good
    one (the emulator after AXRB's main line went emulator-less)."""
    found = release_info(cache.cached_json("axrb-release.json", AXRB_RELEASE, max_age=6 * 3600), backend)
    if not found and backend in KNOWN_GOOD:
        version = KNOWN_GOOD[backend]
        found = release_info(cache.cached_json(f"axrb-release-{version}.json", AXRB_TAG.format(version=version),
                                               max_age=7 * 86400), backend)
    return found


def older(version: str | None, than: str | None) -> bool:
    from ..updates import parse_version

    return bool(version and than) and parse_version(version) < parse_version(than)


def upgrade_needed(backend: str, installed: str | None, offered: str | None, update: bool = False) -> bool:
    """Whether install() runs AXRB's installer over an existing install: always below the known-good release, and
    with `update` when the offered release is newer. An unreadable version is left alone unless asked to update."""
    if older(installed, KNOWN_GOOD.get(backend)):
        return not offered or not older(offered, installed)
    return bool(update and (not installed or older(installed, offered)))


def parse_sums(text: str) -> dict[str, str]:
    out = {}
    for line in text.splitlines():
        m = re.match(r"^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$", line)
        if m:
            out[m[2]] = m[1].lower()
    return out


def install(progress=None, cancel=None, wait: float = 600, backend: str = EMULATOR, update: bool = False) -> Install:
    """AXRB for one backend: download its official installer (checksum from the release's SHA256SUMS) and run it
    silently (per user, no admin) when it isn't installed, is older than the known-good release, or (`update`) is
    older than the release on offer. A local build (FRAMEPORT_AXRB_*_DIR) is never replaced."""
    have = find(backend)
    if have and (have.local or not (update or older(have.version, KNOWN_GOOD.get(backend)))):
        return have  # no release lookup needed
    rel = latest(backend)
    if not rel:
        if have:  # offline or nothing newer published: keep what is there
            return have
        if backend == DIRECT:
            raise AxrbError("AXRB-Emulatorless (AXRB's direct mode) has no published release with an installer yet. "
                            "Build it from AXRB's source and set FRAMEPORT_AXRB_DIRECT_DIR to that folder.")
        raise AxrbError("could not reach GitHub to find AXRB's latest release")
    if have and not upgrade_needed(backend, have.version, rel["version"], update):
        return have
    sums = parse_sums(cache.http_get(rel["sums_url"], timeout=30).text)
    want = sums.get(rel["setup"])
    if not want:
        raise AxrbError(f"AXRB's release has no checksum for {rel['setup']}")
    temp = winhost.env_path("TEMP") or local_appdata()
    if not temp:
        raise AxrbError("Windows' temporary folder was not found")
    setup = cache.download(rel["setup_url"], temp / "FramePort" / rel["setup"], progress, expected_sha256=want)
    if cancel:
        cancel()
    subprocess.run([str(setup), "/S"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=wait, cwd=str(setup.parent))
    end = time.time() + wait  # the NSIS stub can return before its installer child is done

    def done() -> bool:
        now = find(backend)
        return bool(now) and not older(now.version, rel["version"])
    while time.time() < end and not done():
        if cancel:
            cancel()
        time.sleep(2)
    setup.unlink(missing_ok=True)
    if not done():
        raise AxrbError(f"AXRB's installer finished but AXRB {rel['version']} was not found in "
                        "%LOCALAPPDATA%\\Programs")
    return find(backend)


# ------------------------------------------------------------------------------------------ runtime setup
def avd_config(image_win: str, memory_mb: int = MEMORY_MB, cores: int = CPU_CORES, storage_gb: int = STORAGE_GB,
               values: dict | None = None) -> str:
    """The AVD config.ini AXRB's launcher writes (core/setup.mjs avdConfig)."""
    v = values or emulator_values()
    cfg = {"avd.ini.encoding": "UTF-8", "AvdId": v["avd"], "avd.ini.displayname": "AXRB", "abi.type": v["abi"],
           "hw.cpu.arch": v["abi"], "hw.cpu.ncore": cores, "hw.ramSize": memory_mb, "hw.gpu.enabled": "yes",
           "hw.gpu.mode": "host", "hw.audioOutput": "yes", "hw.audioInput": "yes", "hw.lcd.width": 1080,
           "hw.lcd.height": 1920, "hw.lcd.density": 420, "hw.keyboard": "no", "hw.mainKeys": "no", "hw.useext4": "yes",
           "disk.dataPartition.size": f"{storage_gb}G", "disk.cachePartition.size": "66MB", "vm.heapSize": 576,
           "image.sysdir.1": image_win.rstrip("\\") + "\\", "tag.id": v["tag"], "target": f"android-{v['api']}",
           "fastboot.forceColdBoot": "no", "fastboot.forceFastBoot": "yes", "showDeviceFrame": "no",
           "runtime.network.speed": "full", "runtime.network.latency": "none", "PlayStore.enabled": "no"}
    return "".join(f"{k}={v}\n" for k, v in cfg.items())


def _digest(c: dict) -> str:
    return c.get("sha256") or c["sha1"]


def missing_components(root: Path | None = None, app: Path | None = None) -> list[dict]:
    root = root or runtime_root()
    out = []
    for c in components(app):
        dest = root / "sdk" / c["destination"]
        try:
            ok = (dest / c["probe"]).exists() and \
                json.loads((dest / ".axrb-component.json").read_text()).get("digest") == _digest(c)
        except (OSError, ValueError):
            ok = False
        if not ok:
            out.append(c)
    return out


def runtime_hash(app: Path | None = None) -> str:
    h = hashlib.sha256()
    with open(resources(app) / RUNTIME_APK, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def setup_state(root: Path | None = None, app: Path | None = None) -> dict:
    """What the runtime still needs: {components: [...], download_bytes, avd: bool, runtime_apk: bool, ready}."""
    root = root or runtime_root()
    if not root:
        raise AxrbError("Windows' local app data folder was not found")
    comps = missing_components(root, app)
    avd = (root / "avd" / f"{emulator_values(app)['avd']}.avd" / "config.ini").is_file()
    try:
        receipt = json.loads((root / "ready.json").read_text())
    except (OSError, ValueError):
        receipt = {}
    apk_ok = receipt.get("runtimeHash") == runtime_hash(app)
    return {"components": [c["id"] for c in comps], "download_bytes": sum(c["size"] for c in comps), "avd": avd,
            "runtime_apk": apk_ok, "ready": not comps and avd and apk_ok}


def setup_files(progress=None, cancel=None, root: Path | None = None, app: Path | None = None) -> None:
    """Download + verify + unpack the pinned emulator pieces and write the AVD (no emulator start yet)."""
    root = root or runtime_root()
    sdk, cache_dir = root / "sdk", root / "downloads"
    cache_dir.mkdir(parents=True, exist_ok=True)
    write_atomic(root / "license-acceptance.json",
                 json.dumps({"license": "android-sdk-license", "acceptedAt": _now()}))
    todo = missing_components(root, app)
    total = sum(c["size"] for c in todo) or 1
    done = 0
    for c in todo:
        if cancel:
            cancel()
        archive = cache_dir / f"{c['id']}.zip"
        cache.download(c["url"], archive,
                       (lambda f, base=done, size=c["size"], name=c["name"]: progress((base + f * size) / total, name))
                       if progress else None,
                       expected_sha256=c.get("sha256"), expected_sha1=c.get("sha1"))
        if cancel:
            cancel()
        staging = cache_dir / f"{c['id']}-extract"
        shutil.rmtree(staging, ignore_errors=True)
        try:
            with zipfile.ZipFile(archive) as z:
                z.extractall(staging)
            src = staging / c["prefix"]
            if not (src / c["probe"]).exists():
                raise AxrbError(f"{c['name']}: expected files are missing from the download")
            dest = sdk / c["destination"]
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.rmtree(dest, ignore_errors=True)
            shutil.move(str(src), str(dest))
            (dest / ".axrb-component.json").write_text(json.dumps({"digest": _digest(c)}))
        finally:
            shutil.rmtree(staging, ignore_errors=True)
            archive.unlink(missing_ok=True)
        done += c["size"]
    v = emulator_values(app)
    avd_dir = root / "avd" / f"{v['avd']}.avd"
    avd_dir.mkdir(parents=True, exist_ok=True)
    root_win = winhost.to_windows(root)
    if not (avd_dir / "config.ini").is_file():
        image = root_win + f"\\sdk\\system-images\\android-{v['api']}\\{v['tag']}\\{v['abi']}"
        (avd_dir / "config.ini").write_text(avd_config(image, values=v))
    (root / "avd" / f"{v['avd']}.ini").write_text(
        f"avd.ini.encoding=UTF-8\npath={root_win}\\avd\\{v['avd']}.avd\ntarget=android-{v['api']}\n")
    for d in ("android", "output"):
        (root / d).mkdir(exist_ok=True)


def _now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime())


# ------------------------------------------------------------------------------------------ direct-mode setup
def direct_data(inst: Install) -> Path:
    """<runtime>\\direct: the game's Android /data and /sdcard (app data, saves), the sysroot (core/direct.mjs
    directPaths)."""
    if not inst.runtime:
        raise AxrbError("Windows' local app data folder was not found")
    return inst.runtime / "direct"


def sysroot_dir(inst: Install) -> Path:
    return direct_data(inst) / "sysroot"


def missing_direct_files(inst: Install) -> list[str]:
    """Files of the direct runner that are missing or empty (core/direct.mjs checkDirectRuntime)."""
    out = []
    for name in DIRECT_RUNTIME_FILES:
        p = inst.resources / name
        try:
            if not p.is_file() or not p.stat().st_size:
                out.append(name)
        except OSError:
            out.append(name)
    return out


def sysroot_ready(inst: Install, values: dict | None = None) -> bool:
    """The receipt matches the pinned image and format, and every key file is there (core/direct.mjs
    directSystemReady: a receipt alone doesn't make an incomplete sysroot ready)."""
    values = values or direct_values(inst)
    root = sysroot_dir(inst)
    try:
        receipt = json.loads((root / SYSROOT_RECEIPT).read_text(encoding="utf-8"))
        if receipt.get("digest") != values["image"]["sha1"] or receipt.get("format") != values["format"]:
            return False
        return all((root / f).is_file() and (root / f).stat().st_size for f in values["files"])
    except (OSError, ValueError, AttributeError):
        return False


def direct_setup_state(inst: Install) -> dict:
    """What direct mode still needs: {missing_files, components, download_bytes, sysroot, license, ready}."""
    values = direct_values(inst)
    missing = missing_direct_files(inst)
    ready = sysroot_ready(inst, values)
    try:
        license_ok = json.loads((inst.runtime / "license-acceptance.json").read_text()).get("license") \
            == "android-sdk-license"
    except (OSError, ValueError, TypeError, AttributeError):
        license_ok = False
    return {"backend": DIRECT, "missing_files": missing, "components": [] if ready else [values["image"]["id"]],
            "download_bytes": 0 if ready else values["image"]["size"], "sysroot": ready, "license": license_ok,
            "ready": not missing and ready and license_ok}


def setup_direct(inst: Install, progress=None, cancel=None, timeout: float = 20 * 60) -> None:
    """Direct mode's Android userspace, the way AXRB's launcher makes it (core/direct.mjs ensureDirectSystem): the
    pinned Android 16 arm64 system image from Google (SHA-1 checked, in a folder of its own), turned into the sysroot
    by AXRB's make_sysroot.py --art with AXRB's embedded Python, then the receipt AXRB reads. The 810 MB archive is
    removed afterwards. Also the licence receipt AXRB's own launcher checks, so it sees the same runtime as set up."""
    missing = missing_direct_files(inst)
    if missing:
        raise AxrbError("AXRB's direct mode installation is incomplete (" + ", ".join(missing[:4])
                        + "). Reinstall AXRB-Emulatorless or rebuild it.")
    values = direct_values(inst)
    data = direct_data(inst)
    data.mkdir(parents=True, exist_ok=True)
    (inst.runtime / "output").mkdir(exist_ok=True)
    if not (inst.runtime / "license-acceptance.json").is_file():
        write_atomic(inst.runtime / "license-acceptance.json",
                     json.dumps({"license": "android-sdk-license", "acceptedAt": _now()}))
    if sysroot_ready(inst, values):
        return
    image = values["image"]
    need = image["size"] + 6 * 2**30  # the archive + its unpacked system image while the sysroot is made
    if shutil.disk_usage(data).free < need:
        raise AxrbError(f"AXRB's direct mode needs {need / 2**30:.0f} GB free in {winhost.to_windows(data)}")
    python = inst.python
    if not python and not inst.local:
        raise AxrbError("AXRB's embedded Python (resources\\runtime\\tools\\python) is missing")
    downloads = data / "downloads" / "frameport"
    shutil.rmtree(downloads, ignore_errors=True)
    downloads.mkdir(parents=True)
    archive = downloads / f"{image['id']}.zip"
    staging = data / "sysroot.new"
    try:
        cache.download(image["url"], archive, (lambda f: progress(f, image["name"])) if progress else None,
                       expected_sha1=image["sha1"])
        if cancel:
            cancel()
        shutil.rmtree(staging, ignore_errors=True)
        Path(f"{staging}.system.img").unlink(missing_ok=True)
        if progress:
            progress(1.0, "Preparing Android ART and framework")
        args = [winhost.to_windows(inst.resources / MAKE_SYSROOT), winhost.to_windows(archive),
                winhost.to_windows(staging), "--art"]
        if python:
            r = winhost.run_win([str(python), *args], timeout=timeout)
        else:  # a local build without AXRB's embedded Python: Windows' own, as run_direct_game.ps1 falls back to
            r = winhost.powershell("& python " + " ".join(_ps_quote(a) for a in args) + "; exit $LASTEXITCODE",
                                   timeout=timeout)
        if r.returncode != 0 or not staging.is_dir():
            raise AxrbError("AXRB's make_sysroot.py failed: " + ((r.stderr or r.stdout).strip()[-400:]
                                                                  or f"exit {r.returncode}"))
        (staging / SYSROOT_RECEIPT).write_text(
            json.dumps({"digest": image["sha1"], "format": values["format"]}, separators=(",", ":")))
        shutil.rmtree(sysroot_dir(inst), ignore_errors=True)
        staging.rename(sysroot_dir(inst))
    finally:
        shutil.rmtree(downloads, ignore_errors=True)
        Path(f"{staging}.system.img").unlink(missing_ok=True)
    if not sysroot_ready(inst, values):
        raise AxrbError("AXRB's make_sysroot.py finished but the system root is incomplete")


def direct_requirements() -> dict:
    """What direct mode needs from the PC: {memoryGB, vulkan, ok}. No hypervisor, no GPU vendor gate (AXRB's direct
    setup checks only memory); Vulkan = the Windows loader (axrb-direct.exe logs "vulkan-1.dll is not installed: the
    guest has no GPU" without it)."""
    r = winhost.powershell("[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)",
                           timeout=60)
    try:
        memory = float(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        memory = None
    vulkan = (Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "vulkan-1.dll" if winhost.is_windows()
              else Path(winhost.system32("vulkan-1.dll"))).is_file()
    return {"memoryGB": memory, "vulkan": vulkan,
            "ok": vulkan and (memory is None or memory >= DIRECT_MIN_MEMORY_GB)}


def direct_requirement_problems(info: dict) -> list[str]:
    out = []
    if info.get("memoryGB") is not None and info["memoryGB"] < DIRECT_MIN_MEMORY_GB:
        out.append(f"needs at least {DIRECT_MIN_MEMORY_GB} GB of memory ({info['memoryGB']} GB found)")
    if not info.get("vulkan"):
        out.append("needs a graphics driver with Vulkan (vulkan-1.dll was not found)")
    return out


# ------------------------------------------------------------------------------------------ scripts + adb
def script_env(root: Path | None = None, app: Path | None = None) -> dict[str, str]:
    """Environment every AXRB script needs when it runs without AXRB's launcher (Windows paths)."""
    root_win = winhost.to_windows(root or runtime_root())
    res_win = winhost.to_windows(resources(app))
    return {"AXRB_DATA_HOME": root_win + r"\output", "ANDROID_AVD_HOME": root_win + r"\avd",
            "ANDROID_USER_HOME": root_win + r"\android", "ANDROID_HOME": root_win + r"\sdk",
            "ANDROID_SDK_ROOT": root_win + r"\sdk", "ANDROID_ADB_SERVER_PORT": ADB_PORT, "ADB_USB_LEGACY": "1",
            "ADB_LOCAL_TRANSPORT_MAX_PORT": "5683", "FP_AXRB_PYTHON": res_win + r"\tools\python"}


def _ps_quote(s: str) -> str:
    return "'" + str(s).replace("'", "''") + "'"


def ps_command(script: str, args: dict, root: Path | None = None, app: Path | None = None,
               log_win: str | None = None) -> str:
    """A PowerShell command line that sets AXRB's environment and runs one of its scripts (its output appended to
    log_win when given)."""
    env = script_env(root, app)
    sets = "; ".join(f"$env:{k} = {_ps_quote(v)}" for k, v in env.items() if k != "FP_AXRB_PYTHON")
    path = f"$env:PATH = {_ps_quote(env['FP_AXRB_PYTHON'])} + ';' + $env:PATH; $env:ADB_SERVER_SOCKET = $null"
    parts = []
    for k, v in args.items():
        if v is True:
            parts.append(f"-{k}")
        elif v not in (None, False):
            parts.append(f"-{k} {_ps_quote(v)}")
    call = f"& {_ps_quote(winhost.to_windows(resources(app) / script))} {' '.join(parts)}"
    if log_win:
        return f"$ErrorActionPreference = 'Stop'; {sets}; {path}; {logged(call, log_win)}"
    return f"$ErrorActionPreference = 'Stop'; {sets}; {path}; {call}; exit $LASTEXITCODE"


EXIT_MARK = "FP_EXIT"


def logged(call: str, log_win: str) -> str:
    """A PowerShell call with all its output appended to a log, including the error that ended it (AXRB's scripts
    throw their failure reason), and a final "FP_EXIT <code>" line (the caller can't see the exit code of a process
    started through WMI)."""
    log = _ps_quote(log_win)
    # AXRB's run scripts end with `trap { [Console]::Error.WriteLine(<reason>); exit 1 }`: that bypasses PowerShell's
    # streams (*>> never sees it), so the console's error writer is swapped for the call and its text logged after
    return ("$fpErr = [IO.StringWriter]::new(); $fpOld = [Console]::Error; [Console]::SetError($fpErr); "
            f"try {{ {call} *>> {log}; $c = $(if ($LASTEXITCODE) {{ $LASTEXITCODE }} else {{ 0 }}) }} "
            f"catch {{ ($_ | Out-String) *>> {log}; $c = 1 }} finally {{ [Console]::SetError($fpOld) }}; "
            f"if ($fpErr.ToString()) {{ $fpErr.ToString() *>> {log} }}; \"{EXIT_MARK} $c\" *>> {log}; exit $c")


def exit_code(log: Path) -> int | None:
    found = re.findall(rf"^{EXIT_MARK} (-?\d+)\s*$", read_log(log, 1 << 16), re.M)
    return int(found[-1]) if found else None


def powershell_win() -> str:
    """powershell.exe as a Windows path."""
    if winhost.is_windows():
        return powershell_exe()
    root = winhost.env_path("SystemRoot")
    return (winhost.to_windows(root / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe") if root
            else r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe")


def spawn_detached(command: str) -> int:
    """Start a hidden PowerShell running `command` through WMI (Win32_Process.Create) and return its process id.
    Nothing is inherited from FramePort: the emulator and adb server an AXRB script leaves running would otherwise hold
    FramePort's pipes (or WSL's interop relay, which exists even for /dev/null) open, and the call would never end."""
    encoded = base64.b64encode(command.encode("utf-16-le")).decode()
    cmdline = (f'"{powershell_win()}" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden '
               f"-EncodedCommand {encoded}")
    ps = ("$si = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ShowWindow = [uint16]0}; "
          "$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments "
          f"@{{CommandLine = {_ps_quote(cmdline)}; CurrentDirectory = 'C:\\'; ProcessStartupInformation = $si}}; "
          '"$($r.ReturnValue) $($r.ProcessId)"')
    r = winhost.powershell(ps, timeout=90)
    last = (r.stdout.strip().splitlines() or [""])[-1].split()
    if len(last) != 2 or last[0] != "0" or not last[1].isdigit():
        raise AxrbError(f"could not start PowerShell for AXRB: {(r.stdout + r.stderr).strip()[-300:]}")
    return int(last[1])


def pid_running(pid: int) -> bool:
    try:
        r = winhost.run_win([winhost.system32("tasklist.exe"), "/FI", f"PID eq {pid}", "/NH", "/FO", "CSV"], timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return True  # unknown: keep waiting until the deadline
    return f'"{pid}"' in r.stdout


def powershell_exe() -> str:
    if winhost.is_windows():
        return str(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe")
    return winhost.system32("WindowsPowerShell/v1.0/powershell.exe")


def run_script(script: str, args: dict, log: Path, timeout: float, root: Path | None = None,
               app: Path | None = None) -> int:
    """Run an AXRB script detached (spawn_detached), its output in the log file, and wait for it. Returns its exit
    code (1 when it ended without saying)."""
    log.parent.mkdir(parents=True, exist_ok=True)
    log.unlink(missing_ok=True)
    pid = spawn_detached(ps_command(script, args, root, app, winhost.to_windows(log)))
    end = time.time() + timeout
    while pid_running(pid):
        if time.time() > end:
            raise AxrbError(f"AXRB's {Path(script).name} didn't finish within {timeout / 60:.0f} minutes")
        time.sleep(2)
    code = exit_code(log)
    return 1 if code is None else code


def adb(args: list[str], timeout: float = 60, serial: bool = True,
        root: Path | None = None) -> subprocess.CompletedProcess:
    exe = adb_exe(root)
    if not exe:
        raise AxrbError("AXRB's Android runtime is not set up")
    cmd = [str(exe), "-P", ADB_PORT] + (["-s", SERIAL] if serial else []) + args
    return subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True, errors="replace",
                          timeout=timeout)


def start_adb_server(root: Path | None = None) -> None:
    """Start AXRB's adb server ourselves with no inherited pipes (see run_script)."""
    exe = adb_exe(root)
    if exe:
        subprocess.run([str(exe), "-P", ADB_PORT, "start-server"], stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)


def emulator_online(root: Path | None = None) -> bool:
    try:
        r = adb(["devices"], timeout=20, serial=False, root=root)
    except (AxrbError, OSError, subprocess.TimeoutExpired):
        return False
    return bool(re.search(rf"^{SERIAL}\s+device\s*$", r.stdout, re.M))


def log_dir(root: Path | None = None) -> Path:
    return (root or runtime_root()) / "output" / "logs"


def read_log(path: Path, limit: int = 1 << 20, head: int = 0) -> str:
    """A log written on Windows: Windows PowerShell 5.1's redirection writes UTF-16 with a BOM, others UTF-8. Its last
    `limit` characters, and with `head` also its first ones (a long session's start-up lines: direct.out reached
    10 MB in 3 minutes of LEGO Bricktales, and its tail no longer had the milestones)."""
    text = _read_text(path)
    if head and len(text) > head + limit:
        return text[:head] + "\n[... FramePort: middle of the log left out ...]\n" + text[-limit:]
    return text[-limit:]


def _read_text(path: Path) -> str:
    try:
        data = path.read_bytes()
    except OSError:
        return ""
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        text = data.decode("utf-16", errors="replace")
    else:
        text = data.decode("utf-8-sig", errors="replace")
    return text


def start_emulator(root: Path | None = None, app: Path | None = None, timeout: float = 1200) -> bool:
    """Boot AXRB's AVD headless (AXRB's own Start action: clock adapter, GPU layer, guest checks). Returns True if
    this call started it (the caller then stops it again)."""
    if emulator_online(root):
        return False
    start_adb_server(root)
    log = log_dir(root) / "frameport-emulator-start.log"
    v = emulator_values(app)
    rc = run_script("scripts/emulator/windows_android_emulator.ps1",
                    {"Action": "Start", "Sdk": winhost.to_windows((root or runtime_root()) / "sdk"), "Avd": v["avd"],
                     "Port": PORT, "ApiLevel": v["api"], "Abi": "arm64-v8a", "MemoryMB": MEMORY_MB,
                     "CpuCores": CPU_CORES, "GuestClock": "TscCorrected", "GpuSharing": True}, log, timeout, root, app)
    if rc != 0 or not emulator_online(root):
        tail = read_log(log, 600)
        raise AxrbError(f"Android (AXRB's emulator) did not start: {tail.strip() or f'exit {rc}'}")
    return True


def stop_emulator(root: Path | None = None, wait: float = 300, app: Path | None = None) -> None:
    try:
        adb(["shell", "sync"], timeout=60, root=root)
        adb(["emu", "kill"], timeout=30, root=root)
    except (AxrbError, OSError, subprocess.TimeoutExpired):
        return
    lock = (root or runtime_root()) / "avd" / f"{emulator_values(app)['avd']}.avd" / "hardware-qemu.ini.lock"
    end = time.time() + wait  # shutdown writes a quick-boot snapshot: can take a while, never fatal
    while time.time() < end and (emulator_online(root) or lock.exists()):
        time.sleep(2)


def ensure_runtime_apk(root: Path | None = None, app: Path | None = None) -> None:
    """AXRB's OpenXR runtime inside Android (reinstalled when AXRB updated it), and AXRB's ready receipt."""
    root = root or runtime_root()
    want = runtime_hash(app)
    try:
        receipt = json.loads((root / "ready.json").read_text())
    except (OSError, ValueError):
        receipt = {}
    have = "package:" in adb(["shell", "pm", "path", RUNTIME_PKG], root=root).stdout
    if not have or receipt.get("runtimeHash") != want:
        r = adb(["install", "--no-incremental", "--force-queryable", "-r",
                 winhost.to_windows(resources(app) / RUNTIME_APK)], timeout=240, root=root)
        if "Success" not in r.stdout:
            raise AxrbError(f"AXRB's runtime didn't install: {(r.stdout + r.stderr).strip()[-300:]}")
    write_atomic(root / "ready.json", json.dumps({"version": 1, "runtimeHash": want, "completedAt": _now()}))


def requirements(app: Path | None = None) -> dict:
    """AXRB's own hardware check: {hypervisor, gpu, supportedGpu, memoryGB, x64} (+ ok)."""
    app = app or app_dir()
    if not app:
        return {}
    script = winhost.to_windows(resources(app) / "scripts" / "emulator" / "check_windows.ps1")
    r = winhost.powershell(f"& {_ps_quote(script)}", timeout=90)
    try:
        info = json.loads(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"error": (r.stdout + r.stderr).strip()[-300:]}
    info["ok"] = bool(info.get("hypervisor") and info.get("supportedGpu") and info.get("x64")
                      and (info.get("memoryGB") or 0) >= MIN_MEMORY_GB)
    return info


def requirement_problems(info: dict) -> list[str]:
    out = []
    if info.get("error"):
        return [f"AXRB's hardware check failed: {info['error']}"]
    if not info.get("x64"):
        out.append("needs a 64-bit (x64) Windows PC")
    if (info.get("memoryGB") or 0) < MIN_MEMORY_GB:
        out.append(f"needs at least {MIN_MEMORY_GB} GB of memory ({info.get('memoryGB')} GB found)")
    if not info.get("supportedGpu"):
        out.append(f"needs an NVIDIA or AMD graphics card ({info.get('gpu') or 'none found'})")
    if not info.get("hypervisor"):
        out.append("needs Windows Hypervisor Platform: turn it on in 'Turn Windows features on or off', then restart")
    return out


def stop_game(inst: Install, parent_pid: int) -> None:
    """Ask a running direct-mode session to end the way AXRB's launcher does (scripts/run/stop_game.ps1: close the
    bridge window that `parent_pid`'s run script started; the run script then lets the game pause, stop and save)."""
    script = inst.resources / STOP_SCRIPT
    if script.is_file():
        winhost.powershell(f"& {_ps_quote(winhost.to_windows(script))} -ParentPid {int(parent_pid)}", timeout=60)


def install_state(inst: Install) -> dict:
    """setup_state / direct_setup_state for one install."""
    if inst.backend == DIRECT:
        return direct_setup_state(inst)
    return setup_state(inst.runtime, inst.app)


def ready(backend: str | None = None) -> bool:
    """Whether an AXRB backend (any, when None) is installed and set up: nothing to download before an install."""
    for b in (backend,) if backend else BACKENDS:
        inst = find(b)
        try:
            if inst and install_state(inst)["ready"]:
                return True
        except (AxrbError, OSError, KeyError, ValueError):
            continue
    return False


def status() -> dict:
    """For `frameport pc info` / Settings: what is installed and set up, per backend (no downloads, no emulator
    start). The emulator's keys stay at the top level (older readers), the direct backend is under "direct"."""
    out = {}
    for backend in BACKENDS:
        inst = find(backend)
        root = inst.runtime if inst else runtime_root() if backend == EMULATOR else direct_runtime_root()
        info = {"installed": bool(inst), "version": inst.version if inst else None,
                "known_good": KNOWN_GOOD.get(backend), "app": str(inst.app) if inst else None,
                "runtime": str(root) if root else None, "local": bool(inst and inst.local)}
        if inst:
            try:
                info["setup"] = install_state(inst)
            except (AxrbError, OSError, KeyError, ValueError) as exc:
                info["setup"] = {"error": str(exc)}
        if backend == EMULATOR:
            out.update(info)
        else:
            out[backend] = info
    return out


# ------------------------------------------------------------------------------------------ the Play launcher
LAUNCHER = r"""param([Parameter(Mandatory)][string]$Package, [Parameter(Mandatory)][string]$Activity,
      [string]$GameName = $Package, [switch]$CaptureGuestLog, [switch]$PrecomposeProjectionLayers)
# Written by FramePort: starts a Quest game installed on this PC through AXRB's own run script, with AXRB's managed
# Android runtime (what AXRB's launcher passes it). AXRB boots the emulator when needed and shuts it down afterwards.
$ErrorActionPreference = 'Stop'
@ENV@
$env:ADB_SERVER_SOCKET = $null
$env:PATH = (Join-Path @RES@ 'tools\python') + ';' + $env:PATH
$run = Join-Path @RES@ 'scripts\run\run_windows_game.ps1'
# GLES games send their eye images as pixels. AXRB's default route (TCP to 10.0.2.2 through the emulator's user-mode
# network) carries ~8 MB/s, its adb reverse route ~120 MB/s (Lucky's Tale at 448x512 per eye: 1 -> 30 fps). The
# guest reads debug.axrb.image_transport when it connects, so Android is started here first (as AXRB's run script
# would; that script then owns it and shuts it down) and the property set. Vulkan games use shared GPU images.
# FRAMEPORT_AXRB_IMAGE_TRANSPORT=default leaves AXRB's own choice.
$transport = if ($env:FRAMEPORT_AXRB_IMAGE_TRANSPORT) { $env:FRAMEPORT_AXRB_IMAGE_TRANSPORT } else { 'adb' }
$owns = $false
if ($transport -ne 'default') {
    $adb = Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'
    $serial = 'emulator-@PORT@'
    if (((& $adb devices) -join "`n") -notmatch ('(?m)^' + [regex]::Escape($serial) + '\s+device\s*$')) {
        & (Join-Path @RES@ 'scripts\emulator\windows_android_emulator.ps1') -Action Start -Sdk $env:ANDROID_HOME `
            -Avd '@AVD@' -Port @PORT@ -ApiLevel @API@ -Abi arm64-v8a -MemoryMB @MEM@ -CpuCores @CORES@ `
            -GuestClock TscCorrected -GpuSharing:(Test-Path (Join-Path @RES@ 'out\gpu\Release\axrb_gpu_layer.json'))
        $owns = $true
    }
    & $adb -s $serial shell setprop debug.axrb.image_transport $transport
}
& $run -Avd '@AVD@' -Port @PORT@ -Sdk $env:ANDROID_HOME -MemoryMB @MEM@ -CpuCores @CORES@ -TextureMode gpu `
    -Package $Package -Activity $Activity -GameName $GameName -GuestClock Auto -CaptureGuestLog:$CaptureGuestLog `
    -PrecomposeProjectionLayers:$PrecomposeProjectionLayers -RuntimeApk (Join-Path @RES@ '@APK@') -OwnsEmulator:$owns
exit $LASTEXITCODE
"""

DIRECT_LAUNCHER = r"""param([Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9_.]+$')][string]$Package,
      [Parameter(Mandatory)][string]$Activity, [string]$GameName = $Package,
      [ValidateSet('dynarmic', 'digitalis')][string]$Translator = 'dynarmic', [switch]$PrecomposeProjectionLayers,
      [switch]$LegacyOculusDriver, [switch]$QuestIdentity, [switch]$ControllersOnly)
# Written by FramePort: starts a Quest game installed on this PC in AXRB's direct mode (axrb-direct.exe, no
# emulator) through AXRB's own run script, with what AXRB's launcher passes it. The game's APK and its content-file
# list are in games\<package> next to this script; its app data and saves in AXRB's runtime folder (direct\data,
# direct\sdcard), logs in output\logs\game.
$ErrorActionPreference = 'Stop'
$env:AXRB_DATA_HOME = @OUT@
$env:PYTHONUTF8 = '1'
$env:PATH = (Join-Path @RES@ 'tools\python') + ';' + $env:PATH
$game = Join-Path $PSScriptRoot "games\$Package"
$extra = @{}
if (Test-Path -LiteralPath (Join-Path $game 'assets.txt')) { $extra.AssetList = Join-Path $game 'assets.txt' }
& (Join-Path @RES@ 'scripts\run\run_direct_game.ps1') -Apk (Join-Path $game "$Package.apk") -Package $Package `
    -Activity $Activity -GameName $GameName -Executable (Join-Path @RES@ 'out\direct\axrb-direct.exe') `
    -DataDirectory @DATA@ -Sysroot (Join-Path @DATA@ 'sysroot') -Translator $Translator `
    -PrecomposeProjectionLayers:$PrecomposeProjectionLayers -LegacyOculusDriver:$LegacyOculusDriver `
    -QuestIdentity:$QuestIdentity -ControllersOnly:$ControllersOnly @extra
exit $LASTEXITCODE
"""


def launcher_text(root: Path | None = None, app: Path | None = None) -> str:
    env = script_env(root, app)
    sets = "\n".join(f"$env:{k} = {_ps_quote(v)}" for k, v in env.items() if k != "FP_AXRB_PYTHON")
    return (LAUNCHER.replace("@ENV@", sets).replace("@RES@", _ps_quote(winhost.to_windows(resources(app))))
            .replace("@AVD@", emulator_values(app)["avd"]).replace("@PORT@", str(PORT))
            .replace("@MEM@", str(MEMORY_MB)).replace("@CORES@", str(CPU_CORES))
            .replace("@API@", str(emulator_values(app)["api"]))
            .replace("@APK@", RUNTIME_APK.replace("/", "\\")))


def direct_launcher_text(inst: Install) -> str:
    return (DIRECT_LAUNCHER.replace("@RES@", _ps_quote(winhost.to_windows(inst.resources)))
            .replace("@OUT@", _ps_quote(winhost.to_windows(inst.runtime / "output")))
            .replace("@DATA@", _ps_quote(winhost.to_windows(direct_data(inst)))))


def launcher_dir() -> Path:
    """%LOCALAPPDATA%\\FramePort\\axrb: FramePort's launcher scripts and the direct-mode games' APKs (local path).
    FRAMEPORT_AXRB_LAUNCHER_DIR moves it (tests and isolated FRAMEPORT_HOME runs: the default is the real app data
    folder's, and Windows must be able to run what is in it, so a WSL-only FRAMEPORT_HOME can't hold it)."""
    if os.environ.get("FRAMEPORT_AXRB_LAUNCHER_DIR"):
        return Path(os.environ["FRAMEPORT_AXRB_LAUNCHER_DIR"])
    local = local_appdata()
    if not local:
        raise AxrbError("Windows' local app data folder was not found")
    return local / "FramePort" / "axrb"


def game_dir(package: str) -> Path:
    """Where a direct-mode game's APK (<package>.apk) and content-file list (assets.txt) are kept."""
    if not re.fullmatch(r"[A-Za-z0-9_.]+", package):
        raise AxrbError(f"unexpected package name {package!r}")
    return launcher_dir() / "games" / package


def _write_script(name: str, text: str) -> Path:
    path = launcher_dir() / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8-sig", errors="replace") != text:
        path.write_text(text, encoding="utf-8-sig")  # BOM: Windows PowerShell 5.1 reads BOM-less files as ANSI
    return path


def write_launcher(root: Path | None = None, app: Path | None = None) -> Path:
    """FramePort's launcher script in %LOCALAPPDATA%\\FramePort\\axrb (Steam shortcuts point at it). Local path."""
    return _write_script(LAUNCHER_NAME, launcher_text(root, app))


def write_direct_launcher(inst: Install) -> Path:
    return _write_script(DIRECT_LAUNCHER_NAME, direct_launcher_text(inst))


def launcher_args(launcher_win: str, package: str, activity: str, title: str, flags: list[str] | tuple = ()) -> str:
    """Steam shortcut launch options for powershell.exe (`flags`: the launcher's per-game switches)."""
    name = re.sub(r'["`$]', "", title).strip() or package
    return (f'-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{launcher_win}" '
            f'-Package {package} -Activity {activity} -GameName "{name}"' + "".join(f" {f}" for f in flags))
