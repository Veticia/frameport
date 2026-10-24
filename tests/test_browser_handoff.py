"""frame.browser_handoff (GitHub #466, Plex): a browser stand-in activity in the app's own manifest + its dex, so
apps that look for a browser before opening a page find one; Lepton then hands the VIEW intent to Steam."""
from __future__ import annotations

import struct
import zipfile

from conftest import build_axml

from frameport.apk import axml
from frameport.apk.dex import Dex
from frameport.apk.workspace import ApkWorkspace
from frameport.core.events import Reporter
from frameport.core.models import Analysis, Recipe
from frameport.patches import base
from frameport.patches.frame.browser_handoff import ACTIVITY, DEX_CLASS
from frameport.recommend import engine

PLEX_LIKE = [
    ("start", "manifest", [("package", "str", "com.example.media")]),
    ("start", "queries", []),
    ("start", "intent", []),
    ("start", "action", [("name", "str", axml.VIEW)]), ("end", "action"),
    ("start", "category", [("name", "str", axml.BROWSABLE)]), ("end", "category"),
    ("start", "data", [("scheme", "str", "https")]), ("end", "data"),
    ("end", "intent"),
    ("end", "queries"),
    ("start", "application", [("label", "str", "Media"), ("debuggable", "bool", False)]),
    ("start", "activity", [("name", "str", "com.example.media.Main"), ("exported", "bool", True)]),
    ("start", "intent-filter", []),
    ("start", "action", [("name", "str", axml.MAIN)]), ("end", "action"),
    ("start", "category", [("name", "str", axml.LAUNCHER)]), ("end", "category"),
    ("end", "intent-filter"),
    # the app's own deep link: names its host, so it isn't a browser
    ("start", "intent-filter", []),
    ("start", "action", [("name", "str", axml.VIEW)]), ("end", "action"),
    ("start", "category", [("name", "str", axml.BROWSABLE)]), ("end", "category"),
    ("start", "data", [("scheme", "str", "https"), ("host", "str", "app.example.com")]), ("end", "data"),
    ("end", "intent-filter"),
    ("end", "activity"),
    ("end", "application"),
    ("end", "manifest"),
]
FRAMEWORK_IDS = {"label": 0x01010001, "name": 0x01010003, "debuggable": 0x0101000F, "exported": 0x01010010,
                 "host": 0x01010028, "scheme": 0x01010027}


def _with_resource_map(manifest: bytes, attrs: list[str]) -> bytes:
    """Like aapt: the attribute names come first in the pool and the resource map follows it. build_axml writes
    no map, so move the given names to the front (a fresh build with them first) and add one."""
    x = axml.Axml(manifest)
    for name in attrs:
        assert x.index(name) is not None
    order = attrs + [s for s in x.strings() if s not in attrs]
    old = x.strings()  # rebuild the pool in the new order, remapping every reference
    remap = {i: order.index(s) for i, s in enumerate(old)}
    data = bytearray(manifest)
    pos = x.pool_off + x.pool_size
    body = bytearray(data[pos:])
    p = 0
    while p < len(body):
        typ, hsz, size = struct.unpack_from("<HHI", body, p)

        def fix(off):
            v = struct.unpack_from("<I", body, off)[0]
            if v != 0xFFFFFFFF:
                struct.pack_into("<I", body, off, remap[v])
        if typ == 0x0102:
            ext = p + hsz
            fix(ext + 4)
            _, _, acount = struct.unpack_from("<HHH", body, ext + 8)
            for i in range(acount):
                a = ext + 20 + 20 * i
                fix(a)
                fix(a + 4)
                fix(a + 8)
                if body[a + 15] == axml.TYPE_STRING:
                    fix(a + 16)
        elif typ == 0x0103:
            fix(p + 20)
        p += size
    enc, offsets = b"", []
    for s in order:
        offsets.append(len(enc))
        enc += struct.pack("<H", len(s)) + s.encode("utf-16-le") + b"\0\0"
    enc += b"\0" * (-len(enc) % 4)
    sstart = 28 + 4 * len(order)
    pool = struct.pack("<HHIIIIII", 1, 28, sstart + len(enc), len(order), 0, 0, sstart, 0)
    pool += struct.pack(f"<{len(order)}I", *offsets) + enc
    resmap = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(attrs)) + struct.pack(
        f"<{len(attrs)}I", *(FRAMEWORK_IDS[a] for a in attrs))
    out = bytearray(struct.pack("<HHI", 3, 8, 0) + pool + resmap + body)
    struct.pack_into("<I", out, 4, len(out))
    return bytes(out)


