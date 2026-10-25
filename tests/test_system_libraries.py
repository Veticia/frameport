"""frame.keep_system_libraries (GitHub #484, YouTube VR): OVRPort drops every <uses-library>; the ones Lepton's
Android provides (org.apache.http.legacy …) go back into the manifest, always optional."""
from __future__ import annotations

import re
import zipfile

from conftest import build_axml
from pyaxmlparser.axmlprinter import AXMLPrinter
from test_browser_handoff import _with_resource_map

from frameport.analysis import detect
from frameport.apk import axml
from frameport.apk.workspace import ApkWorkspace
from frameport.core.events import Reporter
from frameport.core.models import Analysis
from frameport.patches import base
from frameport.patches.frame.system_libraries import LEPTON_LIBRARIES
from frameport.recommend import engine
from frameport.validate.triage import triage

PKG = "com.google.android.apps.youtube.vr.oculus"


def _uses(kind: str, name: str) -> list[tuple]:
    return [("start", kind, [("name", "str", name), ("required", "bool", False)]), ("end", kind)]


def _manifest(original: bool) -> list[tuple]:
    """Shaped like YouTube VR 1.89.09's manifest (diagnostics of #484): optional libraries between the components,
    two of them provided by no Android but Google's/Meta's own. `original=False` = after OVRPort's
    patch_remove_uses_library (every entry gone but libopenxr.google.so)."""
    lib = (lambda kind, name: _uses(kind, name)) if original else (lambda kind, name: [])
    return [
        ("start", "manifest", [("package", "str", PKG)]),
        ("start", "uses-feature", [("name", "str", "android.hardware.vr.headtracking"), ("required", "bool", False)]),
        ("end", "uses-feature"),
        ("start", "application", [("label", "str", "YouTube"), ("debuggable", "bool", False)]),
        ("start", "activity", [("name", "str", "com.google.android.apps.youtube.vr.Main"),
                               ("exported", "bool", True)]),
        ("start", "intent-filter", []),
        ("start", "action", [("name", "str", axml.MAIN)]), ("end", "action"),
        ("start", "category", [("name", "str", axml.LAUNCHER)]), ("end", "category"),
        ("end", "intent-filter"),
        ("end", "activity"),
        *lib("uses-library", "org.apache.http.legacy"),
        ("start", "service", [("name", "str", "com.google.apps.tiktok.concurrent.AndroidFuturesService"),
                              ("exported", "bool", False)]),
        ("end", "service"),
        *lib("uses-library", "android.ext.adservices"),
        *lib("uses-library", "androidx.window.extensions"),
        *lib("uses-library", "androidx.window.sidecar"),
        *_uses("uses-native-library", "libopenxr.google.so"),  # OVRPort keeps this one
        *lib("uses-library", "com.android.extensions.xr"),
        ("end", "application"),
        ("end", "manifest"),
    ]


def _analysis(**extra) -> Analysis:
    return Analysis(package=PKG, version="1.89.09", label="YouTube", abis=["arm64-v8a"], engine="Other", xr="OpenXR",
                    graphics="?", direct_vrapi=False, libs=[], launcher_activity=None, has_info_category=False,
                    meta_permissions=[], uses_glad_gl=False, unity_msaa_levels=0, oculus_os_classes=False,
                    is_overport_output=False, debuggable=False, extra={"vr_kind": "quest", **extra})


YOUTUBE = _analysis(uses_libraries=["org.apache.http.legacy", "android.ext.adservices", "androidx.window.extensions",
                                    "androidx.window.sidecar", "com.android.extensions.xr"])


def test_reads_uses_libraries():
    assert axml.uses_libraries(build_axml(_manifest(True))) == YOUTUBE.extra["uses_libraries"]
    assert axml.uses_libraries(build_axml(_manifest(False))) == []


def _xml(manifest: bytes) -> str:
    """The manifest as text, attribute prefixes left out (test manifests declare no namespace prefix)."""
    return re.sub(r"\b(ns0|android):", "", AXMLPrinter(manifest).get_xml().decode())


def _check_restored(fixed: bytes, stripped: bytes) -> None:
    x = axml.Axml(fixed)
    assert axml.uses_libraries(fixed) == ["org.apache.http.legacy"]
    el = next(e for e in x.elements() if e.name == "uses-library")
    ids = axml._resource_map(x)[1]
    names = x.strings()
    got = {names[a.name]: (ids[a.name], a.dtype, a.value) for a in el.attrs}
    assert got["name"][:2] == (0x01010003, axml.TYPE_STRING) and names[got["name"][2]] == "org.apache.http.legacy"
    assert got["required"] == (0x0101028E, axml.TYPE_INT_BOOLEAN, 0)
    assert [ids[a.name] for a in el.attrs] == sorted(ids[a.name] for a in el.attrs)  # aapt's order
    # an XML reader sees it under <application>, and everything else as before
    xml = _xml(fixed)
    app = xml[xml.index("<application"):xml.index("</application>")]
    assert 'uses-library name="org.apache.http.legacy" required="false"' in app
    assert 'uses-native-library name="libopenxr.google.so"' in app
    before = _xml(stripped)
    entry = '<uses-library name="org.apache.http.legacy" required="false"/>'
    assert "".join(xml.replace(entry, "").split()) == "".join(before.split())
    assert axml.add_uses_libraries(fixed, ["org.apache.http.legacy"]) is None  # once


