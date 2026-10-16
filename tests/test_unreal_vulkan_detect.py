"""Unreal's Vulkan RHI from the manifest (GameActivity.bSupportsVulkan): Unreal games never declare
android.hardware.vulkan, so they were all "GLES or unknown" and AXRB's automatic backend sent them to the emulator."""
from __future__ import annotations

import zipfile

import pytest
from conftest import build_axml

from frameport.analysis import detect
from frameport.targets import pc_android
from frameport.tools import axrb


def _apk(tmp_path, *, key="com.epicgames.ue4.GameActivity.bSupportsVulkan", value=True, kind="bool",
         engine_lib="libUE4.so"):
    meta = []
    if key is not None:
        meta = [("start", "meta-data", [("name", "str", key), ("value", kind, value)]), ("end", "meta-data")]
    manifest = build_axml([
        ("start", "manifest", [("package", "str", "com.example.unrealgame")]),
        ("start", "application", []),
        ("start", "meta-data", [("name", "str", "com.epicgames.ue4.GameActivity.bHasOBBFiles"),
                                ("value", "bool", True)]),
        ("end", "meta-data"),
        *meta,
        ("end", "application"),
        ("end", "manifest"),
    ])
    p = tmp_path / "game.apk"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("AndroidManifest.xml", manifest)
        z.writestr("classes.dex", b"dex\n035\0")
        z.writestr(f"lib/arm64-v8a/{engine_lib}", b"")
        z.writestr("lib/arm64-v8a/libOVRPlugin.so", b"")
    return p


@pytest.mark.parametrize("key", ["com.epicgames.ue4.GameActivity.bSupportsVulkan",
                                 "com.epicgames.unreal.GameActivity.bSupportsVulkan"])
@pytest.mark.parametrize("engine_lib", ["libUE4.so", "libUnreal.so"])
def test_unreal_vulkan_build_is_vulkan(tmp_path, key, engine_lib):
    a = detect.analyze(_apk(tmp_path, key=key, engine_lib=engine_lib), deep=False)
    assert a.engine == "Unreal"
    assert a.graphics == "Vulkan (Unreal bSupportsVulkan)"
    assert pc_android.choose_backend(None, a.graphics) == axrb.DIRECT


@pytest.mark.parametrize("key,value,kind", [
    ("com.epicgames.ue4.GameActivity.bSupportsVulkan", False, "bool"),  # GLES-only UE 4.21+ build (Espire 1)
    (None, None, None),  # UE 4.19/4.20 write nothing (Vader Immortal, Time Stall)
    ("com.epicgames.ue4.GameActivity.bSupportsVulkan", "true", "str"),  # not a boolean: GameActivity reads false
    ("com.example.GameActivity.bSupportsVulkan", True, "bool"),  # not Unreal's key
])
def test_gles_unreal_builds_stay_gles(tmp_path, key, value, kind):
    a = detect.analyze(_apk(tmp_path, key=key, value=value, kind=kind), deep=False)
    assert a.graphics.startswith("GLES")
    assert pc_android.choose_backend(None, a.graphics) == axrb.EMULATOR


def test_key_only_counts_for_unreal(tmp_path):
    a = detect.analyze(_apk(tmp_path, engine_lib="libunity.so"), deep=False)
    assert a.engine == "Unity" and a.graphics.startswith("GLES")


def test_analysis_version_bumped():
    assert detect.ANALYSIS_VERSION >= 20
