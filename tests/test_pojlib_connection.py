"""QuestCraft skipped its Java runtime download on the Frame: pojlib only counts Wi-Fi (GitHub #18, #468)."""
from __future__ import annotations

import struct
import zipfile

from test_vivox_audio_route import build_dex

from frameport.analysis import detect
from frameport.apk.dex import Dex
from frameport.patches.frame.pojlib_connection import API, PojlibConnection, patch_dex
from frameport.validate.triage import triage

CHECK = [0x0071, 0x0001, 0x0000, 0x000A, 0x000F]  # invoke-static {} …; move-result v0; return v0
OTHER = [0x0012, 0x000F]


def _code(dex: Dex, name: str) -> list[int]:
    off = dex.code_items(API, name)[0]
    n = struct.unpack_from("<I", dex.data, off + 12)[0]
    return list(struct.unpack_from(f"<{n}H", dex.data, off + 16))


def test_has_connection_returns_true():
    raw = build_dex([(API, "hasConnection", "Z", CHECK), (API, "hasExtraProject", "Z", OTHER)])
    dex = Dex(raw)
    assert patch_dex(dex) == ["hasConnection"]
    assert _code(dex, "hasConnection")[:2] == [0x1012, 0x000F]  # const/4 v0, 1; return v0
    assert _code(dex, "hasExtraProject") == OTHER
    out = dex.finish()
    assert len(out) == len(raw) and patch_dex(Dex(out)) == []


def test_detect_and_triage(tmp_path, quest_manifest):
    path = tmp_path / "game.apk"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("classes.dex", b"dex\n035\0" + build_dex([(API, "hasConnection", "Z", CHECK)]))
    a = detect.analyze(path, deep=False)
    assert a.extra["pojlib_connection"] is True
    assert PojlibConnection().applies(a) and PojlibConnection().detect(a).recommended
    log = "10-22 12:20:01.000  1200  1250 E pojlib  : dlopen failed: library \"libjvm.so\" not found\n"
    r = triage(log, "RUNNING", "com.qcxr.qcxr")
    assert [f.suggest for f in r.findings if f.id == "questcraft-no-java-runtime"] == [["frame.pojlib_connection"]]
