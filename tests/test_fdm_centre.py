"""Host harness for the foveation centring layer (native/foveation/centre.h, GitHub #394)."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which("cc") or shutil.which("clang")


def vulkan_headers():
    candidates = [Path("/usr/include"), *(ROOT / "native/.cache").glob(
        "ndk-*/toolchains/llvm/prebuilt/*/sysroot/usr/include")]
    return next((p for p in candidates if (p / "vulkan/vulkan_core.h").is_file()
                 and (p / "vulkan/vk_layer.h").is_file()), None)


@pytest.mark.native
@pytest.mark.skipif(not os.environ.get("FRAMEPORT_NATIVE_TESTS") or not CC,
                    reason="set FRAMEPORT_NATIVE_TESTS=1; needs a host C compiler and Vulkan headers")
def test_density_map_offsets_follow_the_eyes(tmp_path):
    headers = vulkan_headers()
    if headers is None:
        pytest.skip("needs Vulkan headers (host development package or cached NDK)")
    inc = tmp_path / "include"
    shutil.copytree(headers / "vulkan", inc / "vulkan")  # only these: the NDK sysroot shadows host libc headers
    if (headers / "vk_video").is_dir():
        shutil.copytree(headers / "vk_video", inc / "vk_video")
    exe = tmp_path / "centre"
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                    "-I", str(inc), "-I", str(ROOT / "native/foveation"),
                    str(ROOT / "tests/native/fdm_centre_harness.c"), "-lm", "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == "ok"


def test_fixed_and_tracked_launchers_keep_valve_flags():
    # The centring layer reads Valve's own settings; FramePort's launcher exports stay as they were.
    src = (ROOT / "native/foveation/darken.c").read_text()
    assert '"VK_LAYER_VALVE_fdm_injection"' in src and "CENTRE_NAME" in src
    assert "FRAMEPORT_CLIMB_STEREO_OFFSETS" in src  # The Climb 2's private correction stays alone
