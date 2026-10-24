"""Host regression for FrameBridge's colour-space compatibility implementation."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <openxr/openxr.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int log_count;
#define LOG(...) ((void)++log_count)
#include "color_space.c"
int main(void) {
    assert(!color_space_emulation("xrSetColorSpaceFB"));
    emulate_color_space = 1;
    PFN_xrEnumerateColorSpacesFB enumerate =
        (PFN_xrEnumerateColorSpacesFB)color_space_emulation("xrEnumerateColorSpacesFB");
    PFN_xrSetColorSpaceFB set = (PFN_xrSetColorSpaceFB)color_space_emulation("xrSetColorSpaceFB");
    assert(enumerate && set && !color_space_emulation("xrOther"));
    uint32_t count = 0;
    XrColorSpaceFB spaces[9];
    spaces[0] = XR_COLOR_SPACE_MAX_ENUM_FB;
    spaces[8] = XR_COLOR_SPACE_MAX_ENUM_FB;
    assert(enumerate(0, 0, NULL, NULL) == XR_ERROR_VALIDATION_FAILURE);
    assert(enumerate(0, 0, &count, NULL) == XR_SUCCESS && count == 8);
    assert(enumerate(0, 8, &count, NULL) == XR_ERROR_VALIDATION_FAILURE);
    assert(enumerate(0, 7, &count, spaces) == XR_ERROR_SIZE_INSUFFICIENT && count == 8);
    assert(spaces[0] == XR_COLOR_SPACE_MAX_ENUM_FB);
    assert(enumerate(0, 9, &count, spaces) == XR_SUCCESS && count == 8);
    assert(spaces[8] == XR_COLOR_SPACE_MAX_ENUM_FB);
    const XrColorSpaceFB expected[] = {
        XR_COLOR_SPACE_UNMANAGED_FB, XR_COLOR_SPACE_REC2020_FB, XR_COLOR_SPACE_REC709_FB,
        XR_COLOR_SPACE_RIFT_CV1_FB, XR_COLOR_SPACE_RIFT_S_FB, XR_COLOR_SPACE_QUEST_FB,
        XR_COLOR_SPACE_P3_FB, XR_COLOR_SPACE_ADOBE_RGB_FB
    };
    for (unsigned i = 0; i < 8; ++i) {
        assert(spaces[i] == expected[i]);
        assert(set(0, spaces[i]) == XR_SUCCESS);
    }
    assert(log_count == 1);
    assert(set(0, (XrColorSpaceFB)-1) == XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB);
    assert(set(0, XR_COLOR_SPACE_MAX_ENUM_FB) == XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB);
    XrSystemColorSpacePropertiesFB color = {XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB};
    XrBaseOutStructure unknown = {XR_TYPE_UNKNOWN, (XrBaseOutStructure *)&color};
    XrSystemProperties properties = {XR_TYPE_SYSTEM_PROPERTIES, &unknown};
    color_space_properties(&properties);
    assert(color.colorSpace == XR_COLOR_SPACE_REC709_FB);
    assert(unknown.next == (XrBaseOutStructure *)&color && color.next == NULL);
    emulate_color_space = 0;
    color.colorSpace = XR_COLOR_SPACE_P3_FB;
    color_space_properties(&properties);
    color_space_properties(NULL);
    assert(color.colorSpace == XR_COLOR_SPACE_P3_FB);
    assert(!color_space_emulation("xrEnumerateColorSpacesFB"));
    return 0;
}
"""


def test_color_space_host(tmp_path):
    cc = shutil.which("cc") or shutil.which("clang")
    inc = next((ROOT / "native/.cache").glob("openxr-*"), None)
    if not cc or not inc:
        pytest.skip("needs host C compiler and cached OpenXR headers (native/build.py)")
    source = tmp_path / "color_space_test.c"
    source.write_text(HARNESS)
    exe = tmp_path / "color_space_test"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-missing-field-initializers", "-I", str(inc),
                    "-I", str(ROOT / "native/adapter"), str(source), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_color_space_artifacts():
    for abi in ("arm64-v8a", "armeabi-v7a"):
        data = (ROOT / "artifacts" / abi / "libopenxr_loader_generic.so").read_bytes()
        for marker in (b"XR_FB_color_space", b"color_space:", b"xrEnumerateColorSpacesFB", b"xrSetColorSpaceFB"):
            assert marker in data


