"""Just Dance VR (GitHub #480): frame.justdance_local_save gives LocalSaveEvents.RequestSave its body (raise
SaveValueRequested) in a new code segment, with every address found by name (Cpp2IL) and by structure (capstone)."""
import hashlib
import os
import struct
import zipfile
from pathlib import Path
from types import SimpleNamespace

import capstone
import pytest

from frameport.analysis import elf
from frameport.core.models import Analysis
from frameport.patches import base
from frameport.patches.frame import justdance_save as jd
from frameport.validate.triage import triage

NOP = 0xD503201F
TEXT_OFF, TEXT_VA, TEXT_SIZE = 0x2000, 0x3000, 0x2000
SLOT = 0x1C48  # GOT slot with &LocalSaveEvents_TypeInfo_var (any readable address; the code only refers to it)
INIT = 0x4F00  # il2cpp's runtime-metadata init


def _template(va: int) -> list[int]:
    """SaveSystemReady as IL2CPP compiles it: SaveSystemReadyEvent?.Invoke() (static field 0, an Action)."""
    page, lo = SLOT & ~0xFFF, SLOT & 0xFFF
    w = [0xF81E0FFE, 0xA9014FF4, NOP]                       # str x30 ; stp x20, x19 ; (flag page)
    w += [jd._adrp(va + 12, page, 20), NOP, jd._ldr_x(20, 20, lo), NOP]  # adrp x20 ; (ldrb) ; ldr x20 ; (tbnz)
    w += [jd._adrp(va + 28, page, 0), jd._ldr_x(0, 0, lo), jd._b(va + 36, INIT, link=True)]
    w += [NOP, NOP]                                         # mov w8, #1 ; strb
    w += [jd._ldr_x(8, 20, 0), jd._ldr_x(8, 8, 0xB8), jd._ldr_x(8, 8, 0)]
    w += [jd._cbz_x(va + 4 * 15, va + 4 * 22, 8)]
    w += [jd._ldr_x(2, 8, 0x18), jd._ldr_x(0, 8, 0x40), jd._ldr_x(1, 8, 0x28)]
    w += [*jd.POP, jd._br(2), *jd.POP, jd.RET]
    return w


