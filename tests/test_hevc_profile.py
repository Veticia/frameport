"""HEVC Main 10 detection in the shared video codec: such streams decode in software, never on Iris's NV12 path."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_profile_from_parameter_sets(tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or os.name == "nt":
        pytest.skip("needs a POSIX host C compiler")
    binary = tmp_path / "hevc-profile-test"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "native/hevc"),
                    str(ROOT / "tests/fixtures/src/hevc_profile_test.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
