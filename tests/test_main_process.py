"""frame.main_process (GitHub #178): a game activity in its own process leaves no main process, which is the one
Lepton watches; it closed Halo CE VR about a minute in ("Target app ... did not start")."""
from conftest import build_axml

from frameport.apk import axml
from frameport.patches import base

PKG = "com.halo.decomp.vr"


def _activity(name, process=None, category=axml.LAUNCHER):
    attrs = [("name", "str", name)] + ([("process", "str", process)] if process else [])
    return [
        ("start", "activity", attrs),
        ("start", "intent-filter", []),
        ("start", "action", [("name", "str", axml.MAIN)]), ("end", "action"),
        ("start", "category", [("name", "str", category)]), ("end", "category"),
        ("end", "intent-filter"),
        ("end", "activity"),
    ]


def _halo_manifest():
    """Halo CE VR's layout: the VR activity runs in ":halo_game", the 2D launcher starts it."""
    return build_axml([
        ("start", "manifest", [("package", "str", PKG)]),
        ("start", "application", []),
        *_activity("com.halo.decomp.HaloActivity", ":halo_game", axml.VR_CATEGORY),
        *_activity("com.halo.decomp.LauncherActivity"),
        ("start", "service", [("name", "str", "com.halo.decomp.Crash"), ("process", "str", ":crash")]),
        ("end", "service"),
        ("end", "application"),
        ("end", "manifest"),
    ])


def _processes(manifest):
    x = axml.Axml(manifest)
    return {x.attr_str(e, "name"): x.attr_str(e, "process") for e in x.elements() if e.name in ("activity", "service")}


def test_detects_and_moves_the_game_activity():
    m = _halo_manifest()
    assert axml.separate_process_activities(m) == ["com.halo.decomp.HaloActivity"]
    fixed = axml.set_main_process(m, axml.separate_process_activities(m))
    assert axml.separate_process_activities(fixed) == []
    assert _processes(fixed) == {"com.halo.decomp.HaloActivity": PKG, "com.halo.decomp.LauncherActivity": None,
                                 "com.halo.decomp.Crash": ":crash"}  # services keep their own process
    assert len(fixed) == len(m)  # the package name is already in the pool: one value changes, nothing moves
    assert axml.set_main_process(fixed, ["com.halo.decomp.HaloActivity"]) is None


def test_main_process_activities_are_left_alone():
    m = build_axml([
        ("start", "manifest", [("package", "str", PKG)]),
        ("start", "application", []),
        *_activity("com.x.Game", PKG),  # explicitly the main process
        *_activity("com.x.Other"),
        ("end", "application"),
        ("end", "manifest"),
    ])
    assert axml.separate_process_activities(m) == []
    assert axml.set_main_process(m, ["com.x.Game"]) is None


def test_patch_is_offered_only_when_detected():
    patch = base.get("frame.main_process")
    assert patch.default_on

    class A:
        extra = {"separate_process": ["com.halo.decomp.HaloActivity"]}
    assert patch.applies(A()) and patch.detect(A()).recommended
    A.extra = {}
    assert not patch.applies(A()) and patch.detect(A()) is None
