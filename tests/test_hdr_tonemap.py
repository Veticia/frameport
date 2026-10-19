"""HDR (PQ/HLG, BT.2020) to SDR BT.709 tone mapping of the shared video codec's software Main 10 path."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_tone_mapping(tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or os.name == "nt":
        pytest.skip("needs a POSIX host C compiler")
    binary = tmp_path / "hdr-tonemap-test"
    subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "native/hevc"),
                    str(ROOT / "tests/fixtures/src/hdr_tonemap_test.c"), "-o", str(binary), "-lm"], check=True)
    subprocess.run([str(binary)], check=True)