def test_color_space_hooks():
    source = (ROOT / "native/adapter/frame_adapter.c").read_text()
    assert 'XR_FB_COLOR_SPACE_EXTENSION_NAME, 3}' in source
    assert 'emulate_color_space = fb_color_space && XR_SUCCEEDED(result) && requested_color_space' in source
    assert 'if (fb_color_space && !has_color_space)' in source  # only offered when the setting is on
    assert 'if (XR_SUCCEEDED(result)) color_space_properties(properties)' in source
    assert 'if (!emulated) emulated = color_space_emulation(name)' in source
    assert 'name ? color_space_emulation(name) : NULL' in source


def test_extension_enumeration_host(tmp_path):
    """Compile the actual adapter enumeration hook against a runtime with/without the extension."""
    cc = shutil.which("cc") or shutil.which("clang")
    inc = next((ROOT / "native/.cache").glob("openxr-*"), None)
    if not cc or not inc:
        pytest.skip("needs host C compiler and cached OpenXR headers")
    adapter = (ROOT / "native/adapter/frame_adapter.c").read_text()
    hook = adapter.split("XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(", 1)[1]
    hook = "XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(" + hook.split(
        "XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateApiLayerProperties(", 1)[0]
    source = tmp_path / "enumeration.c"
    source.write_text(r"""
#include <openxr/openxr.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int native_color, fail;
static XrResult runtime_enum(const char *layer, uint32_t cap, uint32_t *count, XrExtensionProperties *out) {
    if (layer) return XR_ERROR_API_LAYER_NOT_PRESENT;
    if (fail) return XR_ERROR_RUNTIME_FAILURE;
    *count = native_color;
    if (cap && native_color) {
        strcpy(out[0].extensionName, XR_FB_COLOR_SPACE_EXTENSION_NAME);
        out[0].extensionVersion = 42;
    }
    return XR_SUCCESS;
}
static PFN_xrVoidFunction lookup(XrInstance instance, const char *name) {
    (void)instance; (void)name; return (PFN_xrVoidFunction)runtime_enum;
}
static int runtime_has_fb_passthrough, runtime_has_render_model;
static int hide_fb_foveation, foveation_fix, hide_space_warp, scene_emul, passthrough_emul;
static int space_warp_emul, sw_runtime_has_extension;
static int controller_models, render_models_available;
static int fb_color_space = 1;  // the setting is off by default; on here
#define SCENE_EXTENSION_COUNT 1
static const char *scene_extensions[] = {"XR_FAKE_scene"};
static int fb_is_foveation_extension(const char *name) { (void)name; return 0; }
""" + hook + r"""
int main(void) {
    uint32_t count;
    XrExtensionProperties out[2] = {{XR_TYPE_EXTENSION_PROPERTIES}, {XR_TYPE_EXTENSION_PROPERTIES}};
    assert(xrEnumerateInstanceExtensionProperties(0, 0, &count, 0) == XR_SUCCESS && count == 1);
    assert(xrEnumerateInstanceExtensionProperties(0, 1, &count, out) == XR_SUCCESS && count == 1);
    assert(!strcmp(out[0].extensionName, XR_FB_COLOR_SPACE_EXTENSION_NAME) && out[0].extensionVersion == 3);
    assert(xrEnumerateInstanceExtensionProperties(0, 1, &count, 0) == XR_ERROR_VALIDATION_FAILURE);
    assert(xrEnumerateInstanceExtensionProperties(0, 0, 0, 0) == XR_ERROR_VALIDATION_FAILURE);
    assert(xrEnumerateInstanceExtensionProperties("layer", 0, &count, 0) == XR_ERROR_API_LAYER_NOT_PRESENT);
    passthrough_emul = 1;
    assert(xrEnumerateInstanceExtensionProperties(0, 1, &count, out) == XR_ERROR_SIZE_INSUFFICIENT && count == 2);
    passthrough_emul = 0;
    native_color = 1;
    assert(xrEnumerateInstanceExtensionProperties(0, 2, &count, out) == XR_SUCCESS && count == 1);
    assert(out[0].extensionVersion == 42);
    fail = 1;
    assert(xrEnumerateInstanceExtensionProperties(0, 0, &count, 0) == XR_ERROR_RUNTIME_FAILURE);
    return 0;
}
""")
    exe = tmp_path / "enumeration"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                    "-I", str(inc), str(source), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
