"""vk_no_fossilize / vk_pipeline_fallback (GitHub #349): the Vulkan shim leaves Lepton's Fossilize layer out and
creates pipelines the driver refuses with blank shaders."""
import re
import shutil
import struct
import subprocess
from pathlib import Path

import pytest
import yaml

from frameport.patches import base
from frameport.patches.settings import UI

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("key", ["vk_no_fossilize", "vk_pipeline_fallback"])
def test_settings_registered_off_by_default(key):
    base.load_all()
    p = base.REGISTRY[f"adapter.{key}"]
    assert p.category == "adapter" and p.default == 0 and not p.default_on
    assert UI[key]["group"] == "troubleshooting"


def test_artifact_reads_the_settings():
    so = (ROOT / "artifacts/arm64-v8a/libfp_vk.so").read_bytes()
    for s in (b"vk_no_fossilize=", b"vk_pipeline_fallback=", b"VK_LAYER_fossilize", b"retried with a blank",
              b"_ZN7android11GraphicsEnv14setDebugLayers"):
        assert s in so


def _blank_shaders():
    src = (ROOT / "native/vkshim/vkshim.c").read_text()
    out = {}
    for name in ("FS", "VS"):
        body = re.search(rf"FALLBACK_{name}\[\] = \{{(.*?)\}};", src, re.S).group(1)
        words = [int(w, 16) for w in re.findall(r"0x[0-9a-f]{8}", body)]
        out[name] = struct.pack(f"<{len(words)}I", *words)
    return out


def test_blank_shaders_are_valid_spirv(tmp_path):
    shaders = _blank_shaders()
    assert all(b[:4] == b"\x03\x02\x23\x07" for b in shaders.values())
    if not shutil.which("spirv-val"):
        pytest.skip("needs spirv-val")
    for name, data in shaders.items():
        f = tmp_path / f"{name}.spv"
        f.write_bytes(data)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.0", str(f)], check=True)


def test_triage_suggests_both_settings():
    rules = yaml.safe_load((ROOT / "catalog/triage.yaml").read_text())
    rules = rules.get("signatures", rules) if isinstance(rules, dict) else rules
    rule = next(r for r in rules if r["id"] == "vulkan-pipeline-refused")
    pat = re.compile(rule["pattern"])
    assert pat.search("E UE4: Shader compilation failures are Fatal.")
    assert pat.search("I FrameBridge: vk shim: vkCreateGraphicsPipelines failed: -1 (1 pipeline(s); failure 2)")
    assert not pat.search("I FrameBridge: vk shim: vkCreateGraphicsPipelines failed: 1000297000 (1 pipeline(s))")
    assert set(rule["suggest"]) == {"adapter.vk_no_fossilize", "adapter.vk_pipeline_fallback"}
