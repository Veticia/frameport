"""Quest games on this PC through AXRB (tools/axrb.py, targets/pc_android.py) against fakes; no Windows needed."""
import hashlib
import io
import json
import os
import re
import struct
import subprocess
import zipfile
from pathlib import Path

import pytest
from test_pc_target import fake_steam, vdf_mod

from frameport.core import winhost
from frameport.core.events import Reporter
from frameport.core.models import Recipe
from frameport.tools import axrb


def make_asar(path: Path, files: dict[str, bytes]) -> None:
    """A minimal Electron asar: pickle sizes + JSON index + file data."""
    index, blob, offset = {"files": {}}, b"", 0
    for name, data in files.items():
        node = index
        *dirs, leaf = name.split("/")
        for d in dirs:
            node = node["files"].setdefault(d, {"files": {}})
        node["files"][leaf] = {"size": len(data), "offset": str(offset)}
        blob += data
        offset += len(data)
    js = json.dumps(index).encode()
    payload = struct.pack("<I", len(js)) + js
    payload += b"\0" * (-len(payload) % 4)
    header = struct.pack("<I", len(payload)) + payload
    path.write_bytes(struct.pack("<I", 4) + struct.pack("<I", len(header)) + header + blob)


def fake_app(tmp_path, comps) -> Path:
    app = tmp_path / "Programs" / "axrb-launcher"
    res = app / "resources" / "runtime"
    (res / "out/android/runtime-arm64-v8a").mkdir(parents=True)
    (res / axrb.RUNTIME_APK).write_bytes(b"runtime-apk")
    (app / "AXRB.exe").write_bytes(b"MZ")
    make_asar(app / "resources" / "app.asar", {"package.json": json.dumps({"version": "1.0.4"}).encode(),
                                                "core/components.json": json.dumps(comps).encode()})
    return app


def zip_with(prefix: str, probe: str) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(f"{prefix}/{probe}", b"x")
    return buf.getvalue()


