"""Embedded loader redirection, independent of any game or device."""
import json
import shutil
import struct
import subprocess
import zipfile

import pytest
from test_agent import load_agent
from test_patches import _analysis

from frameport.analysis import detect
from frameport.analysis import static_openxr as sx
from frameport.apk.workspace import ApkWorkspace
from frameport.core.events import Reporter
from frameport.patches import base
from frameport.patches.frame import static_openxr_loader as patch


@pytest.fixture
def embedded(tmp_path):
    # A real ELF layout with synthetic strings; change only e_machine to model an arm64 game.
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("needs host C compiler")
    strings = [f"/{p}/etc/openxr/1/active_runtime{abi}.json"
               for p in ("product", "odm", "oem", "vendor", "system") for abi in ("", ".aarch64", ".arm64-v8a")]
    strings += list(sx.BROKERS) + ["RuntimeManifestFile", "xrNegotiateLoaderRuntimeInterface", "unrelated"]
    src = tmp_path / "embedded.c"
    src.write_text("const char *strings[] = {" + ",".join(json.dumps(s) for s in strings) + "};")
    out = tmp_path / "embedded.so"
    subprocess.run([compiler, "-shared", "-fPIC", str(src), "-o", str(out)], check=True)
    data = bytearray(out.read_bytes())
    struct.pack_into("<H", data, 18, 183)  # EM_AARCH64
    return bytes(data)


def test_detection_and_byte_stable_rewrite(embedded):
    assert sx.detected("libengine.so", embedded)
    assert not sx.detected("libopenxr_loader.so", embedded)
    assert not sx.detected("libengine.so", embedded.replace(b"RuntimeManifestFile", b"X" * 19))
    wrong_arch = bytearray(embedded)
    struct.pack_into("<H", wrong_arch, 18, 62)
    assert not sx.detected("libengine.so", bytes(wrong_arch))
    fixed, count = sx.rewrite(embedded)
    assert count == 17
    assert len(fixed) == len(embedded)
    expected = embedded
    for old in sx.paths(embedded) + list(sx.BROKERS):
        new = sx.MANIFEST if old.startswith("/") else old.replace("org.khronos", "org.fp_null")
        expected = expected.replace(old.encode() + b"\0", new.encode().ljust(len(old) + 1, b"\0"))
    assert fixed == expected  # all other bytes, offsets and ELF metadata are identical
    assert sx.rewrite(fixed) == (fixed, 0)
    assert not sx.paths(fixed)
    assert detect.ANALYSIS_VERSION >= 23


def test_patch_and_adapter_without_overport(tmp_path, monkeypatch, embedded):
    inner = tmp_path / "inner.so"
    inner.write_bytes(b"unmodified Khronos loader")
    monkeypatch.setattr(patch.overport, "ensure_runtime_lib", lambda *args: inner)
    apk = tmp_path / "in.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("lib/arm64-v8a/libengine.so", embedded)
    a = _analysis(extra={"static_openxr_loader": ["libengine.so"]})
    selection = {patch.StaticOpenXRLoader.id: {}, "frame.adapter": {}}
    p = patch.StaticOpenXRLoader()
    assert p.detect(a).recommended
    assert not p.applies(_analysis())
    with ApkWorkspace(apk) as ws:
        ctx = base.ApkContext(ws, a, {}, Reporter(), selection)
        assert p.apply(ctx)
        assert base.get("frame.adapter").apply(ctx)
        assert ws.read_lib("libopenxr_loader_original.so") == inner.read_bytes()
        assert ws.has(ws.lib("libframe_settings.so"))
        assert all(ok for _, ok, _ in p.validate(ctx))
        assert all(ok for _, ok, _ in base.get("frame.adapter").validate(ctx))
        assert not p.apply(ctx)
        assert not base.get("frame.adapter").apply(ctx)
        out = ws.write(tmp_path / "out.apk")
    with ApkWorkspace(out) as ws:
        ctx.ws = ws
        assert not p.apply(ctx)
        ws.put(ws.lib("libengine.so"), embedded)
        assert not p.validate(ctx)[0][1]


def test_manifest_repair_and_launcher_upgrade(tmp_path, monkeypatch):
    a = load_agent(monkeypatch, tmp_path)
    root = tmp_path / "game"
    a.write_openxr_runtime_manifest(str(root), False)
    assert not root.exists()
    a.write_openxr_runtime_manifest(str(root), True)
    target = root / "lepton-data/external/fp_openxr_runtime.json"
    manifest = json.loads(target.read_text())
    assert manifest["runtime"]["library_path"] == patch.SHIM
    victim = tmp_path / "victim"
    victim.write_text("untouched")
    target.unlink()
    target.symlink_to(victim)
    a.write_openxr_runtime_manifest(str(root), True)
    assert victim.read_text() == "untouched"
    assert not target.is_symlink()
    anchor = tmp_path / "anchors/com.example.game"
    anchor.mkdir(parents=True)
    a.write_launcher(str(anchor), str(root), "com.example.game", "Test", 123, "/lepton", {}, static_openxr=True)
    text = (anchor / "launch.sh").read_text()
    assert " _openxr_manifest com.example.game\n" in text
    assert a.set_openxr_manifest_line(text, "com.example.game", True) == text
    assert "_openxr_manifest" not in a.set_openxr_manifest_line(text, "com.example.game", False)
    subprocess.run(["bash", "-n", str(anchor / "launch.sh")], check=True)


