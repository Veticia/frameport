import os
import struct

import pytest

from frameport.patches.frame.meta_depth_occlusion import (
    _patch_laser_dance_assets,
    _patch_shader_proximity_fade,
)

lz4_block = pytest.importorskip("lz4.block")  # UnityPy's dependency; the patch skips the shader fix without it


def test_patch_shader_proximity_fade_synthetic():
    c_2_5 = struct.pack('<f', -2.5)
    c_2_0 = struct.pack('<f', -2.0)
    c_10_0 = struct.pack('<f', 10.0)

    # Construct synthetic raw blob
    # Header: count=1
    # Entry 0: off=16, length=64, seg=0
    # subprogram at offset 16 contains c_2_5 and c_2_0
    sub = b'SMOL' + b'\x00' * 10 + c_2_5 + b'\x00' * 4 + c_2_0 + b'\x00' * 38
    assert len(sub) == 64

    entry0 = struct.pack('<III', 16, len(sub), 0)
    raw = struct.pack('<I', 1) + entry0 + sub
    # Pad raw with pseudo-random repetitive data to > 60000 bytes so comp_sz > 5000
    noise = (os.urandom(256) * 300)[:60000]
    raw = raw + noise
    decomp_sz = len(raw)

    comp = lz4_block.compress(raw, mode='high_compression', compression=10, store_size=False)
    comp_sz = len(comp)

    # Wrap in asset data: decomp_sz (uint32), comp_sz (uint32), comp_bytes
    asset_data = struct.pack('<II', decomp_sz, comp_sz) + comp

    new_data, subs = _patch_shader_proximity_fade(asset_data)
    assert new_data is not None
    assert subs == 1
    assert len(new_data) == len(asset_data)

    # Decompress new_data and verify c_10_0 is present and c_2_0 is replaced
    new_comp = new_data[8:8+comp_sz]
    decomp = lz4_block.decompress(new_comp, uncompressed_size=decomp_sz)
    assert c_10_0 in decomp
    assert c_2_0 not in decomp[:80] # In subprogram area


def test_patch_laser_dance_assets():
    # Test split0: Material 97 shader redirection (281 -> 279)
    split0_data = bytearray(0x7b2a0 + 32)
    split0_data[0x7b2a0 : 0x7b2a0 + 12] = struct.pack("<Iq", 0, 281)
    new_split0, note0 = _patch_laser_dance_assets("sharedassets0.assets.split0", bytes(split0_data))
    assert new_split0 is not None
    assert struct.unpack("<Iq", new_split0[0x7b2a0 : 0x7b2a0 + 12]) == (0, 279)
    assert "Material 97" in note0

    # Test split79: LineRenderer materials redirection (97 -> 45)
    split79_data = bytearray(0x20d78 + 32)
    split79_data[0x20a38 : 0x20a38 + 12] = struct.pack("<Iq", 0, 97)
    split79_data[0x20d78 : 0x20d78 + 12] = struct.pack("<Iq", 0, 97)
    new_split79, note79 = _patch_laser_dance_assets("sharedassets0.assets.split79", bytes(split79_data))
    assert new_split79 is not None
    assert struct.unpack("<Iq", new_split79[0x20a38 : 0x20a38 + 12]) == (0, 45)
    assert struct.unpack("<Iq", new_split79[0x20d78 : 0x20d78 + 12]) == (0, 45)
    assert "LineRenderer" in note79

    # Test split80: LineLod distance rules
    split80_data = bytearray(0xeb40 + 500)
    split80_data[0x9958 + 52 : 0x9958 + 56] = struct.pack("<f", 2.30)
    split80_data[0x9958 + 124 : 0x9958 + 128] = struct.pack("<f", 2.30)
    split80_data[0xeb40 + 360 : 0xeb40 + 364] = struct.pack("<f", 2.20)
    split80_data[0xeb40 + 432 : 0xeb40 + 436] = struct.pack("<f", 2.20)
    new_split80, note80 = _patch_laser_dance_assets("sharedassets0.assets.split80", bytes(split80_data))
    assert new_split80 is not None
    assert round(struct.unpack("<f", new_split80[0x9958 + 52 : 0x9958 + 56])[0], 1) == 999.0
    assert struct.unpack("<f", new_split80[0x9958 + 124 : 0x9958 + 128])[0] == 0.0
    assert round(struct.unpack("<f", new_split80[0xeb40 + 360 : 0xeb40 + 364])[0], 1) == 9999.0
    assert struct.unpack("<f", new_split80[0xeb40 + 432 : 0xeb40 + 436])[0] == 0.0
    assert "LineLod" in note80


def test_analysis_version_bumped():
    from frameport.analysis.detect import ANALYSIS_VERSION

    assert ANALYSIS_VERSION >= 24


