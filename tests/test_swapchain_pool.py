"""Host harness for the GLES swapchain pool, with runtime and GL doubles."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / "native/.cache/openxr-f2448a8797"
if not INC.is_dir():
    INC = None
CC = shutil.which("cc") or shutil.which("clang")


@pytest.mark.native
@pytest.mark.skipif(not os.environ.get("FRAMEPORT_NATIVE_TESTS") or not INC or not CC,
                    reason="set FRAMEPORT_NATIVE_TESTS=1; needs cached OpenXR and host GLES/EGL headers")
def test_pool_runtime_and_gl(tmp_path):
    # Copy only graphics headers: adding the NDK sysroot itself shadows host libc headers.
    inc = tmp_path / "include"
    candidates = [Path("/usr/include"), *(ROOT / "native/.cache").glob(
        "ndk-*/toolchains/llvm/prebuilt/*/sysroot/usr/include")]
    graphics = next((p for p in candidates if all((p / d).is_dir() for d in ("GLES3", "EGL", "KHR"))), None)
    if graphics is None:
        pytest.skip("needs GLES3/EGL/KHR headers (host development packages or cached NDK)")
    for directory in ("GLES3", "EGL", "KHR"):
        shutil.copytree(graphics / directory, inc / directory)
    # Exercise production rect_clamp too, rather than a second implementation in the harness.
    adapter = (ROOT / "native/adapter/frame_adapter.c").read_text()
    start = adapter.index("static int clamp_rect(")
    end = adapter.index("\n}", start) + 2
    (inc / "pool_clamp.h").write_text(adapter[start:end])
    exe = tmp_path / "pool"
    subprocess.run([CC, "-std=c11", "-pthread", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                    "-I", str(INC), "-I", str(inc), "-I", str(ROOT / "native/adapter"),
                    str(ROOT / "tests/native/swapchain_pool_harness.c"), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == "ok"