def component(cid, prefix, probe, dest):
    data = zip_with(prefix, probe)
    return {"id": cid, "name": cid, "url": f"{axrb.DOWNLOAD_HOST}{cid}.zip", "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(), "destination": dest, "prefix": prefix, "probe": probe}, data


@pytest.fixture
def win(monkeypatch):
    monkeypatch.setattr(winhost, "available", lambda: True)
    monkeypatch.setattr(winhost, "is_wsl", lambda: False)
    monkeypatch.setattr(winhost, "to_windows", lambda p: "W:" + str(p).replace("/", "\\"))


def test_asar_read_and_version(tmp_path):
    app = fake_app(tmp_path, [])
    assert axrb.installed_version(app) == "1.0.4"
    assert axrb.asar_read(app / "resources" / "app.asar", "core/components.json") == b"[]"
    with pytest.raises(KeyError):
        axrb.asar_read(app / "resources" / "app.asar", "core/missing.json")


def test_components_only_from_googles_repository(tmp_path):
    good, _ = component("emulator", "emulator", "emulator.exe", "emulator")
    assert axrb.components(fake_app(tmp_path, [good]))[0]["id"] == "emulator"
    bad = dict(good, url="https://example.com/emulator.zip")
    with pytest.raises(axrb.AxrbError):
        axrb.components(fake_app(tmp_path / "b", [bad]))


def test_setup_files_writes_axrbs_layout(tmp_path, monkeypatch, win):
    emu, emu_zip = component("emulator", "emulator", "emulator.exe", "emulator")
    img, img_zip = component("image", "x86_64", "system.img", "system-images/android-36/google_apis/x86_64")
    app = fake_app(tmp_path, [emu, img])
    root = tmp_path / "AXRB Runtime"
    blobs = {emu["url"]: emu_zip, img["url"]: img_zip}

    def download(url, dest, progress=None, expected_sha256=None, expected_sha1=None):
        assert hashlib.sha256(blobs[url]).hexdigest() == expected_sha256
        dest.write_bytes(blobs[url])
        if progress:
            progress(1.0)
        return dest
    monkeypatch.setattr(axrb.cache, "download", download)
    st = axrb.setup_state(root, app)
    assert st["components"] == ["emulator", "image"] and not st["ready"]
    seen = []
    axrb.setup_files(lambda f, name: seen.append(name), root=root, app=app)
    assert (root / "sdk/emulator/emulator.exe").exists()
    assert (root / "sdk/system-images/android-36/google_apis/x86_64/system.img").exists()
    cfg = (root / "avd" / f"{axrb.AVD}.avd" / "config.ini").read_text()
    assert "hw.gpu.mode=host" in cfg and "image.sysdir.1=W:" in cfg and cfg.rstrip().endswith("PlayStore.enabled=no")
    assert f"path=W:{str(root).replace('/', chr(92))}\\avd\\{axrb.AVD}.avd" in \
        (root / "avd" / f"{axrb.AVD}.ini").read_text()
    assert json.loads((root / "license-acceptance.json").read_text())["license"] == "android-sdk-license"
    assert not list((root / "downloads").iterdir()) and seen
    st = axrb.setup_state(root, app)
    assert st["components"] == [] and st["avd"] and not st["runtime_apk"]  # the runtime APK goes in after boot
    (root / "ready.json").write_text(json.dumps({"runtimeHash": axrb.runtime_hash(app)}))
    assert axrb.setup_state(root, app)["ready"]


def test_ps_command_sets_axrbs_environment(tmp_path, win):
    app = fake_app(tmp_path, [])
    cmd = axrb.ps_command("scripts/emulator/windows_android_emulator.ps1",
                          {"Action": "Start", "Port": 5584, "GpuSharing": True, "ColdBoot": False},
                          tmp_path / "AXRB Runtime", app)
    assert "$env:ANDROID_ADB_SERVER_PORT = '5038'" in cmd and "$env:AXRB_DATA_HOME = 'W:" in cmd
    assert "tools\\python' + ';' + $env:PATH" in cmd
    assert "-Action 'Start' -Port '5584' -GpuSharing" in cmd and "ColdBoot" not in cmd
    assert cmd.endswith("exit $LASTEXITCODE")
    assert axrb._ps_quote("it's") == "'it''s'"


def test_launcher_script_and_steam_arguments(tmp_path, win):
    app = fake_app(tmp_path, [])
    text = axrb.launcher_text(tmp_path / "AXRB Runtime", app)
    assert "run_windows_game.ps1" in text and f"-Avd '{axrb.AVD}' -Port {axrb.PORT}" in text
    assert "@" not in text.replace("@{", "")  # every placeholder filled
    args = axrb.launcher_args(r"C:\x\fp-axrb-run.ps1", "com.a.b", "com.a.b/.Main", 'Say "Hi" $now')
    assert '-File "C:\\x\\fp-axrb-run.ps1" -Package com.a.b -Activity com.a.b/.Main -GameName "Say Hi now"' in args


def test_read_log_handles_powershell_utf16(tmp_path):
    p = tmp_path / "a.log"
    p.write_bytes(b"\xff\xfe" + "Host failed to start: x\n".encode("utf-16-le"))
    assert axrb.read_log(p) == "Host failed to start: x\n"
    p.write_bytes("﻿plain".encode())
    assert axrb.read_log(p, 3) == "ain"
    assert axrb.read_log(tmp_path / "missing.log") == ""


def test_logged_call_reports_its_exit_code(tmp_path, win):
    cmd = axrb.logged("& 'x.ps1' -A 1", r"C:\l.log")
    assert "*>> 'C:\\l.log'" in cmd and '"FP_EXIT $c"' in cmd and cmd.endswith("exit $c")
    # AXRB's trap writes its failure reason straight to the console's stderr: captured and logged too
    assert cmd.index("[Console]::SetError($fpErr)") < cmd.index("x.ps1") < cmd.index("$fpErr.ToString() *>>")
    log = tmp_path / "l.log"
    log.write_bytes(b"\xff\xfe" + "output\nFP_EXIT 4\n".encode("utf-16-le"))
    assert axrb.exit_code(log) == 4
    log.write_text("no marker")
    assert axrb.exit_code(log) is None


def test_release_sums_and_requirements():
    sums = axrb.parse_sums("310d" + "0" * 60 + "  AXRB-Setup-1.0.4.exe\nnot a line\n")
    assert sums == {"AXRB-Setup-1.0.4.exe": "310d" + "0" * 60}
    ok = {"hypervisor": True, "supportedGpu": True, "x64": True, "memoryGB": 64, "gpu": "RTX"}
    assert axrb.requirement_problems(ok) == []
    bad = axrb.requirement_problems(dict(ok, hypervisor=False, memoryGB=8))
    assert len(bad) == 2 and "Hypervisor" in bad[1] and "12 GB" in bad[0]


def test_pc_selection_keeps_ovrport_and_apk_fixes_only():
    from frameport import pipeline
    from frameport.patches.base import pc_selection

    patches = {"patch_oculus_unity": {}, "frame.ovrstubs": {}, "frame.swapchain_limit": {}, "frame.vk_sanitize": {},
               "frame.gl_shim": {}, "frame.sdl_clipboard": {}, "frame.a32_bridge": {}, "device.files": {},
               "frame.hw_video_decode": {}, "frame.ovr_stable_user": {}, "frame.launcher": {}}
    assert set(pc_selection(patches)) == {"patch_oculus_unity", "frame.ovrstubs", "frame.swapchain_limit",
                                          "frame.vk_sanitize", "frame.ovr_stable_user", "frame.launcher"}
    r1 = {"patches": patches}
    r2 = {"patches": dict(patches, **{"frame.gl_shim": {"x": 1}, "device.hide_navbar": {}})}  # Frame-only changes
    assert pipeline.pc_recipe_fingerprint(r1) == pipeline.pc_recipe_fingerprint(r2)
    assert pipeline.recipe_fingerprint(r1) != pipeline.recipe_fingerprint(r2)
    r3 = {"patches": dict(patches, **{"frame.unity_text_input": {}})}  # an APK fix both builds get
    assert pipeline.pc_recipe_fingerprint(r1) != pipeline.pc_recipe_fingerprint(r3)


PC_FIXES = ["frame.ovr_stable_user", "frame.unreal_ovrp_entrypoints", "frame.avatar_stub", "frame.vrapi_stub",
            "frame.metaxr_telemetry", "frame.oculusos", "frame.ovr_microphone", "frame.langpacks",
            "frame.asset_files", "frame.asset_files_stub", "frame.ovr_trace", "frame.blazerush",
            "frame.unity_oculus_check", "frame.unity_text_input", "frame.unreal_quest_precompile",
            "frame.unreal_quest_keymap", "frame.unreal_thumb_touch", "frame.unreal_skip_obb_check",
            "frame.unreal_skip_obb_delivery", "frame.nodebug", "frame.meta_permissions", "frame.launcher",
            "frame.adapter", "frame.ovrplatformcompat", "frame.ovrstubs", "frame.swapchain_limit"]
PC_EXPERIMENTAL = ["frame.vk_sanitize", "frame.vrapi_bridge", "frame.unity_tls_key_pool", "frame.eos_keychain_loop"]
FRAME_ONLY = ["frame.gl_shim", "frame.unity_gl_shim", "frame.unreal_gl_shim", "frame.sdl_clipboard",
              "frame.godot_clipboard", "frame.vivox_audio_route", "frame.a32_bridge", "frame.hw_video_decode",
              "frame.zink_shader_fix", "frame.gl_multiview_fbo", "device.files", "device.hide_navbar",
              "device.steam_gamepad", "device.text_input_window"]


def test_pc_selection_patch_lists():
    from frameport.patches import base

    base.load_all()
    for pid in PC_FIXES:
        assert base.get(pid).on_pc is True, pid
    for pid in PC_EXPERIMENTAL:
        assert base.get(pid).on_pc == "experimental", pid
    for pid in FRAME_ONLY:
        assert not base.get(pid).on_pc, pid
    patches = {pid: {} for pid in PC_FIXES + PC_EXPERIMENTAL + FRAME_ONLY}
    sel = base.pc_selection(patches)
    assert set(PC_FIXES + PC_EXPERIMENTAL) <= set(sel)
    assert not set(FRAME_ONLY) & set(sel)
    assert base.pc_selection(sel) == sel  # idempotent: the PC target records pc_selection of the PC recipe


def test_pc_selection_adapter_settings():
    from frameport.patches import base
    from frameport.patches.frame.adapter import settings_text
    from frameport.patches.settings import PC_DENY

    patches = {"frame.adapter": {}, "adapter.scale": {"value": 0.8}, "adapter.haptic_fix": {"value": 1},
               "adapter.pose_time_fix": {"value": 1}, "adapter.rect_clamp": {"value": 1},
               "adapter.controller_fix": {"value": 0}, "adapter.sync_guard": {"value": 1},
               "adapter.focus_hold": {"value": 1}, "adapter.focus_hold_ms": {"value": 2500.0},
               "adapter.controller_models": {"value": 1}, "adapter.gl_hide_msrtt": {"value": 1}}
    sel = base.pc_selection(patches)
    for key in ("scale", "haptic_fix", "pose_time_fix", "rect_clamp", "controller_fix"):
        assert f"adapter.{key}" in sel, key
    for key in ("sync_guard", "focus_hold_ms", "controller_models", "gl_hide_msrtt"):
        assert key in PC_DENY and f"adapter.{key}" not in sel, key
    assert sel["adapter.focus_hold"] == {"value": 0}  # FrameBridge's built-in default is on: off on the PC
    text = settings_text(sel).decode()
    assert "scale=0.8\n" in text and "focus_hold=0\n" in text and "sync_guard" not in text
    assert "controller_models" not in text and "focus_hold_ms" not in text
    # no FrameBridge in the selection: no adapter settings forced
    assert base.pc_selection({"adapter.scale": {"value": 0.8}}) == {"adapter.scale": {"value": 0.8}}


def test_pc_only_telemetry_patch():
    from frameport.core.models import Analysis
    from frameport.patches import base

    telemetry = "patch_disable_meta_xr_audio_telemetry"
    def analysis(libs):
        return Analysis(package="p", version="1", label="P", abis=["arm64-v8a"], engine="Unity", xr="OpenXR",
                        graphics="Vulkan", direct_vrapi=False, libs=libs, launcher_activity=None,
                        has_info_category=False, meta_permissions=[], uses_glad_gl=False, unity_msaa_levels=0,
                        oculus_os_classes=False, is_overport_output=False, debuggable=False, extra={})

    audio = analysis(["libmetaxraudio.so", "libOVRPlugin.so"])
    plain = analysis(["libOVRPlugin.so"])
    assert not base.get(telemetry).applies(audio)  # never in a Steam Frame recipe
    assert telemetry in base.pc_selection({"patch_oculus_unity": {}}, audio)
    assert telemetry not in base.pc_selection({"patch_oculus_unity": {}}, plain)
    assert telemetry not in base.pc_selection({"patch_oculus_unity": {}})  # no analysis: only the recipe
    sel = base.pc_selection({"patch_oculus_unity": {}}, audio)
    assert base.pc_selection(sel) == sel  # kept when the PC target filters the PC recipe again


def test_pc_build_extracts_native_libs(tmp_path):
    from frameport import build
    from frameport.apk import axml

    class WS:
        def __init__(self, manifest):
            self.files = {"AndroidManifest.xml": manifest}

        def read(self, name):
            return self.files[name]

        def put(self, name, data):
            self.files[name] = data

    manifest = _manifest(extract=False)
    ws = WS(manifest)
    assert build.extract_native_libs(ws)
    assert axml.Axml(ws.files["AndroidManifest.xml"]).get_bool("application", "extractNativeLibs") is True
    assert not build.extract_native_libs(ws)  # already on
    ws = WS(_manifest(extract=None))  # no attribute: Android's default is to extract
    assert not build.extract_native_libs(ws)


def _manifest(extract):
    from conftest import build_axml

    attrs = [] if extract is None else [("extractNativeLibs", "bool", extract)]
    return build_axml([("start", "manifest", [("package", "str", "com.example.game")]),
                       ("start", "application", attrs), ("end", "application"), ("end", "manifest")])


def test_pc_installable():
    from frameport.pipeline import pc_installable

    assert pc_installable({"kind": "rift"})
    assert pc_installable({"analysis": {"extra": {"vr_kind": "quest"}}})
    assert pc_installable({"analysis": {"extra": {"vr_kind": "openxr"}}})
    assert pc_installable({})  # older entries without vr_kind are Quest games
    assert not pc_installable({"analysis": {"extra": {"vr_kind": "none"}}})
    assert not pc_installable({"kind": "linux"})


def test_shared_exe_shortcuts_are_matched_by_package(tmp_path):
    m = vdf_mod()
    vdf = str(tmp_path / "shortcuts.vdf")
    exe = '"C:\\Windows\\powershell.exe"'
    a = m.upsert_shortcut(vdf, exe, "Game A", "", "", "FramePort PC VR", "-Package com.a -Activity x",
                          options_key="-Package com.a ")
    b = m.upsert_shortcut(vdf, exe, "Game B", "", "", "FramePort PC VR", "-Package com.b -Activity y",
                          options_key="-Package com.b ")
    again = m.upsert_shortcut(vdf, exe, "Game A (renamed)", "", "", "FramePort PC VR", "-Package com.a -Activity x",
                              options_key="-Package com.a ")
    sc = m.vdf_decode(Path(vdf).read_bytes())["shortcuts"]
    assert a != b and again == a and len(sc) == 2
    assert m.remove_shortcut(vdf, exe, "-Package com.a ")
    left = list(m.vdf_decode(Path(vdf).read_bytes())["shortcuts"].values())
    assert [v["appname"] for v in left] == ["Game B"]


class FakeAdb:
    def __init__(self, remote=None):
        self.calls, self.remote = [], remote or {}

    def __call__(self, args, timeout=60, serial=True, root=None):
        self.calls.append(args)
        out = "Success\n"
        if args[:2] == ["shell", "cmd"]:
            out = "priority=0 preferredOrder=0\ncom.game.x/com.unity3d.player.UnityPlayerActivity\n"
        elif args[0] == "shell" and "find . -type f" in args[1]:
            out = "".join(f"{s} ./{n}\n" for n, s in self.remote.items())
        return subprocess.CompletedProcess(args, 0, out, "")


def test_install_into_axrb(tmp_path, monkeypatch, win):
    from frameport.targets import pc_android

    apk = tmp_path / "out" / "com.game.x.apk"
    apk.parent.mkdir()
    apk.write_bytes(b"apk")
    data = tmp_path / "obb"
    data.mkdir()
    (data / "main.1.com.game.x.obb").write_bytes(b"1" * 10)
    (data / "patch.1.com.game.x.obb").write_bytes(b"2" * 5)
    adb = FakeAdb(remote={"main.1.com.game.x.obb": 10})  # already there from an earlier install
    events = []
    inst = axrb.Install(axrb.EMULATOR, "1.0.4", tmp_path / "app", tmp_path / "app/resources/runtime", tmp_path / "rt")
    monkeypatch.setattr(pc_android, "ensure_axrb", lambda rep, backend: inst)
    monkeypatch.setattr(axrb, "start_emulator", lambda **k: events.append("start") or True)
    monkeypatch.setattr(axrb, "stop_emulator", lambda *a, **k: events.append("stop"))
    monkeypatch.setattr(axrb, "ensure_runtime_apk", lambda **k: events.append("runtime"))
    monkeypatch.setattr(axrb, "installed_version", lambda app=None: "1.0.4")
    monkeypatch.setattr(axrb, "write_launcher", lambda **k: tmp_path / "FramePort/axrb/fp-axrb-run.ps1")
    monkeypatch.setattr(axrb, "adb", adb)
    monkeypatch.setattr(pc_android, "powershell_win", lambda: r"C:\Windows\powershell.exe")
    recipe = Recipe("com.game.x", {"patch_oculus_unity": {}, "frame.gl_shim": {}, "frame.ovrstubs": {}})
    res = pc_android.install("com.game.x", "Game X", apk, data, recipe, Reporter(), record_dir=tmp_path / "rec",
                             appid=lambda exe, title: 0x80001234)
    assert res == {"ok": True, "appid": 0x80001234, "activity": "com.game.x/com.unity3d.player.UnityPlayerActivity",
                   "backend": "emulator"}  # no analysis (GLES or unknown): the emulator
    assert events == ["start", "runtime", "stop"]
    pushes = [c for c in adb.calls if c[0] == "push"]
    assert len(pushes) == 1 and pushes[0][2] == "/sdcard/Android/obb/com.game.x/patch.1.com.game.x.obb"
    dep = json.loads((tmp_path / "rec" / "deployment.json").read_text())
    assert dep["kind"] == "android" and dep["recipe"]["patches"] == ["frame.ovrstubs", "patch_oculus_unity"]
    assert dep["backend"] == "emulator" and dep["flags"] == []
    exe, start, opts = pc_android.shortcut_fields(dep)
    assert exe == '"C:\\Windows\\powershell.exe"' and "-Package com.game.x " in opts
    assert start.endswith('axrb\\"')


def test_signature_conflict_explained():
    from frameport.targets import pc_android

    r = subprocess.CompletedProcess([], 1, "", "Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: ...]")
    with pytest.raises(RuntimeError, match="signed with another key"):
        pc_android._adb_ok(r, "installing the game")


def test_two_quest_games_get_their_own_pc_shortcuts(tmp_path, monkeypatch, win):
    from frameport.targets import pc_revive

    root = fake_steam(tmp_path)
    monkeypatch.setattr(winhost, "env_path", lambda name: None)
    monkeypatch.setattr(winhost, "steam_root", lambda: root)
    monkeypatch.setattr(winhost, "stop_steam", lambda r: True)
    monkeypatch.setattr(winhost, "start_steam", lambda r: None)
    monkeypatch.setattr("frameport.artwork.fetch.cache.http_get", lambda *a, **k: (_ for _ in ()).throw(OSError()))
    for pkg, title in (("com.a.game", "Game A"), ("com.b.game", "Game B")):
        d = pc_revive.pc_dir() / pkg
        d.mkdir(parents=True)
        (d / "deployment.json").write_text(json.dumps({
            "package": pkg, "kind": "android", "title": title, "activity": f"{pkg}/.Main",
            "launcher_win": r"C:\Users\u\AppData\Local\FramePort\axrb\fp-axrb-run.ps1",
            "powershell_win": r"C:\Windows\powershell.exe"}))
    t = pc_revive.PcReviveTarget()
    assert {d["package"] for d in t.installed()} == {"com.a.game", "com.b.game"}
    t.add_to_library(["com.a.game", "com.b.game"], Reporter())
    sc = vdf_mod().vdf_decode((root / "userdata/12345/config/shortcuts.vdf").read_bytes())["shortcuts"]
    assert sorted(v["appname"] for v in sc.values()) == ["Game A", "Game B"]
    assert all(v["tags"]["0"] == pc_revive.TAG for v in sc.values())


def test_pc_outdated_for_android_follows_the_pc_build():
    from frameport.ui import components as C

    dep = {"kind": "android", "recipe": {"patches": ["frame.ovrstubs", "patch_oculus_unity"]}}
    g = {"recipe": {"patches": {"patch_oculus_unity": {}, "frame.ovrstubs": {}, "frame.gl_shim": {}}}}
    assert not C.pc_outdated(g, dep)  # Frame-only patches don't matter on the PC
    g["recipe"]["patches"]["patch_oculus_unreal"] = {}
    assert C.pc_outdated(g, dep)


def test_axrb_triage_uses_its_own_signatures():
    from frameport.validate.triage import triage

    host = ("AXRB OpenXR: adjacent loader \"openxr_loader.dll\": loaded (Windows error 0)\n"
            "AXRB OpenXR: failed to find D3D11 adapter requested by OpenXR runtime\n"
            "Host pose server did not become ready within 45 seconds.\n")
    r = triage(host, "NEVER_STARTED", "com.zenstudios.PFXVRQuest", kind="axrb")
    assert {f.id for f in r.findings} == {"axrb-no-headset", "axrb-host-not-ready"}
    guest = ("I OpenXR-Loader: Got runtime: package: com.axrb.openxrruntime, so filename: libopenxr_runtime.so\n"
             "I OVRPlugin: Preinitialize: xrCreateInstance() succeeded\n"
             "F DEBUG   :       #00 pc 000000000006fb32  /vendor/lib64/hw/vulkan.ranchu.so "
             "(gfxstream::vk::ResourceTracker::on_vkAllocateMemory(void*)+802)\n")
    r = triage(guest, "EXITED", "com.zenstudios.PFXVRQuest", kind="axrb")
    assert [f.id for f in r.findings] == ["axrb-gfxstream-crash"] and r.findings[0].report
    assert r.milestone == "OpenXR instance created"
    assert not [f for f in triage(host, "EXITED", "com.zenstudios.PFXVRQuest").findings if f.id.startswith("axrb")]


# ------------------------------------------------------------------------------------------ direct mode (2.x)
DIRECT_MJS = """export const directSystemImage = {
  id: 'direct-system', name: 'Android 16 ARM64 system libraries',
  url: 'https://dl.google.com/android/repository/sys-img/android/arm64-v8a-36_r09.zip',
  size: %d, sha1: '%s',
};
// Bump when make_sysroot.py changes what it writes.
const SYSROOT_FORMAT = 3;
export const directSystemFiles = ['system/bin/app_process64',
  'system/lib64/libandroid.so'];
"""
SETUP_MJS = """const systemImageDirectory = sdk => path.join(sdk, 'system-images/android-37/google_apis/x86_64');
const managedAvd = 'axrb-managed-api37';
"""


def fake_direct_root(root: Path) -> Path:
    """A folder laid out like AXRB-Emulatorless' resources/runtime (or its source tree after build_direct.ps1)."""
    for name in axrb.DIRECT_RUNTIME_FILES:
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(b"x")
    (root / "tools/python").mkdir(parents=True)
    (root / "tools/python/python.exe").write_bytes(b"MZ")
    return root


def fake_direct_app(local: Path, files: dict | None = None, version="2.0.0") -> Path:
    app = local / "Programs" / "axrb-emulatorless-launcher"
    fake_direct_root(app / "resources" / "runtime")
    (app / "AXRB-Emulatorless.exe").write_bytes(b"MZ")
    make_asar(app / "resources" / "app.asar", {"package.json": json.dumps({"version": version}).encode(),
                                                **(files or {})})
    return app


@pytest.fixture
def no_axrb_env(monkeypatch):
    for name in ("FRAMEPORT_AXRB_DIR", "FRAMEPORT_AXRB_RUNTIME", "FRAMEPORT_AXRB_DIRECT_DIR",
                 "FRAMEPORT_AXRB_DIRECT_RUNTIME", "FRAMEPORT_AXRB_LAUNCHER_DIR"):
        monkeypatch.delenv(name, raising=False)


def test_launcher_dir_can_be_moved(tmp_path, monkeypatch, no_axrb_env):
    """Isolated runs (FRAMEPORT_HOME in WSL) keep launchers and direct-mode APKs out of the real app data folder."""
    monkeypatch.setattr(axrb, "local_appdata", lambda: tmp_path / "Local")
    assert axrb.launcher_dir() == tmp_path / "Local/FramePort/axrb"
    monkeypatch.setenv("FRAMEPORT_AXRB_LAUNCHER_DIR", str(tmp_path / "iso"))
    assert axrb.launcher_dir() == tmp_path / "iso"
    assert axrb.game_dir("com.example.game") == tmp_path / "iso/games/com.example.game"


def test_both_installs_are_found(tmp_path, monkeypatch, win, no_axrb_env):
    local = tmp_path / "Local"
    emu = fake_app(local, [])
    direct = fake_direct_app(local)
    monkeypatch.setattr(axrb, "local_appdata", lambda: local)
    monkeypatch.setattr(winhost, "env_path", lambda name: None)
    e, d = axrb.find(axrb.EMULATOR), axrb.find(axrb.DIRECT)
    assert (e.version, e.resources, e.runtime, e.local) == ("1.0.4", emu / "resources/runtime", local / "AXRB Runtime",
                                                             False)
    assert (d.version, d.resources, d.runtime) == ("2.0.0", direct / "resources/runtime",
                                                   local / "AXRB-Emulatorless Runtime")
    assert d.python == direct / "resources/runtime/tools/python/python.exe" and not d.local
    assert set(axrb.installs()) == {"emulator", "direct"}
    # the runtime folder AXRB's launcher was set up with wins
    roaming = tmp_path / "Roaming"
    (roaming / "AXRB-Emulatorless").mkdir(parents=True)
    (roaming / "AXRB-Emulatorless/library.json").write_text(json.dumps({"settings": {"managedDirectory": r"E:\AXRB"}}))
    monkeypatch.setattr(winhost, "env_path", lambda name: roaming if name == "APPDATA" else None)
    monkeypatch.setattr(winhost, "to_local", lambda w: Path("/mnt/e/AXRB"))
    assert axrb.find(axrb.DIRECT).runtime == Path("/mnt/e/AXRB")
    status = axrb.status()
    assert status["installed"] and status["version"] == "1.0.4" and status["direct"]["version"] == "2.0.0"
    assert status["direct"]["setup"]["ready"] is False and status["known_good"] == "1.0.4"


def test_local_direct_build(tmp_path, monkeypatch, no_axrb_env):
    """FRAMEPORT_AXRB_DIRECT_DIR: AXRB's source tree after build_direct.ps1 (or its out/direct folder)."""
    src = fake_direct_root(tmp_path / "AXRB")
    (src / "launcher/core").mkdir(parents=True)
    (src / "launcher/package.json").write_text(json.dumps({"version": "2.0.0"}))
    (src / "launcher/core/direct.mjs").write_text(DIRECT_MJS % (5, "ab" * 20))
    monkeypatch.setenv("FRAMEPORT_AXRB_DIRECT_RUNTIME", str(tmp_path / "rt"))
    for given in (src, src / "out" / "direct"):
        monkeypatch.setenv("FRAMEPORT_AXRB_DIRECT_DIR", str(given))
        d = axrb.find(axrb.DIRECT)
        assert d.resources == src and d.local and d.version == "2.0.0" and d.runtime == tmp_path / "rt"
        assert axrb.direct_values(d)["image"]["sha1"] == "ab" * 20
    monkeypatch.setenv("FRAMEPORT_AXRB_DIRECT_DIR", str(tmp_path / "nothing"))
    assert axrb.find(axrb.DIRECT) is None
    (src / axrb.DIRECT_EXE).unlink()  # an incomplete build is not one
    monkeypatch.setenv("FRAMEPORT_AXRB_DIRECT_DIR", str(src))
    assert axrb.find(axrb.DIRECT) is None


def test_values_come_from_the_installed_axrb(tmp_path):
    app = fake_app(tmp_path, [])
    assert axrb.emulator_values(app) == {"avd": axrb.AVD, "api": 36, "tag": "google_apis", "abi": "x86_64"}
    make_asar(app / "resources/app.asar", {"package.json": b"{}", "core/setup.mjs": SETUP_MJS.encode()})
    v = axrb.emulator_values(app)
    assert v == {"avd": "axrb-managed-api37", "api": 37, "tag": "google_apis", "abi": "x86_64"}
    cfg = axrb.avd_config(r"C:\img", values=v)
    assert "AvdId=axrb-managed-api37" in cfg and "target=android-37" in cfg
    direct = fake_direct_app(tmp_path / "L", {"core/direct.mjs": (DIRECT_MJS % (1_234, "CD" * 20)).encode()})
    d = axrb.direct_values(direct)
    assert d["image"]["size"] == 1234 and d["image"]["sha1"] == "cd" * 20 and d["format"] == 3
    assert d["image"]["url"].endswith("arm64-v8a-36_r09.zip")
    assert d["files"] == ("system/bin/app_process64", "system/lib64/libandroid.so")
    assert axrb.direct_values(tmp_path / "none")["image"] == axrb.DIRECT_IMAGE  # FramePort's copy of 2.0.0's
    evil = fake_direct_app(tmp_path / "E", {"core/direct.mjs": (DIRECT_MJS % (1, "ab" * 20)).replace(
        "https://dl.google.com/android/repository/", "https://example.com/").encode()})
    with pytest.raises(axrb.AxrbError):
        axrb.direct_values(evil)


def test_axrbs_compatibility_profiles(tmp_path):
    profiles = {"schemaVersion": 1, "profiles": [
        {"id": "climb", "package": "com.crytek.climb2", "versions": [{"versionName": "2.2"}],
         "runtime": {"precomposeProjectionLayers": True}},
        {"id": "ironman", "package": "com.camouflaj.salmon", "versions": [{"versionCode": "443305"}],
         "runtime": {"legacyOculusDriver": True, "questIdentity": True, "controllersOnly": True}}]}
    app = fake_direct_app(tmp_path, {"core/game-compatibility.json": json.dumps(profiles).encode()})
    assert axrb.compatibility_runtime(app, "com.crytek.climb2", "2.2", "1") == {"precomposeProjectionLayers": True}
    assert axrb.compatibility_runtime(app, "com.crytek.climb2", "2.1", "1") == {}
    assert axrb.compatibility_runtime(app, "com.camouflaj.salmon", None, "443305")["legacyOculusDriver"]
    assert axrb.compatibility_runtime(tmp_path / "none", "com.crytek.climb2", "2.2") == {}


def release(tag, *names):
    return {"tag_name": tag, "assets": [{"name": n, "browser_download_url": f"https://github.com/dl/{n}"}
                                        for n in (*names, f"SHA256SUMS-{tag}.txt")]}


def test_release_per_backend_and_known_good_fallback(monkeypatch):
    two = release("v2.0.1", "AXRB-Emulatorless-Setup-2.0.1.exe")
    old = release("v1.0.4", "AXRB-Setup-1.0.4.exe")
    asked = []

    def cached(name, url, max_age=0, fallback=None):
        asked.append(url)
        return two if url == axrb.AXRB_RELEASE else old if url.endswith("/tags/v1.0.4") else None
    monkeypatch.setattr(axrb.cache, "cached_json", cached)
    assert axrb.latest(axrb.DIRECT)["setup"] == "AXRB-Emulatorless-Setup-2.0.1.exe"
    assert asked == [axrb.AXRB_RELEASE]
    emu = axrb.latest(axrb.EMULATOR)  # the latest has no emulator installer: the known-good release
    assert emu["version"] == "1.0.4" and emu["setup"] == "AXRB-Setup-1.0.4.exe"
    assert axrb.release_info(dict(two, prerelease=True), axrb.DIRECT) is None


def test_upgrade_decision():
    up = axrb.upgrade_needed
    assert up("emulator", "1.0.3", "1.0.4")  # below the known-good release: always
    assert not up("emulator", "1.0.4", "1.0.5")  # newer release: only when asked
    assert up("emulator", "1.0.4", "1.0.5", update=True)
    assert not up("emulator", "1.0.5", "1.0.4", update=True)
    assert not up("direct", "2.0.0", "2.0.0", update=True)
    assert up("direct", "2.0.0", "2.0.1", update=True) and not up("direct", "2.0.0", "2.0.1")
    assert not up("emulator", None, "1.0.4") and up("emulator", None, "1.0.4", update=True)


def test_install_reruns_the_installer_when_older(tmp_path, monkeypatch, win):
    versions = ["1.0.3"]

    def find(backend=axrb.EMULATOR):
        return axrb.Install(backend, versions[-1], tmp_path, tmp_path, tmp_path) if versions else None
    sums = "".join(f"{'ab' * 32}  {n}\n" for n in ("AXRB-Setup-1.0.4.exe",))
    ran = []
    monkeypatch.setattr(axrb, "find", find)
    monkeypatch.setattr(axrb, "latest", lambda backend=axrb.EMULATOR: axrb.release_info(
        release("v1.0.4", "AXRB-Setup-1.0.4.exe"), backend))
    monkeypatch.setattr(axrb.cache, "http_get", lambda url, timeout=0: type("R", (), {"text": sums})())
    monkeypatch.setattr(winhost, "env_path", lambda name: tmp_path)
    monkeypatch.setattr(axrb.cache, "download", lambda url, dest, progress=None, expected_sha256=None: dest)
    monkeypatch.setattr(axrb.subprocess, "run", lambda args, **k: ran.append(args) or versions.append("1.0.4"))
    assert axrb.install(wait=5).version == "1.0.4" and ran and ran[0][1] == "/S"
    ran.clear()
    assert axrb.install(wait=5).version == "1.0.4" and not ran  # current: nothing to do, no release lookup
    monkeypatch.setattr(axrb, "latest", lambda backend=axrb.EMULATOR: None)
    assert axrb.install(wait=5, update=True).version == "1.0.4"  # offline: keep what is there
    versions.clear()
    with pytest.raises(axrb.AxrbError, match="FRAMEPORT_AXRB_DIRECT_DIR"):
        axrb.install(wait=5, backend=axrb.DIRECT)


def test_local_build_is_never_replaced(tmp_path, monkeypatch):
    local = axrb.Install(axrb.DIRECT, None, tmp_path, tmp_path, tmp_path, local=True)
    monkeypatch.setattr(axrb, "find", lambda backend=axrb.EMULATOR: local)
    monkeypatch.setattr(axrb, "latest", lambda backend=axrb.EMULATOR: pytest.fail("no release lookup"))
    assert axrb.install(backend=axrb.DIRECT, update=True) is local


def direct_install(tmp_path, files=None) -> axrb.Install:
    app = fake_direct_app(tmp_path / "Local", files)
    return axrb.Install(axrb.DIRECT, "2.0.0", app, app / "resources/runtime",
                        tmp_path / "Local/AXRB-Emulatorless Runtime")


def test_direct_setup_builds_the_sysroot_with_axrbs_script(tmp_path, monkeypatch, win):
    data = zip_with("arm64-v8a", "system.img")
    inst = direct_install(tmp_path, {"core/direct.mjs": (DIRECT_MJS % (len(data), hashlib.sha1(data).hexdigest()))
                                     .encode()})
    st = axrb.direct_setup_state(inst)
    assert st["components"] == ["direct-system"] and st["download_bytes"] == len(data) and not st["ready"]
    calls = []

    def download(url, dest, progress=None, expected_sha256=None, expected_sha1=None):
        assert url.endswith("arm64-v8a-36_r09.zip") and expected_sha1 == hashlib.sha1(data).hexdigest()
        assert list(dest.parent.iterdir()) == []  # a folder of its own
        dest.write_bytes(data)
        return dest

    def run_win(args, timeout=30):
        calls.append(args)
        staging = Path(args[3][2:].replace("\\", "/"))  # "W:<local path>" from the test's to_windows
        for f in ("system/bin/app_process64", "system/lib64/libandroid.so"):
            (staging / f).parent.mkdir(parents=True, exist_ok=True)
            (staging / f).write_bytes(b"elf")
        return subprocess.CompletedProcess(args, 0, "", "")
    monkeypatch.setattr(axrb.cache, "download", download)
    monkeypatch.setattr(winhost, "run_win", run_win)
    monkeypatch.setattr(axrb.shutil, "disk_usage", lambda p: type("U", (), {"free": 1 << 40})())
    axrb.setup_direct(inst)
    python, script, archive, staging, art = calls[0]
    assert python == str(inst.python) and script.endswith("direct\\tools\\make_sysroot.py") and art == "--art"
    assert archive.endswith("direct-system.zip") and staging.endswith("direct\\sysroot.new")
    sysroot = inst.runtime / "direct" / "sysroot"
    assert json.loads((sysroot / ".axrb-sysroot.json").read_text()) == {"digest": hashlib.sha1(data).hexdigest(),
                                                                         "format": 3}
    assert (sysroot / ".axrb-sysroot.json").read_text().startswith('{"digest":')  # written like AXRB's launcher
    assert not (inst.runtime / "direct/downloads/frameport").exists()
    assert not (inst.runtime / "direct/sysroot.new").exists()
    assert json.loads((inst.runtime / "license-acceptance.json").read_text())["license"] == "android-sdk-license"
    assert axrb.direct_setup_state(inst)["ready"]
    (sysroot / "system/lib64/libandroid.so").write_bytes(b"")  # a receipt alone doesn't make it ready
    assert not axrb.sysroot_ready(inst)


def test_direct_setup_refuses_an_incomplete_install(tmp_path, win):
    inst = direct_install(tmp_path)
    (inst.resources / axrb.DIRECT_EXE).unlink()
    assert axrb.direct_setup_state(inst)["missing_files"] == [axrb.DIRECT_EXE]
    with pytest.raises(axrb.AxrbError, match="incomplete"):
        axrb.setup_direct(inst)


def test_direct_launcher_script_and_arguments(tmp_path, win):
    inst = direct_install(tmp_path)
    text = axrb.direct_launcher_text(inst)
    assert "run_direct_game.ps1" in text and "out\\direct\\axrb-direct.exe" in text
    assert not re.search(r"@[A-Z]+@", text)  # every placeholder filled
    assert "$env:AXRB_DATA_HOME = 'W:" in text and "AXRB-Emulatorless Runtime\\output'" in text
    assert "-DataDirectory 'W:" in text and "@extra" in text
    args = axrb.launcher_args(r"C:\x\fp-axrb-direct.ps1", "com.a.b", "com.a.b/com.a.b.Main", "Game",
                              ["-Translator", "digitalis", "-PrecomposeProjectionLayers"])
    assert args.endswith('-GameName "Game" -Translator digitalis -PrecomposeProjectionLayers')


def test_backend_choice_and_flags():
    from frameport.targets import pc_android

    plain = Recipe("com.g", {"patch_oculus_unity": {}})
    assert pc_android.choose_backend(plain, "Vulkan (libvulkan)") == "direct"
    assert pc_android.choose_backend(plain, "GLES or unknown") == "emulator"  # direct mode has no GLES renderer
    assert pc_android.choose_backend(plain, "") == "emulator"
    # Unity boot.config's xr-vulkan-* keys are in GLES builds too (Toy Master: EGL crash in direct mode)
    assert pc_android.choose_backend(plain, "Vulkan (Unity boot.config)") == "emulator"
    assert pc_android.choose_backend(plain, "Vulkan (declared in manifest)") == "direct"
    chosen = Recipe("com.g", {"pc.axrb_backend": {"backend": "emulator", "translator": "digitalis"}})
    assert pc_android.choose_backend(chosen, "Vulkan (x)") == "emulator"
    assert pc_android.choose_backend(chosen, "Vulkan (x)", override="direct") == "direct"
    with pytest.raises(ValueError):
        pc_android.choose_backend(plain, "", override="qemu")
    flags = pc_android.launch_flags
    assert flags("direct", {}) == []
    assert flags("direct", {"translator": "dynarmic"}) == []  # AXRB's default
    assert flags("direct", {"translator": "digitalis", "precompose": True}) == ["-Translator", "digitalis",
                                                                                "-PrecomposeProjectionLayers"]
    assert flags("emulator", {"translator": "digitalis", "legacy_oculus_driver": True}) == []
    assert flags("emulator", {}, {"precomposeProjectionLayers": True}) == ["-PrecomposeProjectionLayers"]
    # AXRB's profile never turns the legacy driver / Quest identity on; controllers_only needs the legacy driver
    assert flags("direct", {}, {"legacyOculusDriver": True, "questIdentity": True}) == []
    assert flags("direct", {"controllers_only": True}) == []
    assert flags("direct", {"legacy_oculus_driver": True, "quest_identity": True, "controllers_only": True}) == [
        "-LegacyOculusDriver", "-QuestIdentity", "-ControllersOnly"]
    with pytest.raises(ValueError):
        flags("direct", {"translator": "qemu"})


def test_backend_patch_goes_into_the_pc_selection():
    from frameport.patches.base import get, pc_selection

    p = get("pc.axrb_backend")
    assert p.on_pc and p.stage == "install" and p.CHOICES[0] == ("", "Automatic")
    assert pc_selection({"pc.axrb_backend": {"backend": "direct"}, "frame.gl_shim": {}}) == {
        "pc.axrb_backend": {"backend": "direct"}}


def manifest(package="com.game.x", activity=".Main", alias=False):
    from conftest import build_axml

    tag = "activity-alias" if alias else "activity"
    attrs = [("name", "str", ".Launcher" if alias else activity)]
    if alias:
        attrs.append(("targetActivity", "str", activity))
    return build_axml([
        ("start", "manifest", [("package", "str", package)]), ("start", "application", []),
        ("start", "activity", [("name", "str", "com.other.Settings")]), ("start", "intent-filter", []),
        ("start", "action", [("name", "str", "android.intent.action.MAIN")]), ("end", "action"),
        ("end", "intent-filter"), ("end", "activity"),
        ("start", tag, attrs), ("start", "intent-filter", []),
        ("start", "action", [("name", "str", "android.intent.action.MAIN")]), ("end", "action"),
        ("start", "category", [("name", "str", "android.intent.category.LAUNCHER")]), ("end", "category"),
        ("end", "intent-filter"), ("end", tag), ("end", "application"), ("end", "manifest")])


def test_launcher_activity_from_the_manifest():
    from frameport.apk import axml

    assert axml.launcher_activity(manifest()) == "com.game.x/com.game.x.Main"
    assert axml.launcher_activity(manifest(activity="com.unity3d.player.UnityPlayerActivity")) == \
        "com.game.x/com.unity3d.player.UnityPlayerActivity"
    assert axml.launcher_activity(manifest(activity="Main")) == "com.game.x/com.game.x.Main"
    assert axml.launcher_activity(manifest(alias=True)) == "com.game.x/com.game.x.Main"


def test_install_for_direct_mode(tmp_path, monkeypatch, win):
    from frameport.targets import pc_android

    apk = tmp_path / "out" / "com.game.x.apk"
    apk.parent.mkdir()
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", manifest(activity="com.epicgames.unreal.GameActivity"))
    data = tmp_path / "obb"
    data.mkdir()
    (data / "main.1.com.game.x.obb").write_bytes(b"1" * 10)
    inst = direct_install(tmp_path)
    monkeypatch.setattr(pc_android, "ensure_axrb", lambda rep, backend: inst if backend == "direct" else pytest.fail())
    monkeypatch.setattr(pc_android, "_analysis", lambda pkg: {"graphics": "Vulkan (libvulkan.so)"})
    monkeypatch.setattr(axrb, "local_appdata", lambda: tmp_path / "Local")
    monkeypatch.setattr(axrb, "adb", lambda *a, **k: pytest.fail("direct mode has no adb"))
    monkeypatch.setattr(pc_android, "powershell_win", lambda: r"C:\Windows\powershell.exe")
    recipe = Recipe("com.game.x", {"patch_oculus_unreal": {}, "frame.adapter": {},
                                   "pc.axrb_backend": {"translator": "digitalis"}})
    res = pc_android.install("com.game.x", "Game X", apk, data, recipe, Reporter(), record_dir=tmp_path / "rec",
                             appid=lambda exe, title: 0x80001234)
    assert res["backend"] == "direct" and res["activity"] == "com.game.x/com.epicgames.unreal.GameActivity"
    game = tmp_path / "Local/FramePort/axrb/games/com.game.x"
    assert (game / "com.game.x.apk").read_bytes() == apk.read_bytes()
    assert (game / "assets.txt").read_text() == \
        f"Android/obb/com.game.x/main.1.com.game.x.obb\tW:{str(data).replace('/', chr(92))}\\main.1.com.game.x.obb\n"
    assert (tmp_path / "Local/FramePort/axrb/fp-axrb-direct.ps1").read_text(encoding="utf-8-sig") == \
        axrb.direct_launcher_text(inst)
    dep = json.loads((tmp_path / "rec" / "deployment.json").read_text())
    assert dep["backend"] == "direct" and dep["flags"] == ["-Translator", "digitalis"]
    assert dep["recipe"]["patches"] == ["adapter.focus_hold", "frame.adapter", "patch_oculus_unreal",
                                        "pc.axrb_backend"]
    exe, _, opts = pc_android.shortcut_fields(dep)
    assert "fp-axrb-direct.ps1" in opts and "-Package com.game.x " in opts and opts.endswith("-Translator digitalis")
    # an uninstall that drops the saves removes direct mode's Android data too
    saves = inst.runtime / "direct/data/data/com.game.x"
    saves.mkdir(parents=True)
    monkeypatch.setattr(axrb, "direct_runtime_root", lambda: inst.runtime)
    assert pc_android.uninstall(dep, keep_data=True) and saves.exists() and not game.exists()
    assert pc_android.uninstall(dep, keep_data=False) and not saves.exists()


def test_long_content_lists_go_through_hard_links(tmp_path, monkeypatch, win):
    """axrb-direct.exe gets one --mount-ro argument per content file (paths resolved by AXRB, so junctions don't
    help): Batman's 90 files from a long dump folder overflowed Windows' 32767-character command line. Such lists go
    through hard links in <game dir>\\content; a short list keeps the files' own paths."""
    from frameport.targets import pc_android

    monkeypatch.setattr(axrb, "local_appdata", lambda: tmp_path / "Local")
    data = tmp_path / ("a-very-long-dump-folder-name" * 6) / "obb"
    data.mkdir(parents=True)
    names = [f"scenes_{i:03d}_assets_all_with_a_long_name.bundle" for i in range(90)]
    for n in names:
        (data / n).write_bytes(b"x")
    links = tmp_path / "Local/FramePort/axrb/games/com.game.x/content"
    lines = pc_android.asset_lines("com.game.x", data, names[:3], Reporter())  # fits: the files' own paths
    assert lines[0] == f"Android/obb/com.game.x/{names[0]}\tW:{str(data / names[0]).replace('/', chr(92))}"
    assert not links.exists()
    lines = pc_android.asset_lines("com.game.x", data, None, Reporter())
    assert len(lines) == 90 and pc_android.mount_args_length(lines) <= pc_android.MOUNT_ARGS_BUDGET
    assert lines[0] == f"Android/obb/com.game.x/{names[0]}\tW:{str(links).replace('/', chr(92))}\\{names[0]}"
    assert os.path.samefile(links / names[5], data / names[5])
    assert pc_android.asset_lines("com.game.x", data, None, Reporter()) == lines  # again: links kept
    pc_android.asset_lines("com.game.x", data, names[:3], Reporter())  # short again: the links go, the data stays
    assert not links.exists() and (data / names[0]).read_bytes() == b"x"
    # no hard links possible (another drive): a clear error instead of Windows' "filename too long"
    monkeypatch.setattr(os, "link", lambda a, b: (_ for _ in ()).throw(OSError(18, "cross-device link")))
    with pytest.raises(RuntimeError, match="don't fit on the command line of AXRB's direct mode"):
        pc_android.asset_lines("com.game.x", data, None, Reporter())
    assert not links.exists()
    # a folder Windows reaches only through \\wsl$: copies next to the APK
    monkeypatch.setattr(winhost, "to_windows", lambda p: ("\\\\wsl.localhost\\U" if "games" not in str(p) else "W:")
                        + str(p).replace("/", "\\"))
    lines = pc_android.asset_lines("com.game.x", data, names[:1], Reporter())
    copy = tmp_path / "Local/FramePort/axrb/games/com.game.x/obb" / names[0]
    assert copy.read_bytes() == b"x" and lines[0].endswith(names[0]) and "\\\\wsl" not in lines[0]


def test_frame_rates_from_axrb_perf_lines():
    """Launch tests report the rate from AXRB's 5-second summaries (Batman under Monado, 2026-10-15)."""
    from frameport.targets import pc_android

    text = "\n".join(
        [f"I/AXRB.Perf( 4290): AXRB.Perf end-frame: rate={r}/s avg=0.113ms max=1.561ms samples=315" for r in
         (3.0, 58.9, 62.4, 62.9, 64.1, 64.1, 63.7)]
        + ["AXRB.Perf host-fresh-submit: rate=62.9/s avg=15.889ms max=31.773ms samples=315",
           "AXRB.Perf host-end-frame: rate=63.7/s avg=0.197ms"])
    assert pc_android.frame_rates(text) == {"game": 63.7, "host": 62.9}
    assert pc_android.frame_rates("no perf lines") == {}


def test_direct_logs_and_state(tmp_path, monkeypatch):
    from frameport.targets import pc_android

    root = tmp_path / "rt"
    (root / "output/logs/game").mkdir(parents=True)
    (root / "output/logs/game/direct.log").write_text("  1.0  12 I/vulkan: vkCreateDevice(1 queue families) = 0\n")
    (root / "output/logs/game/host.err").write_text("AXRB TCP: listening on 0.0.0.0:38490\n")
    monkeypatch.setattr(axrb, "direct_runtime_root", lambda: root)
    monkeypatch.setattr(axrb, "runtime_root", lambda: None)
    assert set(pc_android.game_logs()) == {"direct/direct.log", "direct/host.err"}
    assert set(pc_android.game_logs(axrb.DIRECT)) == {"direct.log", "host.err"}
    assert pc_android.direct_state("", True) is None
    assert pc_android.direct_state("", False) == "NEVER_STARTED"
    out = "AXRB_AOSP_BOOTSTRAP\nAXRB_AOSP_ACTIVITY_CREATED com.game.x.Main\n"
    assert pc_android.direct_state(out, True) == "RUNNING" and pc_android.direct_state(out, False) == "EXITED"
    assert pc_android.dep_backend({}) == "emulator"  # installs from before direct mode


def test_direct_mode_triage():
    from frameport.validate.triage import triage

    log = ("===== host.err\nAXRB TCP: listening on 0.0.0.0:38490\n"
           "===== direct.out\nAXRB_AOSP_BOOTSTRAP\nAXRB_AOSP_ACTIVITY_CREATED com.game.x.Main\n"
           "===== direct.log\n"
           "   3.250  4410 W/syscall: unimplemented syscall 283 (membarrier) from libunity.so + 0x12\n"
           "   9.100  4410 I/vulkan: vkCreateDevice(1 queue families, 9 extensions) = 0\n"
           "  12.000  4410 E/cpu: thread 7 (UnityMain): fatal signal 4 (SIGILL), code 1, fault addr 7f001000\n")
    r = triage(log, "EXITED", "com.game.x", kind="axrb")
    assert {f.id for f in r.findings} == {"axrb-direct-unhandled-instruction", "axrb-direct-unsupported-syscall"}
    assert r.milestone == "Vulkan device created on the PC's GPU (direct mode)"
    assert "Game activity created (direct mode)" in r.milestones
    crash = "E/cpu: thread 3 (RenderThread): fatal signal 11 (SIGSEGV), code 1, fault addr 0\n"
    assert [f.id for f in triage(crash, "EXITED", "com.game.x", kind="axrb").findings] == ["axrb-direct-guest-crash"]
    gles = "E/Unity   ( 4410): [EGL] Unable to initialize EGL display: EGL_BAD_DISPLAY\n"
    assert [f.id for f in triage(gles, "EXITED", "com.game.x", kind="axrb").findings] == ["axrb-direct-no-gles"]
    # Toy Master (Unity 2022.3, GLES) in direct mode, 2026-10-15: Unity's own EGL message, then its SIGTRAP
    toy = ("E/Unity( 4252): Unable to retrieve the default EGL display!\n"
           "   17.854 82044 E/cpu: thread 4252 (UnityMain): fatal signal 5 (SIGTRAP), code -6, fault addr 278b00001092\n")
    assert {f.id for f in triage(toy, "EXITED", "com.game.x", kind="axrb").findings} == \
        {"axrb-direct-no-gles", "axrb-direct-guest-crash"}
    for text, want in (("E/vulkan: vulkan-1.dll is not installed: the guest has no GPU", "axrb-direct-no-vulkan"),
                       ("Direct mode is missing C:\\x\\sysroot/system/lib64.", "axrb-direct-incomplete"),
                       ("  1.0 1 E/host-fault: axrb-direct.exe + 0x1234", "axrb-direct-host-fault"),
                       ("E/AndroidRuntime( 99): FATAL EXCEPTION: main", "axrb-direct-java-crash")):
        assert want in {f.id for f in triage(text, "EXITED", "com.game.x", kind="axrb").findings}
    assert not [f for f in triage(log, "EXITED", "com.game.x").findings if f.id.startswith("axrb")]