def _fake_lib(req_va=0x3100, tmpl_va=0x3200, req_words=(jd.RET,), note=True):
    data = bytearray(TEXT_OFF + TEXT_SIZE)
    phoff, phnum = 64, 3 if note else 2
    data[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", data, 16, 3, 183, 1, 0, phoff, 0, 0, 64, 56, phnum, 64, 0, 0)
    struct.pack_into("<IIQQQQQQ", data, phoff, 1, 4, 0, 0, 0, 0x2000, 0x2000, 0x1000)
    struct.pack_into("<IIQQQQQQ", data, phoff + 56, 1, 5, TEXT_OFF, TEXT_VA, TEXT_VA, TEXT_SIZE, TEXT_SIZE, 0x1000)
    if note:
        struct.pack_into("<IIQQQQQQ", data, phoff + 112, 4, 4, 0x200, 0x200, 0x200, 0x20, 0x20, 4)
    for i in range(TEXT_SIZE // 4):
        struct.pack_into("<I", data, TEXT_OFF + 4 * i, NOP)
    struct.pack_into(f"<{len(req_words)}I", data, req_va - 0x1000, *req_words)
    words = _template(tmpl_va)
    struct.pack_into(f"<{len(words)}I", data, tmpl_va - 0x1000, *words)
    found = {jd.EVENTS: {"RequestSave": (req_va - 0x1000, 4 * len(req_words)),
                         "SaveSystemReady": (tmpl_va - 0x1000, 4 * len(words)),
                         "field:SaveValueRequested": (0x28, 0), "field:SaveSystemReadyEvent": (0, 0)},
             jd.ARGS: {"field:Key": (0, 0), "field:Value": (8, 0)}}
    return bytes(data), found


def _dis(data: bytes, va: int, n: int) -> list[str]:
    off = elf.vaddr_to_offset(elf.load_segments(data), va)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    return [f"{i.mnemonic} {i.op_str}".strip() for i in md.disasm(data[off:off + 4 * n], va)]


def _expected(va: int) -> list[str]:
    return ["str x30, [sp, #-0x20]!", "stp x20, x19, [sp, #0x10]", "mov x19, x0", "mov x20, x1",
            "adrp x0, #0x1000", "ldr x0, [x0, #0xc48]", f"bl #{INIT:#x}",
            "adrp x8, #0x1000", "ldr x8, [x8, #0xc48]", "ldr x8, [x8]", "ldr x8, [x8, #0xb8]",
            "ldr x8, [x8, #0x28]", f"cbz x8, #{va + 21 * 4:#x}",
            "ldr x4, [x8, #0x18]", "ldr x0, [x8, #0x40]", "ldr x3, [x8, #0x28]", "mov x1, x19", "mov x2, x20",
            "ldp x20, x19, [sp, #0x10]", "ldr x30, [sp], #0x20", "br x4",
            "ldp x20, x19, [sp, #0x10]", "ldr x30, [sp], #0x20", "ret"]


def test_template_is_decoded():
    lib, found = _fake_lib()
    t = jd.inspect(lib, found)["template"]
    assert t == {"slot": SLOT, "init": INIT, "statics": 0xB8, "impl": 0x18, "target": 0x40, "method": 0x28}


@pytest.mark.parametrize("note", [True, False])
def test_request_save_gets_the_event_call(note):
    lib, found = _fake_lib(note=note)
    new, info = jd.patch_library(lib, found)
    va = info["body_va"]
    assert info["state"] == "needed" and va >= TEXT_VA + TEXT_SIZE and va % 4 == 0
    changed = [i for i, (a, b) in enumerate(zip(lib, new[:len(lib)], strict=True)) if a != b]
    assert all(32 <= i < 40 or 56 <= i < 58 or 64 <= i < 64 + 56 * 3 or 0x2100 <= i < 0x2104 for i in changed)
    assert _dis(new, 0x3100, 1) == [f"b #{va:#x}"]
    assert _dis(new, va, 24) == _expected(va)
    seg = [s for s in elf._elf(new).iter_segments() if s["p_type"] == "PT_LOAD"][-1]
    assert seg["p_vaddr"] <= va and seg["p_flags"] == 5  # R+X
    assert jd.inspect(new, found)["state"] == "done"
    assert jd.patch_library(new, found)[0] is None  # recognised, not patched twice


def test_shifted_layout_is_still_found():
    lib, found = _fake_lib(req_va=0x4400, tmpl_va=0x3800)
    new, info = jd.patch_library(lib, found)
    assert _dis(new, 0x4400, 1) == [f"b #{info['body_va']:#x}"] and _dis(new, info["body_va"], 24) == \
        _expected(info["body_va"])


def test_request_save_with_a_body_is_left_alone():
    """A later build that writes its save itself: nothing to do."""
    lib, found = _fake_lib(req_words=(0xF81E0FFE, NOP, NOP, jd.RET))
    new, info = jd.patch_library(lib, found)
    assert new is None and info["state"] == "saves"


@pytest.mark.parametrize("change", ["missing", "struct", "template", "word"])
def test_anything_unexpected_is_refused(change):
    lib, found = _fake_lib(req_words=(0x12345678,) if change == "word" else (jd.RET,))
    if change == "missing":
        del found[jd.EVENTS]["SaveSystemReady"]
    elif change == "struct":
        found[jd.ARGS]["field:Key"] = (0x10, 0)  # a class, not the 16-byte struct
    elif change == "template":
        lib = bytearray(lib)
        struct.pack_into("<I", lib, 0x3200 - 0x1000 + 4 * 21, jd.RET)  # no delegate call
        lib = bytes(lib)
    with pytest.raises(jd.Refused):
        jd.patch_library(lib, found)


class _Ws:
    abi = "arm64-v8a"

    def __init__(self, data):
        self.files = {f"lib/arm64-v8a/{jd.LIB}": data, jd.METADATA: b"meta"}

    def lib(self, name):
        return f"lib/arm64-v8a/{name}"

    def has(self, path):
        return path in self.files

    def read(self, path):
        return self.files[path]

    def put(self, path, data):
        self.files[path] = data


def _ctx(lib):
    log = []
    return SimpleNamespace(ws=_Ws(lib), notes=[], checks=[],
                           analysis=SimpleNamespace(extra={"unity_version": "2022.3.15f1"}),
                           reporter=SimpleNamespace(log=log.append, check=lambda *a: log.append(a)))


def test_patch_apply(monkeypatch):
    lib, found = _fake_lib()
    monkeypatch.setattr("frameport.analysis.il2cpp.find_methods", lambda *a, **k: found)
    p = base.get("frame.justdance_local_save")
    ctx = _ctx(lib)
    assert p.apply(ctx) and "RequestSave" in ctx.notes[0]
    assert ctx.checks == [("Just Dance VR local save", True, "RequestSave raises SaveValueRequested")]
    assert not p.apply(ctx)  # already patched
    saves, found2 = _fake_lib(req_words=(0xF81E0FFE, NOP, NOP, jd.RET))
    monkeypatch.setattr("frameport.analysis.il2cpp.find_methods", lambda *a, **k: found2)
    ctx = _ctx(saves)
    assert not p.apply(ctx) and "not needed" in ctx.checks[0][2]
    del found2[jd.ARGS]
    ctx = _ctx(saves)
    assert not p.apply(ctx) and ctx.checks[0][1] is None and "not found" in ctx.checks[0][2]


def _analysis(**kw):
    d = dict(package=jd.PACKAGE, version="0.5.4", label="Just Dance VR", abis=["arm64-v8a"], engine="Unity",
             xr="OpenXR", graphics="Vulkan", direct_vrapi=False, libs=["libil2cpp.so", "libunity.so"],
             launcher_activity="com.unity3d.player.UnityPlayerActivity", has_info_category=False, meta_permissions=[],
             uses_glad_gl=False, unity_msaa_levels=0, oculus_os_classes=False, is_overport_output=False,
             debuggable=False, extra={"unity_version": "2022.3.15f1"})
    d.update(kw)
    return Analysis(**d)


@pytest.mark.parametrize("version,suggested", [("0.5.4", True), ("0.5.10", True), ("0.6", True), ("1.2.0", True),
                                               ("0.5.3", False), ("0.4.9", False), ("", False)])
def test_detect_from_the_lower_bound(version, suggested):
    p = base.get("frame.justdance_local_save")
    assert bool(p.detect(_analysis(version=version))) is suggested
    assert not p.applies(_analysis(package="com.other.game"))


def test_version_tuple():
    assert jd.version_tuple("0.5.10") > jd.version_tuple("0.5.4") > jd.version_tuple("0.5")
    assert jd.version_tuple("1.0") == (1, 0, 0) and jd.version_tuple("0.6.1-b2") == (0, 6, 1)


def test_append_segment_keeps_everything_in_place():
    lib, _ = _fake_lib()
    new, va = elf.append_segment(lib, b"\x01\x02\x03\x04")
    off = elf.vaddr_to_offset(elf.load_segments(new), va)
    assert new[off:off + 4] == b"\x01\x02\x03\x04"
    assert new[TEXT_OFF:TEXT_OFF + TEXT_SIZE] == lib[TEXT_OFF:TEXT_OFF + TEXT_SIZE]
    vaddrs = [s["p_vaddr"] for s in elf._elf(new).iter_segments() if s["p_type"] == "PT_LOAD"]
    assert vaddrs == sorted(vaddrs) and va % 16384 == 0


def test_triage_names_the_patch(monkeypatch):
    log = ("10-24 23:55:37.333  1217  1243 I Unity   : 2026/10/24 - 23h 55m 37s 332|INF |LocalSave      "
           "|OfflineSave_Init succeeded\n")
    found = {f.id: f for f in triage(log, package=jd.PACKAGE, applied=set()).findings}
    assert found["justdance-local-save-off"].suggest == ["frame.justdance_local_save"]
    # the same line from a build with the fix: no finding (the build's applied patches, from the library by default)
    built = {"frame.adapter", "frame.justdance_local_save"}
    assert "justdance-local-save-off" not in {f.id for f in triage(log, package=jd.PACKAGE, applied=built).findings}
    monkeypatch.setattr("frameport.core.library.game", lambda pkg: {"build": {"applied": sorted(built)}})
    assert "justdance-local-save-off" not in {f.id for f in triage(log, package=jd.PACKAGE).findings}
    monkeypatch.setattr("frameport.core.library.game", lambda pkg: {"build": {"applied": ["frame.adapter"]}})
    assert "justdance-local-save-off" in {f.id for f in triage(log, package=jd.PACKAGE).findings}


@pytest.mark.games
def test_real_library():
    """0.5.4 through the real Cpp2IL run: the template decodes to the values found by hand (GitHub #480)."""
    from frameport.analysis.il2cpp import find_methods

    root = os.environ.get("FRAMEPORT_GAMES")
    if not root:
        pytest.skip("set FRAMEPORT_GAMES")
    apks = sorted(Path(root).glob("**/com.ubisoft.justdancevr.apk"))
    if not apks:
        pytest.skip("no Just Dance VR APK in FRAMEPORT_GAMES")
    with zipfile.ZipFile(apks[0]) as z:
        lib, meta = z.read("lib/arm64-v8a/libil2cpp.so"), z.read(jd.METADATA)
    if hashlib.sha256(lib).hexdigest() != "ff91e080194a6a64e2125d354f3c4a501bbe35602f0fe09aca9183c1fde75116":
        pytest.skip("another Just Dance VR build")
    found = find_methods(lib, meta, "2022.3.15f1", jd.WANTED)
    assert found[jd.EVENTS]["RequestSave"] == (0x57F99E8, 4)
    new, info = jd.patch_library(lib, found)
    assert info["template"] == {"slot": 0x7347C48, "init": 0x3192E20, "statics": 0xB8, "impl": 0x18,
                                "target": 0x40, "method": 0x28}
    assert jd.inspect(new, found)["state"] == "done" and new[:len(lib)] != lib
