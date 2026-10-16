"""GLES Quest games in the build for this PC (AXRB's emulator: eye images travel as pixels)."""
from frameport.core.models import Analysis
from frameport.patches import base


def analysis(graphics, **extra):
    return Analysis(package="p", version="1", label="P", abis=["arm64-v8a"], engine="Unity", xr="OpenXR",
                    graphics=graphics, direct_vrapi=False, libs=["libil2cpp.so"], launcher_activity=None,
                    has_info_category=False, meta_permissions=[], uses_glad_gl=False, unity_msaa_levels=0,
                    oculus_os_classes=False, is_overport_output=False, debuggable=False, extra=extra)


def test_gles_games_get_half_size_eyes_and_multipass_on_the_pc():
    """The PC build caps FrameBridge's scale at 0.5 for GLES (Lucky's Tale 9 -> 30 fps); Vulkan games (shared GPU
    images) keep theirs; Unity GLES + Oculus XR Plugin renders multipass (gfxstream drops multiview draws)."""
    gles, vulkan = analysis("GLES or unknown (no Vulkan declaration)", oculus_xr_plugin=True), analysis("Vulkan (x)")
    recipe = {"frame.adapter": {}, "adapter.scale": {"value": 1.2}}
    sel = base.pc_selection(recipe, gles)
    assert sel["adapter.scale"] == {"value": 0.5} and "frame.unity_multipass" in sel
    assert base.pc_selection(sel, gles) == sel  # idempotent
    assert base.pc_selection({"frame.adapter": {}}, gles)["adapter.scale"] == {"value": 0.5}
    assert base.pc_selection({"frame.adapter": {}, "adapter.scale": {"value": 0.6}}, gles)["adapter.scale"] == {
        "value": 0.5}
    vk = base.pc_selection(recipe, vulkan)
    assert vk["adapter.scale"] == {"value": 1.2} and "frame.unity_multipass" not in vk
    plain = analysis("GLES or unknown (no Vulkan declaration)", oculus_xr_plugin=False)  # built-in VR: no plugin
    assert "frame.unity_multipass" not in base.pc_selection({"frame.adapter": {}}, plain)
    assert "frame.unity_multipass" not in base.pc_selection({"frame.adapter": {}})  # no analysis: the recipe only


def test_pc_gles_note():
    from frameport import pipeline
    from frameport.ui.help import HELP

    assert pipeline.pc_gles({"analysis": {"graphics": "GLES or unknown (no Vulkan declaration)"}})
    assert not pipeline.pc_gles({"analysis": {"graphics": "Vulkan (libvulkan.so)"}})
    assert not pipeline.pc_gles({"kind": "rift", "analysis": {"graphics": "GLES"}})
    assert "half size" in HELP["pc_android_gles"]