def _attrs(x: axml.Axml, el: axml.Element) -> dict[str, tuple[int, int, int]]:
    """name -> (resource id, type, value) through the resource map."""
    ids = axml._resource_map(x)[1]
    names = x.strings()
    return {names[a.name]: (ids[a.name] if a.name < len(ids) else None, a.dtype, a.value) for a in el.attrs}


def _check_added(fixed: bytes) -> None:
    x = axml.Axml(fixed)
    names = x.strings()
    assert axml.browser_activities(fixed) == [ACTIVITY]
    stub = next(el for el in x.elements() if el.name == "activity" and x.attr_str(el, "name") == ACTIVITY)
    got = _attrs(x, stub)
    assert got["theme"][:2] == (0x01010000, axml.TYPE_REFERENCE) and got["theme"][2] == axml.THEME_NO_DISPLAY
    assert got["name"][0] == 0x01010003 and names[got["name"][2]] == ACTIVITY
    assert got["exported"][0] == 0x01010010 and got["exported"][2] == 0xFFFFFFFF
    assert got["excludeFromRecents"][0] == 0x01010017
    ids = [axml._resource_map(x)[1][a.name] for a in stub.attrs]
    assert ids == sorted(ids)  # aapt's order: Android's attribute lookup relies on it
    inside, children = False, []
    for kind, name, el in axml.events(fixed):
        if kind == "start" and name == "activity" and x.attr_str(el, "name") == ACTIVITY:
            inside = True
        elif inside and kind == "start":
            children.append((name, x.attr_str(el, "name") or x.attr_str(el, "scheme")))
            assert all(rid in (0x01010003, 0x01010027) for rid, _, _ in _attrs(x, el).values())
        elif inside and name == "activity":
            break
    assert children == [("intent-filter", None), ("action", axml.VIEW), ("category", axml.DEFAULT),
                        ("category", axml.BROWSABLE), ("data", "http"), ("data", "https")]
    # everything that was there reads the same
    assert x.attr_str(next(el for el in x.elements() if el.name == "manifest"), "package") == "com.example.media"
    host = next(el for el in x.elements() if el.name == "data" and x.attr_str(el, "host"))
    assert x.attr_str(host, "host") == "app.example.com"
    assert axml.categories(fixed) >= {axml.LAUNCHER, axml.BROWSABLE, axml.DEFAULT}
    assert x.get_bool("application", "debuggable") is False
    assert axml.add_browser_activity(fixed, ACTIVITY) is None  # once


def test_queries_browser():
    assert axml.queries_browser(build_axml(PLEX_LIKE))
    no_queries = [e for e in PLEX_LIKE if e[1] not in ("queries",)]
    # the same intent outside <queries> (here: inside nothing) doesn't count, nor does a deep link filter
    assert not axml.queries_browser(build_axml([e for e in no_queries if e[1] != "intent"]))
    tabs = [("start", "manifest", []), ("start", "queries", []), ("start", "intent", []),
            ("start", "action", [("name", "str", axml.CUSTOM_TABS_SERVICE)]), ("end", "action"),
            ("end", "intent"), ("end", "queries"), ("end", "manifest")]
    assert axml.queries_browser(build_axml(tabs))
    mail = [("start", "manifest", []), ("start", "queries", []), ("start", "intent", []),
            ("start", "action", [("name", "str", axml.VIEW)]), ("end", "action"),
            ("start", "data", [("scheme", "str", "mailto")]), ("end", "data"),
            ("end", "intent"), ("end", "queries"), ("end", "manifest")]
    assert not axml.queries_browser(build_axml(mail))


