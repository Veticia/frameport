"""PC VR target (Revive + local Windows Steam) against a fake Steam folder; no Windows needed."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace

from frameport.core import winhost
from frameport.core.events import Reporter
from frameport.core.models import Recipe

AGENT = Path(__file__).resolve().parents[1] / "agent" / "frameport_agent.py"


def vdf_mod():
    spec = importlib.util.spec_from_file_location("agent_for_test", AGENT)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def fake_steam(tmp_path):
    root = tmp_path / "Steam"
    (root / "userdata/12345/config").mkdir(parents=True)
    (root / "userdata/999/config").mkdir(parents=True)
    (root / "config").mkdir()
    (root / "steam.exe").write_bytes(b"MZ")
    (root / "config/loginusers.vdf").write_text(
        '"users"\n{\n\t"76561197960278073"\n\t{\n\t\t"AccountName"\t\t"a"\n\t\t"MostRecent"\t\t"1"\n\t}\n'
        '\t"76561197960266727"\n\t{\n\t\t"MostRecent"\t\t"0"\n\t}\n}\n')
    return root


def test_steam_user_most_recent(tmp_path):
    assert winhost.steam_user(fake_steam(tmp_path)) == "12345"


def test_path_translation():
    if not winhost.is_wsl():
        return
    assert winhost.to_windows("/mnt/d/Games/X") == "D:\\Games\\X"
    assert winhost.to_local("C:\\Program Files\\Revive") == Path("/mnt/c/Program Files/Revive")


def test_install_and_shortcut(tmp_path, monkeypatch):
    from frameport.targets import pc_revive
    from frameport.tools import revive

    root = fake_steam(tmp_path)
    rv = tmp_path / "revive"
    rv.mkdir()
    for n in revive.RUNTIME_FILES:
        (rv / n).write_bytes(b"x")
    monkeypatch.setenv("FRAMEPORT_REVIVE_DIR", str(rv))
    events = []
    monkeypatch.setattr(winhost, "available", lambda: True)
    monkeypatch.setattr(winhost, "is_wsl", lambda: False)  # never copy into the real %LOCALAPPDATA%
    monkeypatch.setattr(winhost, "env_path", lambda name: None)
    monkeypatch.setattr(winhost, "steam_root", lambda: root)
    monkeypatch.setattr(winhost, "stop_steam", lambda r: events.append("stop") or True)
    monkeypatch.setattr(winhost, "start_steam", lambda r: events.append("start"))
    monkeypatch.setattr(winhost, "to_windows", lambda p: "W:" + str(p).replace("/", "\\"))
    monkeypatch.setattr("frameport.artwork.fetch.cache.http_get", lambda *a, **k: (_ for _ in ()).throw(OSError()))
    game = tmp_path / "Games" / "Space Game"
    game.mkdir(parents=True)
    (game / "Space Game.exe").write_bytes(b"MZ")
    t = pc_revive.PcReviveTarget()
    r = Recipe("rift.space_game", {"pcvr.revive": {}})
    res = t.install_pcvr("rift.space_game", "Space Game", game, "Space Game.exe", r, Reporter())
    assert res["ok"] and [d["package"] for d in t.installed()] == ["rift.space_game"]
    t.add_to_library(["rift.space_game"], Reporter())
    assert events == ["stop", "start"]
    sc = vdf_mod().vdf_decode((root / "userdata/12345/config/shortcuts.vdf").read_bytes())["shortcuts"]["0"]
    exe_win = "W:" + str(game / "Space Game.exe").replace("/", "\\")
    assert sc["Exe"] == '"' + "W:" + str(rv).replace("/", "\\") + '\\ReviveInjector.exe"'
    assert sc["LaunchOptions"] == f'/openxr "{exe_win}"'
    assert sc["tags"] == {"0": "FramePort PC VR", "1": "PC VR"} and sc["appid"] == res["appid"]
    assert sc["StartDir"].endswith('Space Game\\"')
    # OpenVR backend drops /openxr
    r2 = Recipe("rift.space_game", {"pcvr.revive": {}, "pcvr.revive_openvr": {}})
    t.install_pcvr("rift.space_game", "Space Game", game, "Space Game.exe", r2, Reporter())
    assert pc_revive.shortcut_fields(t._dep("rift.space_game"))[2] == f'"{exe_win}"'
    assert t.uninstall("rift.space_game")["removed"] and t.installed() == []


def test_openxr_native_runs_exe_directly():
    from frameport.targets.pc_revive import shortcut_fields

    exe, start, opts = shortcut_fields({"exe_win": "D:\\G\\Game.exe", "revive_win": None})
    assert (exe, start, opts) == ('"D:\\G\\Game.exe"', '"D:\\G\\"', "")


def test_no_crash_reporter_patch(tmp_path):
    from types import SimpleNamespace

    from frameport.core.models import Recipe
    from frameport.patches import pcvr
    from frameport.patches.base import get

    p = get("pcvr.no_crash_reporter")
    ue = SimpleNamespace(engine="Unreal", extra={"kind": "rift"})
    unity = SimpleNamespace(engine="Unity", extra={"kind": "rift"})
    assert p.detect(ue).recommended and p.applies(ue)
    assert p.detect(unity) is None and not p.applies(unity)
    assert pcvr.game_args(Recipe(package="rift.x", patches=["pcvr.no_crash_reporter"])) == ["-nocrashreports"]
    assert pcvr.game_args(Recipe(package="rift.x", patches=[])) == []


def test_agent_crash_reporter_toggle(tmp_path):
    import importlib.util
    from pathlib import Path

    spec = importlib.util.spec_from_file_location("fpa", Path(__file__).parents[1] / "agent" / "frameport_agent.py")
    agent = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(agent)
    exe = tmp_path / "game" / "Engine" / "Binaries" / "Win64" / "CrashReportClient.exe"
    exe.parent.mkdir(parents=True)
    exe.write_bytes(b"MZ")
    agent.set_crash_reporter(str(tmp_path), enabled=False)
    assert not exe.exists() and exe.with_name("CrashReportClient.exe.disabled").exists()
    # still counts as uploaded (not re-sent on the next install)
    assert "Engine/Binaries/Win64/CrashReportClient.exe" in agent.tree_manifest(str(tmp_path / "game"))
    agent.set_crash_reporter(str(tmp_path), enabled=True)
    assert exe.exists()


def test_library_migration_adds_no_crash_reporter(tmp_path, monkeypatch):
    import json

    from frameport.core import library

    monkeypatch.setattr(library, "_path", lambda: tmp_path / "library.json")
    (tmp_path / "library.json").write_text(json.dumps({"games": {
        "rift.ue": {"analysis": {"engine": "Unreal", "extra": {"kind": "rift"}}, "recipe": {"patches": {}}},
        "rift.unity": {"analysis": {"engine": "Unity", "extra": {"kind": "rift"}}, "recipe": {"patches": {}}},
    }, "settings": {}}))
    games = library.load()["games"]
    # the run-correct migration re-derives from engine defaults: Oculus games get Revive + as-is
    assert set(games["rift.ue"]["recipe"]["patches"]) == {"pcvr.revive", "pcvr.revive_openvr", "pcvr.libovr_redirect",
        "pcvr.steamvr_tuning", "pcvr.no_crash_reporter", "pcvr.oculus_unreal", "pcvr.xr_timefix"}
    assert set(games["rift.unity"]["recipe"]["patches"]) == {"pcvr.revive", "pcvr.revive_openvr", "pcvr.steamvr_tuning",
        "pcvr.libovr_redirect", "pcvr.xr_timefix"}
    assert games["rift.ue"]["recipe"]["as_is"] and games["rift.unity"]["recipe"]["as_is"]
    # runs once: a user who turns it off keeps it off
    data = library.load()
    data["games"]["rift.ue"]["recipe"]["patches"].pop("pcvr.no_crash_reporter")
    library.save(data)
    assert "pcvr.no_crash_reporter" not in library.load()["games"]["rift.ue"]["recipe"]["patches"]


def test_oculus_unreal_patch():
    from types import SimpleNamespace

    from frameport.patches.base import get

    p = get("pcvr.oculus_unreal")
    ue = SimpleNamespace(engine="Unreal", extra={"kind": "rift"})
    ue_xr = SimpleNamespace(engine="Unreal", extra={"kind": "rift", "frame_native": True})
    unity = SimpleNamespace(engine="Unity", extra={"kind": "rift"})
    quest_ue = SimpleNamespace(engine="Unreal", extra={})
    assert p.category == "pcvr" and p.detect(ue).recommended and p.applies(ue)
    assert not p.detect(ue_xr).recommended and p.applies(ue_xr)
    assert p.detect(unity) is None and not p.applies(unity)
    assert p.detect(quest_ue) is None and not p.applies(quest_ue)


def test_migration_rederives_rift_recipes(tmp_path, monkeypatch):
    import json

    """The run-correct migration re-derives every Rift recipe from engine defaults: Oculus games get Revive for VR."""
    from frameport.core import library

    monkeypatch.setattr(library, "_path", lambda: tmp_path / "library.json")
    (tmp_path / "library.json").write_text(json.dumps({"games": {
        "rift.ue": {"analysis": {"engine": "Unreal", "abis": ["x86_64"],
                                 "extra": {"kind": "rift", "needs_revive": True}},
                    "recipe": {"as_is": True, "patches": {}}},  # Revive wrongly dropped -> restored
        "com.q.ue": {"analysis": {"engine": "Unreal", "extra": {}}, "recipe": {"patches": {}}},
    }, "settings": {}}))
    games = library.load()["games"]
    r = games["rift.ue"]["recipe"]
    assert r["as_is"] and "pcvr.revive" in r["patches"] and "pcvr.oculus_unreal" in r["patches"]
    # Quest games untouched by the Rift migrations (they only gain the default-on binary fixes and, from the
    # headset-model migration, the Quest 3 identity every Quest game gets)
    assert set(games["com.q.ue"]["recipe"]["patches"]) <= {"frame.swapchain_limit", "frame.vk_sanitize",
                                                           "device.headset_model"}
    assert "device.headset_model" not in r["patches"]  # never for PC VR games

def test_oculus_hmd_helper_artifact():
    """The prebuilt helper is a freestanding Windows x64 console exe that only imports kernel32."""
    import struct

    from frameport.core.paths import artifacts_dir

    data = (artifacts_dir() / "win-x64" / "fp_oculushmd.exe").read_bytes()
    assert data[:2] == b"MZ"
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    assert struct.unpack_from("<H", data, pe + 4)[0] == 0x8664  # AMD64
    assert struct.unpack_from("<H", data, pe + 24)[0] == 0x20B  # PE32+
    assert struct.unpack_from("<H", data, pe + 24 + 68)[0] == 3  # console subsystem (stderr -> launch.log)
    assert b"kernel32.dll" in data and b"CreateEventW" in data
    assert "OculusHMDConnected".encode("utf-16-le") in data
    assert not any(dll in data.lower() for dll in (b"msvcrt", b"vcruntime", b"ucrtbase"))


def test_oculus_hmd_triage():
    from frameport.validate.triage import triage

    ok = triage("FramePort: launching rift.x with proton_11-arm64\n"
                "FramePort oculushmd: OculusHMDConnected event ready\n", package="rift.x")
    assert "Oculus headset event provided" in ok.milestones and not ok.findings
    bad = triage("FramePort oculushmd: could not create the OculusHMDConnected event (error 5)\n", package="rift.x")
    assert [f.id for f in bad.findings] == ["oculus-hmd-event"]
    assert "pcvr.oculus_unreal" in bad.findings[0].suggest


def test_migration_respects_catalog_openxr_native(tmp_path, monkeypatch):
    import json

    """A catalog SteamVR/OpenXR-native recipe (Revive removed) keeps Revive off after the re-derive."""
    from frameport.core import library

    monkeypatch.setattr(library, "_path", lambda: tmp_path / "library.json")
    (tmp_path / "library.json").write_text(json.dumps({"games": {
        # openxr_native -> engine.suggest doesn't add Revive
        "rift.xr": {"analysis": {"engine": "Unity", "abis": ["x86_64"],
                                 "extra": {"kind": "rift", "openxr_native": True}},
                    "recipe": {"as_is": True, "patches": {"pcvr.revive": {}}}},
    }, "settings": {}}))
    r = library.load()["games"]["rift.xr"]["recipe"]
    assert "pcvr.revive" not in r["patches"]


def test_pc_defaults_to_openvr_backend():
    from frameport.patches import base
    from frameport.targets.pc_revive import shortcut_fields

    p = base.get("pcvr.revive_openvr")
    assert p.detect(SimpleNamespace(extra={"kind": "rift"})).recommended  # on by default (PC)
    assert not p.detect(SimpleNamespace(extra={"kind": "rift", "frame_native": True})).recommended
    # OpenVR backend -> no /openxr flag in the launch command
    dep_ovr = {"exe_win": "C:\\g\\Game.exe", "revive_win": "C:\\Revive", "backend": "openvr", "game_args": []}
    dep_xr = {**dep_ovr, "backend": "openxr"}
    assert "/openxr" not in shortcut_fields(dep_ovr)[2] and "Game.exe" in shortcut_fields(dep_ovr)[2]
    assert "/openxr" in shortcut_fields(dep_xr)[2]


def test_start_steamvr(monkeypatch, tmp_path):
    from frameport.core import winhost

    running = {"vr": False}
    monkeypatch.setattr(winhost, "process_running", lambda img: running["vr"] if "vr" in img.lower() else False)
    monkeypatch.setattr(winhost, "app_installed", lambda root, appid: True)
    started = []
    monkeypatch.setattr(winhost, "start_detached", lambda exe, args=(), cwd=None: (started.append(list(args)),
                        running.__setitem__("vr", True))[0])
    assert winhost.start_steamvr(tmp_path, wait=5) is True and started == [["-applaunch", "250820"]]
    # already running -> no-op
    started.clear()
    assert winhost.start_steamvr(tmp_path) is True and started == []
    # not installed -> False, nothing started
    running["vr"] = False
    monkeypatch.setattr(winhost, "app_installed", lambda root, appid: False)
    assert winhost.start_steamvr(tmp_path, wait=2) is False and started == []


def test_libovr_redirect_patch():
    from frameport.patches.base import get

    p = get("pcvr.libovr_redirect")
    assert p.requires == ()  # also for repacks (their bundled Revive), not only FramePort's
    assert p.detect(SimpleNamespace(extra={"kind": "rift"})).recommended
    assert p.detect(SimpleNamespace(extra={"kind": "rift", "launch": "repack"})).recommended
    assert not p.detect(SimpleNamespace(extra={"kind": "rift", "frame_native": True})).recommended
    assert p.detect(SimpleNamespace(extra={})) is None
