"""frame.cortopia_org_id: skip Cortopia's Users.GetOrgScopedID request (OVRPort never answers it, GitHub #360)."""
from pathlib import Path

import pytest

from frameport.analysis.il2cpp import parse_methods
from frameport.patches.base import get, load_all
from frameport.patches.frame import cortopia_org_id as C
from frameport.patches.frame.unity_text_input import ALL_TARGETS, _executable


def _lib() -> bytes:
    fx = sorted(Path(__file__).parent.joinpath("fixtures").glob("libfake*_arm64.so"))[0]
    return fx.read_bytes()


def _bl(at: int, target: int) -> int:
    return 0x94000000 | (((target - at) >> 2) & 0x3FFFFFF)


def _bne(at: int, target: int) -> int:
    return 0x54000001 | ((((target - at) >> 2) & 0x7FFFF) << 5)


def _fake(lib: bytes, org_inside: bool = True):
    """The shape of Escaping Wonderland 1.0.1.1354's MoveNext: bl reach; cmp w0,#2; b.ne skip; ...;
    bl GetOrgScopedID; ...; skip:"""
    (lo, hi), = _executable(lib)[:1]
    m = (lo + 0x40) & ~3
    reach, org = m + 0x60, m + 0x70  # call targets only (outside the 64-byte method)
    assert m + 64 <= hi
    words = [0xD503201F] * 16                         # nop
    words[2] = _bl(m + 8, reach)
    words[3] = C.CMP_W0_2
    words[4] = _bne(m + 16, m + 48)                   # skip to word 12
    words[7] = _bl(m + 28, org if org_inside else reach + 8)
    out = bytearray(lib)
    for i, w in enumerate(words):
        out[m + 4 * i:m + 4 * i + 4] = w.to_bytes(4, "little")
    return bytes(out), (m, 64), reach, org


def test_branch_becomes_unconditional_once():
    lib, method, reach, org = _fake(_lib())
    patched, notes = C.skip_org_id(lib, method, reach, org)
    br = method[0] + 16
    w = int.from_bytes(patched[br:br + 4], "little")
    assert w == 0x14000000 | 8 and "skips Users.GetOrgScopedID" in notes[0]  # b +32 (to word 12)
    assert patched[:br] == lib[:br] and patched[br + 4:] == lib[br + 4:]
    again, notes = C.skip_org_id(patched, method, reach, org)
    assert again is None and "already" in notes[0]


def test_refuses_when_the_skipped_code_has_no_org_scoped_id_call():
    lib, method, reach, org = _fake(_lib(), org_inside=False)
    with pytest.raises(RuntimeError, match="wasn't found"):
        C.skip_org_id(lib, method, reach, org)


def test_cpp2il_names_are_found_and_requested():
    cs = '''	private struct <Initialize>d__13 : IAsyncStateMachine
	{
		[Address(RVA = "0x269F428", Offset = "0x269F428", Length = "0xEE4")]
		[Token(Token = "0x6000952")]
		private override void MoveNext() { }
'''
    assert parse_methods(cs, ["MoveNext"]) == {"MoveNext": (0x269F428, 0xEE4)}
    for cls, names in C.WANTED.items():
        assert set(names) <= set(ALL_TARGETS[cls])


def test_suggested_only_for_cortopia_platform_code():
    load_all()
    p = get("frame.cortopia_org_id")

    class A:
        engine, libs, abis = "Unity", ["libil2cpp.so"], ["arm64-v8a"]
        extra = {"cortopia_org_id": True, "unity_version": "2022.3.19f1"}

    assert p.applies(A) and p.detect(A).recommended
    A.extra = {"cortopia_org_id": False}
    assert not p.applies(A) and p.detect(A) is None


def test_login_state_machine_is_found_by_nested_type():
    """TMNT: Empire City (GitHub #422): the platform class has another async method first (FetchEntitlements), so a
    plain "MoveNext" is the wrong one; "<Login>d__.MoveNext" is Login's state machine, whatever its number."""
    cs = '''	private struct <FetchEntitlements>d__18 : IAsyncStateMachine
	{
		[Address(RVA = "0x43C9BC8", Offset = "0x43C9BC8", Length = "0xA3C")]
		[Token(Token = "0x6000EB2")]
		private override void MoveNext() { }

	}

	[CompilerGenerated]
	[Token(Token = "0x200026C")]
	private struct <Login>d__17 : IAsyncStateMachine
	{
		[FieldOffset(Offset = "0x0")]
		public int <>1__state; //Field offset: 0x0

		[Address(RVA = "0x43CAC38", Offset = "0x43CAC38", Length = "0xE8C")]
		[Token(Token = "0x6000EB6")]
		private override void MoveNext() { }

		[Address(RVA = "0x43CBE78", Offset = "0x43CBE78", Length = "0x58")]
		private override void SetStateMachine(IAsyncStateMachine stateMachine) { }

	}

	[Address(RVA = "0x43C915C", Offset = "0x43C915C", Length = "0xF0")]
	public virtual UniTask<Int64> Login() { }
'''
    assert parse_methods(cs, [C.LOGIN, "MoveNext", "Login", "<Initialize>d__.MoveNext"]) == {
        C.LOGIN: (0x43CAC38, 0xE8C), "MoveNext": (0x43C9BC8, 0xA3C), "Login": (0x43C915C, 0xF0)}
    assert C.START_UP == [(C.PLATFORM, "MoveNext"), (C.PLATFORM_LOGIN, C.LOGIN)]


def test_triage_finds_the_login_wait():
    from frameport.validate.triage import triage

    log = """E Unity   : An unknown error occurred. Request failed.
E Unity   : UnityEngine.DebugLogHandler:Internal_Log(LogType, LogOption, String, Object)
E Unity   : Oculus.Platform.Request`1:OnComplete(Callback) (at Scripts/Request.cs:31)
E Unity   : Cortopia.Scripts.Platform.PlatformExtensions:ToUniTask(Request`1, String) (at PlatformExtensions.cs:29)
E Unity   : Cortopia.Scripts.Platform.<Login>d__17:MoveNext() (at Assets/Cortopia/Scripts/Platform/OculusPlatform.cs:70)
"""
    ids = {f.id for f in triage(log, "RUNNING").findings}
    assert "cortopia-org-id-wait" in ids


def test_analysis_flag_with_and_without_the_meta_namespace():
    from frameport.analysis.detect import ANALYSIS_VERSION, cortopia_org_id

    org = b"\0GetOrgScopedID\0"
    assert cortopia_org_id(b"\0Cortopia.Scripts.Platform.Meta\0" + org)            # Escaping Wonderland
    assert cortopia_org_id(b"\0Cortopia.Scripts.Platform\0OculusPlatform\0" + org)  # TMNT: Empire City
    assert not cortopia_org_id(b"\0Cortopia.Scripts.Platform\0OculusPlatform\0")    # no org-scoped id request
    assert not cortopia_org_id(b"\0Cortopia.Scripts.Platform\0PicoPlatform\0" + org)
    assert not cortopia_org_id(None) and ANALYSIS_VERSION >= 26