def test_stand_in_without_resource_map():
    manifest = build_axml(PLEX_LIKE)
    assert axml.browser_activities(manifest) == []  # the deep link names a host
    _check_added(axml.add_browser_activity(manifest, ACTIVITY))


def test_stand_in_with_aapt_resource_map():
    # names, exported, host, scheme already mapped (Plex has them all); theme and excludeFromRecents get added
    manifest = _with_resource_map(build_axml(PLEX_LIKE), ["label", "name", "debuggable", "exported", "scheme",
                                                          "host"])
    x = axml.Axml(manifest)
    assert axml._resource_map(x)[1][x.index("scheme")] == 0x01010027
    fixed = axml.add_browser_activity(manifest, ACTIVITY)
    _check_added(fixed)
    ids = axml._resource_map(axml.Axml(fixed))[1]
    assert ids[:6] == [0x01010001, 0x01010003, 0x0101000F, 0x01010010, 0x01010027, 0x01010028]
    assert sorted(ids[6:]) == [0x01010000, 0x01010017]


def _analysis(**extra) -> Analysis:
    return Analysis(package="com.example.media", version="1", label="Media", abis=[], engine="Other", xr="?",
                    graphics="?", direct_vrapi=False, libs=[], launcher_activity=None, has_info_category=False,
                    meta_permissions=[], uses_glad_gl=False, unity_msaa_levels=0, oculus_os_classes=False,
                    is_overport_output=False, debuggable=False, extra={"vr_kind": "none", **extra})


def test_patch_adds_activity_and_dex(tmp_path):
    apk = tmp_path / "in.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", build_axml(PLEX_LIKE))
        z.writestr("classes.dex", b"dex\n035\0")
        z.writestr("classes2.dex", b"dex\n035\0")
    patch = base.get("frame.browser_handoff")
    a = _analysis(browser_query=True)
    with ApkWorkspace(apk) as ws:
        ctx = base.ApkContext(ws, a, {}, Reporter(), {"frame.browser_handoff": {}})
        assert patch.apply(ctx)
        assert [ok for _, ok, _ in patch.validate(ctx)] == [True]
        assert not patch.apply(base.ApkContext(ws, a, {}, Reporter(), {}))  # once
        out = ws.write(tmp_path / "out.apk")
    with zipfile.ZipFile(out) as z:
        added = z.read("classes3.dex")
        _check_added(z.read("AndroidManifest.xml"))
    assert DEX_CLASS.decode() in Dex(added).class_names()


def test_suggestion_and_applicability():
    patch = base.get("frame.browser_handoff")
    flat, asks = _analysis(), _analysis(browser_query=True)
    vr_asks = _analysis(browser_query=True, vr_kind="quest")
    assert patch.applies(flat) and patch.applies(vr_asks)
    assert not patch.applies(_analysis(vr_kind="quest"))
    assert patch.detect(flat) is None
    s = patch.detect(asks)
    assert s is not None and not s.recommended  # opt-in
    assert not patch.needs_vr


def test_flat_app_is_built_once_an_apk_patch_is_on():
    """A 2D app has no "install unchanged" switch: turning on an APK patch must build it (Plex's recipe kept
    as_is with frame.sdl_clipboard on, so nothing was ever applied)."""
    a = _analysis(browser_query=True)
    r = Recipe(package=a.package, title="Media", overport=False, as_is=True, patches={
        "frame.hw_video_decode": {}, "frame.sdl_clipboard": {}})  # install-stage + not applicable (no SDL)
    engine.flat_as_is(a, r)
    assert r.as_is
    r.patches["frame.browser_handoff"] = {}
    engine.flat_as_is(a, r)
    assert not r.as_is
    r.patches.pop("frame.browser_handoff")
    engine.flat_as_is(a, r)
    assert r.as_is
    vr = Recipe(package=a.package, title="VR", as_is=True, patches={"frame.browser_handoff": {}})
    engine.flat_as_is(_analysis(vr_kind="quest"), vr)
    assert vr.as_is  # VR games keep their own switch