def test_patch_shader_proximity_fade_ignores_unknown_build_and_unrelated_shader():
    c_2_5 = struct.pack('<f', -2.5)
    c_2_0 = struct.pack('<f', -2.0)

    # Subprogram with constants but NO known shader keyword/signature
    sub = b'UNRELATED_SHADER' + b'\x00' * 10 + c_2_5 + b'\x00' * 4 + c_2_0 + b'\x00' * 26
    entry0 = struct.pack('<III', 16, len(sub), 0)
    raw = struct.pack('<I', 1) + entry0 + sub
    noise = (os.urandom(256) * 300)[:60000]
    raw = raw + noise
    decomp_sz = len(raw)
    comp = lz4_block.compress(raw, mode='high_compression', compression=10, store_size=False)
    asset_data = struct.pack('<II', decomp_sz, len(comp)) + comp

    # If is_known_build=False, returns None, 0 immediately
    res, subs = _patch_shader_proximity_fade(asset_data, is_known_build=False)
    assert res is None and subs == 0

    # Even with is_known_build=True, unrelated shader without known signature is not modified
    res, subs = _patch_shader_proximity_fade(asset_data, is_known_build=True)
    assert res is None and subs == 0


def test_reanalysis_keeps_user_recipes_without_the_patch(tmp_path):
    """Catalog-only patch: a re-analysis that finds the keywords doesn't change a recipe the user edited."""
    from test_patches import _analysis

    from frameport.core import library
    from frameport.pipeline import _store_analysis

    apk = tmp_path / "game.apk"
    apk.write_bytes(b"PK\x03\x04")
    recipe = {"package": "com.test.mrgame", "source": "user", "patches": {}, "reasons": {}}
    library.upsert_game("com.test.mrgame", apk=str(apk), recipe=recipe,
                        analysis={"package": "com.test.mrgame", "engine": "Unity", "extra": {}})
    _store_analysis("com.test.mrgame", _analysis(package="com.test.mrgame", engine="Unity",
                                                 extra={"meta_depth_occlusion": True}))
    assert "frame.meta_depth_occlusion" not in library.game("com.test.mrgame")["recipe"]["patches"]


def _ld_files(build_ok=True):
    split0 = bytearray(0x7B2A0 + 32)
    struct.pack_into("<Iq", split0, 0x7B2A0, 0, 281)
    split79 = bytearray(0x20D78 + 32)
    struct.pack_into("<Iq", split79, 0x20A38, 0, 97)
    struct.pack_into("<Iq", split79, 0x20D78, 0, 97 if build_ok else 96)
    split80 = bytearray(0xEB40 + 500)
    for off, v in ((0x9958 + 52, 2.30), (0x9958 + 124, 2.30), (0xEB40 + 360, 2.20), (0xEB40 + 432, 2.20)):
        struct.pack_into("<f", split80, off, v)
    d = "assets/bin/Data/"
    return {d + "sharedassets0.assets.split0": bytes(split0), d + "sharedassets0.assets.split79": bytes(split79),
            d + "sharedassets0.assets.split80": bytes(split80)}


@pytest.mark.parametrize("build_ok", [True, False])
def test_laser_dance_asset_fixes_only_for_the_known_build(tmp_path, build_ok):
    import zipfile

    from test_patches import _analysis

    from frameport.apk.workspace import ApkWorkspace
    from frameport.core.events import Reporter
    from frameport.patches import base
    from frameport.patches.frame.meta_depth_occlusion import MetaDepthOcclusion, laser_dance_build

    apk = tmp_path / "ld.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", b"<manifest/>")
        for name, data in _ld_files(build_ok).items():
            z.writestr(name, data)
    with ApkWorkspace(apk) as ws:
        assert bool(laser_dance_build(ws)) is build_ok
        ctx = base.ApkContext(ws, _analysis(package="com.mrf.tvb.laser_dance", engine="Unity"), {}, Reporter(), {})
        MetaDepthOcclusion().apply(ctx)
        split0 = ws.read("assets/bin/Data/sharedassets0.assets.split0")
        assert struct.unpack_from("<Iq", split0, 0x7B2A0) == ((0, 279) if build_ok else (0, 281))


def test_workspace_contains_streams_across_chunks(tmp_path):
    import zipfile

    from frameport.apk.workspace import ApkWorkspace

    apk = tmp_path / "a.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("big.assets", b"x" * 10 + b"HARD_OCCLUSION" + b"y" * 10)
        z.writestr("none.assets", b"z" * 40)
    with ApkWorkspace(apk) as ws:
        assert ws.contains("big.assets", (b"HARD_OCCLUSION",), chunk=8)  # the keyword spans chunk borders
        assert not ws.contains("none.assets", (b"HARD_OCCLUSION",), chunk=8)
        assert ws.size("big.assets") == 34
