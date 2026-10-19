"""Full swapchain recommendations and opt-in OVRPlugin routing."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest
from test_patches import _analysis

from frameport.patches import base
from frameport.patches.frame.adapter import needs_xrshim
from frameport.patches.settings import UI, adapter_settings

ROOT = Path(__file__).resolve().parents[1]


def test_setting_is_opt_in_and_routes_ovrplugin():
    base.load_all()
    patch = base.get("adapter.fixed_resolution")
    assert patch.default == 0
    assert patch.detect(_analysis()) is None
    assert not adapter_settings({}).get("fixed_resolution")
    assert needs_xrshim({patch.id: {"value": 1}})
    assert not needs_xrshim({patch.id: {"value": 0}, "adapter.haptic_fix": {"value": 0}})
    assert UI[patch.key]["group"] == "picture"
    assert patch.applies(_analysis(xr="OpenXR", libs=[]))
    assert patch.applies(_analysis(xr="VrApi", libs=["libOVRPlugin.so"]))
    assert not patch.applies(_analysis(xr="VrApi", libs=[]))
    assert not patch.applies(_analysis(abis=["armeabi-v7a"], xr="OpenXR"))


@pytest.mark.native
def test_layer_dimensions(tmp_path):
    inc = next(iter((ROOT / "native/.cache").glob("openxr-*")), None)
    cc = shutil.which("cc")
    if not os.environ.get("FRAMEPORT_NATIVE_TESTS") or not inc or not cc:
        pytest.skip("needs FRAMEPORT_NATIVE_TESTS=1, OpenXR headers and cc")
    exe = tmp_path / "fixed-resolution"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-missing-field-initializers", "-I", str(inc),
                    "-I", str(ROOT / "native/adapter"),
                    str(ROOT / "tests/fixtures/src/fixed_resolution_test.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_triage_only_offers_fix_for_offset_rects():
    from frameport.validate.triage import triage
    log = "projection view 0 rect change: swapchain=0x123 rect=0,32 2736x2736 array=0"
    found = [f for f in triage(log).findings if f.id == "dynamic-resolution-foveation"]
    assert found and found[0].suggest == ["adapter.fixed_resolution"] and found[0].question
    assert not any(f.id == "dynamic-resolution-foveation" for f in
                   triage(log.replace("rect=0,32", "rect=0,0")).findings)
    assert not any(f.id == "dynamic-resolution-foveation" for f in
                   triage(log + "\nfixed_resolution: recommended 2704x2704 -> 2768x2768").findings)


def test_catalog_requires_app_with_the_fix(monkeypatch):
    import yaml

    import frameport
    from frameport.recommend.catalog import unusable_reason
    entry = yaml.safe_load((ROOT / "catalog/games/com.camouflaj.manta.yaml").read_text())
    assert entry["adapter"]["fixed_resolution"] == 1
    monkeypatch.setattr(frameport, "__version__", "1.2.1")
    assert unusable_reason(entry) == "needs FramePort 1.2.2"
    monkeypatch.setattr(frameport, "__version__", "1.2.2")
    assert unusable_reason(entry) is None


@pytest.mark.native
def test_recommendation_wrapper(tmp_path):
    inc = next(iter((ROOT / "native/.cache").glob("openxr-*")), None)
    cc = shutil.which("cc")
    if not os.environ.get("FRAMEPORT_NATIVE_TESTS") or not inc or not cc:
        pytest.skip("needs FRAMEPORT_NATIVE_TESTS=1, OpenXR headers and cc")
    # Compile the actual wrapper with a stand-in runtime and swapchain table.
    source = (ROOT / "native/adapter/frame_adapter.c").read_text()
    wrapper = source[source.index("static XRAPI_ATTR XrResult XRAPI_CALL fixed_recommended_resolution"):
                     source.index("// Clamp `r` to a w x h image;")]
    harness = (ROOT / "tests/fixtures/src/fixed_resolution_wrapper_test.c").read_text()
    unit = tmp_path / "wrapper.c"
    unit.write_text(harness.replace("/* WRAPPER */", wrapper))
    exe = tmp_path / "wrapper"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-missing-field-initializers", "-I", str(inc),
                    "-I", str(ROOT / "native/adapter"), str(unit), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