def test_add_uses_libraries_round_trip():
    stripped = build_axml(_manifest(False))
    fixed = axml.add_uses_libraries(stripped, ["org.apache.http.legacy", "org.apache.http.legacy"])
    _check_restored(fixed, stripped)
    # like aapt: attribute names mapped to resource ids first ("required" is mapped by the edit)
    mapped = _with_resource_map(stripped, ["label", "name", "debuggable", "exported"])
    fixed = axml.add_uses_libraries(mapped, ["org.apache.http.legacy"])
    _check_restored(fixed, mapped)
    assert axml._resource_map(axml.Axml(fixed))[1][:4] == [0x01010001, 0x01010003, 0x0101000F, 0x01010010]
    assert axml.add_uses_libraries(stripped, []) is None


def _apply(tmp_path, manifest: bytes, analysis: Analysis) -> tuple[bool, bytes, list]:
    apk = tmp_path / "in.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", manifest)
    patch = base.get("frame.keep_system_libraries")
    with ApkWorkspace(apk) as ws:
        ctx = base.ApkContext(ws, analysis, {}, Reporter(), {patch.id: {}})
        changed = patch.apply(ctx)
        return changed, ws.read("AndroidManifest.xml"), patch.validate(ctx)


def test_patch_restores_after_ovrport(tmp_path):
    stripped = build_axml(_manifest(False))
    changed, out, checks = _apply(tmp_path, stripped, YOUTUBE)
    assert changed and [ok for _, ok, _ in checks] == [True]
    _check_restored(out, stripped)


def test_patch_changes_nothing_without_such_libraries(tmp_path):
    stripped = build_axml(_manifest(False))
    for a in (_analysis(), _analysis(uses_libraries=["android.ext.adservices", "com.android.extensions.xr"])):
        changed, out, checks = _apply(tmp_path, stripped, a)
        assert not changed and out == stripped and checks == []
    # OVRPort didn't run (entries still there): byte-identical
    original = build_axml(_manifest(True))
    changed, out, checks = _apply(tmp_path, original, YOUTUBE)
    assert not changed and out == original and [ok for _, ok, _ in checks] == [True]


def test_detect_from_original_manifest(tmp_path):
    path = tmp_path / "youtube.apk"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", build_axml(_manifest(True)))
    a = detect.analyze(path, deep=False)
    assert a.extra["uses_libraries"] == YOUTUBE.extra["uses_libraries"]
    patch = base.get("frame.keep_system_libraries")
    s = patch.detect(YOUTUBE)
    assert s.recommended and "org.apache.http.legacy" in s.reason and "adservices" not in s.reason
    assert patch.applies(YOUTUBE)
    assert patch.detect(_analysis()) is None and not patch.applies(_analysis())
    assert patch.detect(_analysis(uses_libraries=["android.ext.adservices"])) is None
    assert "frame.keep_system_libraries" in engine.suggest(YOUTUBE, use_catalog=False).patches
    assert "frame.keep_system_libraries" not in engine.suggest(_analysis(), use_catalog=False).patches
    # 2D apps aren't converted by OVRPort: their manifests keep the entries (and they stay unchanged)
    flat = _analysis(vr_kind="none", uses_libraries=["org.apache.http.legacy"])
    assert not patch.applies(flat)
    recipe = engine.suggest(flat, use_catalog=False)
    assert "frame.keep_system_libraries" not in recipe.patches and recipe.as_is
    assert not base.works_on_a32(patch)
    assert len(set(LEPTON_LIBRARIES)) == len(LEPTON_LIBRARIES) and "org.apache.http.legacy" in LEPTON_LIBRARIES


def test_triage():
    log = ("10-25 00:26:12.111  1214  1214 E AndroidRuntime: Caused by: java.lang.NoClassDefFoundError: Failed "
           "resolution of: Lorg/apache/http/protocol/HttpRequestHandler;\n"
           "10-25 00:26:12.111  1214  1214 E AndroidRuntime: Caused by: java.lang.ClassNotFoundException: Didn't "
           "find class \"org.apache.http.protocol.HttpRequestHandler\" on path: DexPathList[[zip file \"/data/app/"
           "base.apk\"]]\n")
    r = triage(log, "EXITED", PKG)
    assert [f.suggest for f in r.findings if f.id == "system-library-missing"] == [["frame.keep_system_libraries"]]
    for line in ("java.lang.ClassNotFoundException: javax.obex.ServerRequestHandler",
                 "NoClassDefFoundError: Failed resolution of: Landroid/test/InstrumentationTestCase;"):
        assert any(f.id == "system-library-missing" for f in triage(line, "EXITED", PKG).findings)
    other = "java.lang.ClassNotFoundException: com.android.settingslib.bluetooth.BroadcastSourceInfoHandler"
    assert not any(f.id == "system-library-missing" for f in triage(other, "RUNNING", PKG).findings)