def test_runtime_shim_host(tmp_path):
    import os
    from pathlib import Path

    root = Path(__file__).resolve().parents[1]
    headers = next((root / "native/.cache").glob("openxr-*/openxr/openxr.h"), None)
    cc = shutil.which("cc")
    if not headers or not cc:
        pytest.skip("needs cached OpenXR headers and host C compiler")
    src = root / "native/xrruntime"
    inc = str(headers.parent.parent)
    subprocess.run([cc, "-shared", "-fPIC", "-I", inc, str(src / "test_bridge.c"),
                    "-o", str(tmp_path / "libopenxr_loader_generic.so")], check=True)
    exe = tmp_path / "shim_test"
    subprocess.run([cc, "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-I", inc,
                    str(src / "xrruntime.c"), str(src / "test.c"), "-ldl", "-lpthread", "-o", str(exe)], check=True)
    env = dict(os.environ, LD_LIBRARY_PATH=str(tmp_path))
    subprocess.run([str(exe)], env=env, check=True)
    (tmp_path / "libopenxr_loader_generic.so").unlink()
    subprocess.run([str(exe), "missing"], env=env, check=True)


def test_composed_khronos_paths(tmp_path, embedded):
    # Rebuild the fixture with Khronos platform_utils.hpp's actual fragment table.
    # This is also the layout in OVRPort's unmodified Khronos loader.

    from frameport.analysis import elf

    cc = shutil.which("cc")
    src = tmp_path / "composed.c"
    strings = list(sx.PARTITIONS) + [sx.SUBDIR, "/active_runtime.json", "/active_runtime.aarch64.json"]
    strings += list(sx.BROKERS) + ["RuntimeManifestFile", "xrNegotiateLoaderRuntimeInterface"]
    src.write_text("const char *strings[] = {" + ",".join(json.dumps(s) for s in strings) + "};")
    out = tmp_path / "composed.so"
    subprocess.run([cc, "-shared", "-fPIC", str(src), "-o", str(out)], check=True)
    buf = bytearray(out.read_bytes())
    struct.pack_into("<H", buf, 18, 183)
    data = bytes(buf)
    assert sx.composed(data) and sx.detected("libgame.so", data)
    fixed, count = sx.rewrite(data)
    assert count == 8 and len(fixed) == len(data)
    assert sx.redirected(fixed) and sx.rewrite(fixed) == (fixed, 0)
    expected = data
    for old, new in [(p, "/sd") for p in sx.PARTITIONS] + [(sx.SUBDIR, "card/fp/")] + [
            (p, p.replace("org.khronos", "org.fp_null")) for p in sx.BROKERS]:
        expected, _ = elf.replace_rodata_string(expected, old, new)
    assert fixed == expected
    assert "/sd" + "card/fp/" + "1" + "/active_runtime.json" == sx.COMPOSED_MANIFEST


def test_game_loader_skips_implicit_layers(tmp_path):
    # GitHub #254: the game's loader and FrameBridge's inner loader both loaded Valve's implicit layer; its two
    # chains called each other until the stack ran out. Only the inner loader may see implicit layers.
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("needs host C compiler")
    src = tmp_path / "layers.c"
    strings = list(sx.PARTITIONS) + [sx.SUBDIR, "/active_runtime.json", "/active_runtime.aarch64.json"]
    strings += ["/vendor/etc", "/system/etc", sx.IMPLICIT_LAYERS, "/api_layers/explicit.d",
                "openxr/1/api_layers/implicit.d/"]
    strings += list(sx.BROKERS) + ["RuntimeManifestFile", "xrNegotiateLoaderRuntimeInterface"]
    src.write_text("const char *strings[] = {" + ",".join(json.dumps(s) for s in strings) + "};")
    out = tmp_path / "layers.so"
    subprocess.run([cc, "-shared", "-fPIC", str(src), "-o", str(out)], check=True)
    buf = bytearray(out.read_bytes())
    struct.pack_into("<H", buf, 18, 183)
    data = bytes(buf)
    fixed, count = sx.rewrite(data)
    assert count == 9 and len(fixed) == len(data)
    assert not sx.implicit_layers(fixed) and sx.redirected(fixed) and sx.rewrite(fixed) == (fixed, 0)
    assert b"/api_layers/explicit.d\0" in fixed and b"openxr/1/api_layers/implicit.d/\0" in fixed
    # a build from before the fix (runtime already redirected, layers not) is no longer "redirected" and gets them
    old = fixed.replace(sx.NO_IMPLICIT_LAYERS.encode() + b"\0\0", sx.IMPLICIT_LAYERS.encode() + b"\0")
    assert not sx.redirected(old) and sx.rewrite(old) == (fixed, 1)
    assert patch.StaticOpenXRLoader.revision >= 2


def test_analysis_records_embedded_loader(tmp_path, embedded, quest_manifest):
    apk = tmp_path / "analyze.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("lib/arm64-v8a/libengine.so", embedded)
    assert detect.analyze(apk).extra["static_openxr_loader"] == ["libengine.so"]


def test_triage_only_when_chain_absent():
    from frameport.validate.triage import triage

    line = "RuntimeManifestFile::FindManifestFiles - using global runtime file /vendor/etc/openxr/1/active_runtime.json"
    assert "static-openxr-loader-bypass" in {f.id for f in triage(line, "RUNNING").findings}
    for chain in ("FrameBridge: settings read", "fp_xrruntime: negotiated"):
        assert "static-openxr-loader-bypass" not in {f.id for f in triage(line + "\n" + chain, "RUNNING").findings}


def test_heuristic_selects_chain_without_game_specific_rules():
    from frameport.recommend.engine import suggest

    a = _analysis(package="com.example.othergame", engine="Other", libs=["libengine.so"],
                  extra={"static_openxr_loader": ["libengine.so"], "vr_kind": "openxr"})
    r = suggest(a, use_catalog=False)
    assert {"frame.static_openxr_loader", "frame.adapter"} <= r.patches.keys()
    assert not r.as_is
