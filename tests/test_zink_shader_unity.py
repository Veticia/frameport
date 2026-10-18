"""GitHub #382: Unity games whose boot.config merely mentions Vulkan often render with OpenGL ES (SynthRiders), so the
OpenGL ES shader dump/fix stay offered for them, and either setting brings in the shader-fix layer by itself."""
from frameport.build import apk_patches
from frameport.core.models import Analysis
from frameport.patches import base


def analysis(graphics):
    return Analysis(package="com.kluge.SynthRiders", version="3.6.6a2", label="Synth Riders", abis=["arm64-v8a"],
                    engine="Unity", xr="OpenXR", graphics=graphics, direct_vrapi=False,
                    libs=["libunity.so", "libOVRPlugin.so"], launcher_activity=None, has_info_category=False,
                    meta_permissions=[], uses_glad_gl=False, unity_msaa_levels=0, oculus_os_classes=False,
                    is_overport_output=False, debuggable=False, extra={})


def test_shader_dump_offered_for_unity_boot_config_vulkan():
    a = analysis("Vulkan (Unity boot.config)")
    for pid in ("adapter.zink_shader_dump", "adapter.zink_shader_fix", "frame.zink_shader_fix"):
        assert base.get(pid).applies(a), pid


def test_shader_dump_hidden_for_declared_vulkan():
    a = analysis("Vulkan (declared in manifest)")
    for pid in ("adapter.zink_shader_dump", "adapter.zink_shader_fix", "frame.zink_shader_fix"):
        assert not base.get(pid).applies(a), pid


def test_dump_setting_alone_adds_the_layer():
    ids = [p.id for p in apk_patches({"frame.adapter": {}, "adapter.zink_shader_dump": {"value": 1}})]
    assert ids.count("frame.zink_shader_fix") == 1
    ids = [p.id for p in apk_patches({"frame.zink_shader_fix": {}, "adapter.zink_shader_dump": {"value": 1}})]
    assert ids.count("frame.zink_shader_fix") == 1
    assert "frame.zink_shader_fix" not in [p.id for p in apk_patches({"adapter.zink_shader_dump": {"value": 0}})]
