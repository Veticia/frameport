"""Static compatibility survey of Rift games for the experimental Rift-on-Frame mode (x86_64 GE-Proton under FEX +
Meta's runtime + Revive, see native/fexwine/README.md). Read-only: looks at the game folders' PE files and the Unreal
config inside paks; never runs anything, never touches a repack's own Oculus Platform files (only notes them).

Engine, VR APIs and the Platform SDK come from FramePort's own Rift analysis (analysis/rift.py); on top of that:
the Unreal version (branch string, else the pak format's range), the RHI the game's config selects, the generic
Unreal IME fix site (agent/fexrift.py find_ime_patch), anti-tamper sections and other runtime needs.

usage: uv run python scripts/rift_fex_survey.py [--folder DIR ...] [--out DIR]
  default input: the library's rift.* games; output: <user data>/fexrift/survey.{md,json} (holds local paths and folder
  names: keep it out of the repo)
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from frameport.analysis import rift  # noqa: E402
from frameport.core import library  # noqa: E402
from frameport.core.paths import user_data_dir  # noqa: E402


def load_fexrift():
    spec = importlib.util.spec_from_file_location("fexrift_helper", ROOT / "agent" / "fexrift.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.load_extensions()
    return mod


FX = load_fexrift()
MAX_PE = 400 << 20

DRM_SECTIONS = {".arch": "Denuvo/Arxan-style section", ".vmp0": "VMProtect", ".vmp1": "VMProtect", ".bind": "Steam DRM",
                ".themida": "Themida", ".enigma1": "Enigma"}
ANTICHEAT_FILES = ("easyanticheat", "battleye", "beclient", "eac_launcher")

# ------------------------------------------------------------------------------------------ Unreal paks
PAK_MAGIC = 0x5A6F12E1
# pak format version -> the Unreal versions that write it (when the exe doesn't name its version)
PAK_UE = {1: "4.0-4.2", 2: "4.0-4.2", 3: "4.3-4.15", 4: "4.16-4.19", 5: "4.20", 6: "4.20", 7: "4.21",
          8: "4.22-4.24", 9: "4.25", 10: "4.26", 11: "4.26-4.27"}


class Reader:
    def __init__(self, buf: bytes, pos: int = 0):
        self.buf, self.p = buf, pos

    def take(self, fmt: str):
        v = struct.unpack_from(fmt, self.buf, self.p)
        self.p += struct.calcsize(fmt)
        return v if len(v) > 1 else v[0]

    def fstring(self) -> str:
        n = self.take("<i")
        if n < 0:
            s = self.buf[self.p:self.p - 2 * n].decode("utf-16-le", "replace")
            self.p += -2 * n
        else:
            s = self.buf[self.p:self.p + n].decode("latin-1")
            self.p += n
        return s.rstrip("\0")


def entry_header_size(ver: int, compressed: bool, nblocks: int) -> int:
    """Size of the FPakEntry copy in front of a file's data (offset, sizes, method, hash, blocks, flags, block size)."""
    return 8 + 8 + 8 + 4 + 20 + (4 + 16 * nblocks if compressed else 0) + (1 + 4 if ver >= 3 else 0)


def legacy_entry(r: Reader, ver: int):
    """A full FPakEntry: (offset, compressed size, size, method, blocks, encrypted)."""
    off, csize, usize = r.take("<qqq")
    method = r.take("<i")
    r.p += 20
    blocks, enc = [], 0
    if ver >= 3:
        if method:
            nb = r.take("<i")
            blocks = [r.take("<qq") for _ in range(nb)]
        enc = r.buf[r.p]
        r.p += 1 + 4
    return off, csize, usize, method, blocks, enc


def encoded_entry(buf: bytes, pos: int, ver: int):
    """A v10+ bit-packed entry (FPakFile::DecodePakEntry); blocks relative to the entry like v7+ full entries."""
    r = Reader(buf, pos)
    v = r.take("<I")
    if v & 0x3f == 0x3f:
        r.take("<I")                               # compression block size (not needed to read)
    method = (v >> 23) & 0x3f
    off = r.take("<I" if v & (1 << 31) else "<Q")
    usize = r.take("<I" if v & (1 << 30) else "<Q")
    csize = r.take("<I" if v & (1 << 29) else "<Q") if method else usize
    enc = bool(v & (1 << 22))
    nb = (v >> 6) & 0xffff
    base = entry_header_size(ver, bool(method), nb)
    blocks = []
    if nb == 1 and not enc:
        blocks = [(base, base + csize)]
    elif nb:
        o = base
        for _ in range(nb):
            s = r.take("<I")
            blocks.append((o, o + s))
            o += (s + 15) & ~15 if enc else s
    return off, csize, usize, method, blocks, enc


def pak_entries(path: Path):
    """(version, mount point, {name: entry}, compression names) of an unencrypted UE4 pak, v1-v11; ValueError if the
    index can't be read (encrypted, unknown footer)."""
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - 512))
        tail = f.read()
        i = tail.rfind(struct.pack("<I", PAK_MAGIC), 0, len(tail) - 40)
        while i >= 0 and len(tail) - i not in (44, 44 + 4 * 32, 44 + 5 * 32):   # magic..end, 0/4/5 method names
            i = tail.rfind(struct.pack("<I", PAK_MAGIC), 0, i)
        if i < 0:
            raise ValueError("no pak footer")
        _, ver, ioff, isize = struct.unpack_from("<IIqq", tail, i)
        if ver >= 4 and i >= 1 and tail[i - 1] == 1:
            raise ValueError(f"pak v{ver}: index encrypted")
        names = [tail[j:j + 32].split(b"\0")[0].decode("latin-1") for j in range(i + 44, len(tail) - 31, 32)]
        if not 0 <= ioff < size or not 0 < isize <= size - ioff:
            raise ValueError(f"pak v{ver}: index outside the file")
        f.seek(ioff)
        r = Reader(f.read(isize))
        mount = r.fstring()
        count = r.take("<i")
        entries = {}
        if ver < 10:
            for _ in range(count):
                name = r.fstring()
                entries[name] = legacy_entry(r, ver)
            return ver, mount, entries, names
        r.take("<Q")                                   # path hash seed
        if r.take("<i"):
            r.take("<qq")
            r.p += 20                                  # path hash index: hashes only, no names
        if not r.take("<i"):
            raise ValueError(f"pak v{ver}: no full directory index (names hashed only)")
        fdi_off, fdi_size = r.take("<qq")
        r.p += 20
        enc_size = r.take("<i")
        encoded = r.buf[r.p:r.p + enc_size]
        r.p += enc_size
        files = [legacy_entry(r, ver) for _ in range(r.take("<i"))]
        f.seek(fdi_off)
        d = Reader(f.read(fdi_size))
    for _ in range(d.take("<i")):
        folder = d.fstring()
        for _ in range(d.take("<i")):
            name, loc = d.fstring(), d.take("<i")
            entries[(folder + name).lstrip("/")] = encoded_entry(encoded, loc, ver) if loc >= 0 else files[-loc - 1]
    return ver, mount, entries, names


def pak_read(path: Path, ver: int, entry, names: list[str] | None = None) -> bytes:
    off, csize, usize, method, blocks, enc = entry
    if enc:
        raise ValueError("entry encrypted")
    with open(path, "rb") as f:
        if not method:
            f.seek(off + entry_header_size(ver, False, 0))
            return f.read(usize)
        kind = (names[method - 1] if ver >= 8 and names and 0 < method <= len(names) else "zlib").lower()
        if kind not in ("zlib", "gzip"):
            raise ValueError(f"{kind}-compressed")
        out = b""
        for s, e in blocks:
            if ver >= 7:
                s, e = s + off, e + off
            f.seek(s)
            out += zlib.decompress(f.read(e - s), 31 if kind == "gzip" else 15)
        return out


CONFIG_KEYS = ("r.MobileMSAA", "r.MSAACount", "r.DefaultFeature.AntiAliasing", "r.ForwardShading",
               "r.useclusteredforward", "vr.InstancedStereo", "r.MobileHDR", "DefaultGraphicsRHI", "TargetedRHIs",
               "bUseVulkan")


def parse_engine_ini(text: str) -> dict:
    """The keys survey cares about; repeated keys (+TargetedRHIs=…) become lists."""
    want = {k.lower(): k for k in CONFIG_KEYS}
    vals: dict = {}
    for line in text.splitlines():
        k, _, v = line.strip().partition("=")
        key = want.get(k.lstrip("+-.!").lower())
        if not key:
            continue
        if key == "TargetedRHIs":
            vals.setdefault(key, [])
            if k.startswith("-"):
                vals[key] = [x for x in vals[key] if x != v]
            elif v not in vals[key]:
                vals[key].append(v)
        else:
            vals[key] = v
    return vals


def unreal_config(game_dir: Path) -> dict:
    """Pak format + rendering/AA settings from the project's DefaultEngine.ini (paks, else a loose file)."""
    paks = sorted(game_dir.rglob("*.pak"), key=lambda p: -p.stat().st_size)
    notes, ver_seen = [], None
    for pak in paks[:6]:
        try:
            ver, _, entries, names = pak_entries(pak)
        except (ValueError, OSError, struct.error, IndexError) as e:
            notes.append(f"{pak.name}: {e}")
            continue
        ver_seen = ver_seen or ver
        name = next((n for n in entries if n.endswith("Config/DefaultEngine.ini")), None)
        if not name:
            continue
        try:
            text = pak_read(pak, ver, entries[name], names).decode("utf-8-sig", "replace")
        except (ValueError, zlib.error, OSError) as e:
            notes.append(f"{pak.name}: DefaultEngine.ini {e}")
            continue
        return {"pak": f"v{ver}", "config": parse_engine_ini(text), "pak_notes": notes}
    loose = next((p for p in game_dir.rglob("DefaultEngine.ini") if p.parent.name == "Config"), None)
    if loose:
        text = loose.read_text("utf-8", "replace")
        return {"pak": f"v{ver_seen}" if ver_seen else "", "config": parse_engine_ini(text),
                "pak_notes": notes + ["config from a loose DefaultEngine.ini"]}
    return {"pak": f"v{ver_seen}" if ver_seen else "unreadable", "pak_notes": notes or ["no DefaultEngine.ini found"]}


RHI_NAMES = {"DefaultGraphicsRHI_DX11": "d3d11", "DefaultGraphicsRHI_DX12": "d3d12",
             "DefaultGraphicsRHI_Vulkan": "vulkan"}


def unreal_rhi(cfg: dict) -> tuple[str, str]:
    """(RHI the Windows build starts with, where that came from). Unreal 4 defaults to D3D11."""
    if cfg.get("DefaultGraphicsRHI") in RHI_NAMES:
        return RHI_NAMES[cfg["DefaultGraphicsRHI"]], "config"
    rhis = cfg.get("TargetedRHIs") or []
    if rhis and not any(r.startswith("PCD3D") for r in rhis):
        if any("VULKAN" in r for r in rhis):
            return "vulkan", "config (no D3D shaders)"
        if any(r.startswith("GLSL") for r in rhis):
            return "opengl", "config (no D3D shaders)"
    if rhis and all(r == "PCD3D_SM6" for r in rhis if r.startswith("PCD3D")):
        return "d3d12", "config (SM6 shaders only)"
    return "d3d11", "config" if rhis else "Unreal default"


# ------------------------------------------------------------------------------------------ PE checks
def pe_files(game_dir: Path, limit: int = 600):
    out = []
    for p in game_dir.rglob("*"):
        if p.suffix.lower() in (".exe", ".dll") and p.is_file():
            out.append(p)
            if len(out) >= limit:
                break
    return out


_BRANCH = re.compile(r"\+\+[A-Za-z0-9_+.\-]{3,60}")
_VER = re.compile(r"(?<![\d.])([45])\.(\d{1,2})(?![\d])")


def unreal_version(data: bytes) -> str | None:
    """"4.13" etc. from Unreal's branch name (++UE4+Release-4.13, ++unreal+4.10+UE4-Studio, ++UE4+Studio-4.27; ASCII
    or UTF-16). Branches without a number (++depot+Game) give None."""
    texts = [m.group(0).decode() for m in re.finditer(_BRANCH.pattern.encode(), data)]
    key = "++".encode("utf-16-le")
    i = data.find(key)
    while i >= 0 and len(texts) < 2000:
        m = _BRANCH.match(data[i:i + 128].decode("utf-16-le", "replace"))
        if m:
            texts.append(m.group(0))
        i = data.find(key, i + 4)
    for t in texts:
        letters = sum(c.isalpha() for c in t)
        if letters < 4 or not re.search(r"ue[45]|unreal|release", t, re.I):
            continue
        m = _VER.search(t)
        if m:
            return f"{m.group(1)}.{m.group(2)}"
    return None


def sections(data: bytes) -> list[str]:
    try:
        return [s[0] for s in FX.pe_sections(data)]
    except struct.error:
        return []


def imports_of(p: Path) -> set[str]:
    try:
        info = rift.read_pe(p, limit=80 << 20)[0]
        return set(info.imports) | set(info.delay_imports)
    except (rift.PEError, OSError, struct.error, ValueError):
        return set()


GRAPHICS = {"d3d11.dll": "d3d11", "d3d12.dll": "d3d12", "opengl32.dll": "opengl", "vulkan-1.dll": "vulkan",
            "d3d9.dll": "d3d9"}


def survey_game(title: str, game_dir: Path, exe_rel: str | None) -> dict:
    res = {"title": title, "folder": str(game_dir)}
    if not game_dir.is_dir():
        res["verdict"] = ("missing", "folder not found")
        return res
    exe = game_dir / exe_rel if exe_rel else None
    if not exe or not exe.is_file():
        res["verdict"] = ("unknown", "main exe not known")
        return res
    a = rift.analyze(game_dir, exe_rel)
    files = pe_files(game_dir)
    names = {p.name.lower(): p for p in files}
    info, data = rift.read_pe(exe, limit=MAX_PE)
    data = bytes(data)
    res["exe"] = exe_rel
    res["arch"] = info.machine
    res["exe_32bit"] = info.machine == "x86"
    res["exe_size"] = exe.stat().st_size
    res["vr"] = [x for x in a.xr.split("+") if x != "?"] or ["?"]
    res["platform_sdk"] = bool(a.extra.get("platform_sdk"))
    res["own_platform_dll"] = any(n.startswith("libovrplatform") for n in names)   # noted only, never used
    imports = set(info.imports) | set(info.delay_imports)
    # engine
    if a.engine == "Unreal":
        cores = [p for n, p in names.items() if re.search(r"-core(-win64-shipping)?\.dll$", n)]
        ue = unreal_version(data) or next((v for v in map(lambda p: unreal_version(p.read_bytes()), cores) if v), None)
        cfg = unreal_config(game_dir)
        res.update(cfg)
        pak_v = int(cfg["pak"][1:]) if re.fullmatch(r"v\d+", cfg.get("pak", "")) else None
        res["ue_version"] = ue or (PAK_UE.get(pak_v) and f"{PAK_UE[pak_v]} (pak v{pak_v})") or "?"
        res["engine"] = f"Unreal {res['ue_version']}"
        rhi, src = unreal_rhi(cfg.get("config") or {})
        res["graphics"] = [rhi]
        res["graphics_from"] = src
        # Unreal IME deadlock fix: exe (monolithic), else modular ApplicationCore/Slate (Core before that split).
        patch, why = FX.find_ime_patch(data)
        if not patch:
            for n, p in sorted(names.items()):
                if "applicationcore" in n or re.search(r"-(slate|core)(-win64-shipping)?\.dll$", n):
                    p2, why2 = FX.find_ime_patch(p.read_bytes())
                    if p2:
                        patch, why = dict(p2, module=n), None
                        break
                    if not why2.startswith("no Slate"):
                        why = why2
        res["ime_patch"] = {"rva": hex(patch["rva"]), "module": patch.get("module", "exe")} if patch else why
        lo = (res["ue_version"].split(" ")[0].split("-")[0]).split(".")
        try:
            major_minor = (int(lo[0]), int(lo[1]))
            res["large_block_alloc"] = "FMallocBinned2 (UE>=4.18): fp_mem candidate" if major_minor >= (4, 18) \
                else "FMallocBinned (older): fp_mem likely unneeded"
        except (ValueError, IndexError):
            res["large_block_alloc"] = "?"
    else:
        for p in files:   # graphics imports of the game's own DLLs (Unity players, custom engines)
            if p.parent == exe.parent and p.stat().st_size <= 80 << 20:
                imports |= imports_of(p)
        res["graphics"] = sorted({v for k, v in GRAPHICS.items() if k in imports}) or ["?"]
        res["graphics_from"] = "imports"
        if a.engine == "Unity":
            src = names.get("unityplayer.dll")
            blob = rift.read_pe(src, limit=MAX_PE)[1] if src else data
            uv = re.search(rb"(20\d\d\.\d+\.\d+[fp]\d+|5\.\d\.\d+[fp]\d+)", blob)
            res["engine"] = f"Unity {uv.group(1).decode() if uv else '?'} " + \
                ("IL2CPP" if "gameassembly.dll" in names else "Mono")
        elif any(n.startswith("crysystem") for n in names) or b"CryEngine" in data or b"CRYENGINE" in data:
            res["engine"] = "CryEngine"
        else:
            res["engine"] = "custom"
    # anti-tamper / anti-cheat
    drm = sorted({DRM_SECTIONS[s] for s in sections(data) if s in DRM_SECTIONS})
    if any(any(x in n for x in ANTICHEAT_FILES) for n in names):
        drm.append("anti-cheat files")
    res["drm"] = drm
    # other runtime needs
    # Imports in modular engine/audio/video DLLs matter too. Reuse this for each candidate exe below; companion
    # middleware is a folder-level hint, not proof that a particular movie or audio route will be exercised.
    runtime_imports = set(imports)
    for p in files:
        if p.suffix.lower() == ".dll" and p.parent == exe.parent and p.stat().st_size <= 80 << 20:
            runtime_imports |= imports_of(p)
    res["runtime"] = runtime_facts(runtime_imports, names)
    res["executables"] = []
    for candidate in a.extra.get("exe_candidates", []):
        rel = candidate["path"]
        p = game_dir / rel
        try:
            pi = info if p == exe else rift.read_pe(p, limit=80 << 20)[0]
            facts = runtime_facts(set(pi.imports) | set(pi.delay_imports), names)
            g = sorted({GRAPHICS[i] for i in set(pi.imports) | set(pi.delay_imports) if i in GRAPHICS})
            res["executables"].append(dict(exe=rel, arch=pi.machine, exe_32bit=pi.machine == "x86",
                                           graphics_imports=g, **facts))
        except (OSError, rift.PEError, struct.error, ValueError):
            res["executables"].append({"exe": rel, "error": "PE unreadable"})
    other = []
    if "mscoree.dll" in imports:
        other.append(".NET")
    if res["runtime"]["xaudio2"]:
        other.append("XAudio2")
    if res["runtime"]["media_foundation"]:
        other.append("Media Foundation: video playback is a Proton compatibility risk")
    if res["runtime"]["bink"]:
        other.append("Bink video")
    winrt_re = rb"W\x00i\x00n\x00d\x00o\x00w\x00s\x00\.\x00(?:[A-Z]\x00[a-zA-Z\x00\.]{4,80})"
    winrt = sorted({w.decode("utf-16-le") for w in re.findall(winrt_re, data)})[:5]
    if winrt:
        other.append("WinRT: " + ", ".join(winrt))
    res["other"] = other
    res["size_gib"] = round(sum(p.stat().st_size for p in game_dir.rglob("*") if p.is_file()) / 2**30, 1)
    res["verdict"] = verdict(res)
    res["suggested_graphics"] = graphics_route(res)
    res["profile"] = suggested_profile(res)
    return res


def runtime_facts(imports: set[str], names) -> dict:
    imports = {i.lower() for i in imports}
    names = {n.lower() for n in names}
    return {"xaudio2": any(i.startswith("xaudio2") for i in imports),
            "media_foundation": bool(imports & {"mf.dll", "mfplat.dll", "mfreadwrite.dll", "evr.dll"}),
            "bink": any(n.startswith("bink") for n in names | imports),
            "anti_cheat": any(any(s in n for s in ANTICHEAT_FILES) for n in names | imports)}


def graphics_route(r: dict) -> str:
    g = r.get("graphics", [])
    # Unreal's config already reduced its imports to the selected RHI. For custom multi-API engines prefer a
    # DXVK route over GL; imports prove availability, not that the game will select that renderer at runtime.
    return next((api for api in ("d3d11", "d3d12", "d3d9", "opengl", "vulkan") if api in g), "d3d11")


def suggested_profile(r: dict) -> dict:
    status = {"likely OK": "likely", "blocked": "blocked"}.get(r["verdict"][0], "untested")
    profile = {"title": r["title"],
            "match": {"executables": [{"name": Path(r["exe"]).name, "size": r["exe_size"]}]},
            "status": status, "graphics": graphics_route(r), "env": {}, "ime_patch": None,
            "fp_mem": "candidate" if "candidate" in r.get("large_block_alloc", "") else "off",
            "bigcache_mb": 256, "dxvk": [],
            "notes": r["verdict"][1] + ("; Media Foundation videos need a playback test" if
                                       r.get("runtime", {}).get("media_foundation") else "")}
    if r.get("vr") == ["OpenVR"]:
        profile["revive"] = False
    if r.get("folder"):
        info = {"exe": r["exe"], "game_dir": r["folder"]}
        for hook in FX.GAME_INFO_HOOKS:
            hook("", info)
        # Keep an existing build-matched direct-OpenVR choice (e.g. Unity that also contains inert LibOVR strings)
        # when generating pasteable data. Do not propagate guesses from the folder/title or a bundled crack.
        if info.get("revive") is False:
            profile["revive"] = False
    return profile


def verdict(r: dict) -> tuple[str, str]:
    """(likely OK | needs work | blocked, why). "likely OK" = the profile First Contact has on the Frame: x64,
    D3D11, Unreal with the IME fix site found (or an engine without that deadlock), a VR API Revive/SteamVR serve."""
    blockers, work = [], []
    if r.get("arch") not in ("x86_64", "amd64", "AMD64"):
        work.append(f"{r.get('arch')} exe: needs GE's 32-bit (wow64) path under FEX (untested)")
    g = r.get("graphics", [])
    if "d3d11" not in g:
        if "d3d12" in g:
            work.append("D3D12: needs vkd3d-proton on Turnip under FEX (untested)")
        elif "d3d9" in g:
            work.append("D3D9: DXVK under FEX (untested)")
        elif "opengl" in g:
            work.append("OpenGL: no FEX GL thunk configured; rootfs GL renderer unverified")
        elif "vulkan" in g:
            work.append("Vulkan: Turnip through FEX's Vulkan thunk (untested with a game)")
        else:
            work.append(f"graphics API unclear ({', '.join(g)})")
    if "anti-cheat files" in r.get("drm", []) or r.get("runtime", {}).get("anti_cheat"):
        blockers.append("anti-cheat")
    elif r.get("drm"):
        work.append("anti-tamper section (" + ", ".join(r["drm"]) + "): heavier under FEX, may break")
    if r.get("vr") == ["?"]:
        work.append("VR API not detected")
    if r.get("engine", "").startswith("Unreal") and isinstance(r.get("ime_patch"), str):
        work.append(f"IME fix site not found ({r['ime_patch']})")
    if r.get("engine") in ("custom", "CryEngine"):
        work.append(f"{r['engine']} engine: no Unreal/Unity fixes apply; startup/allocator behaviour unknown")
    if blockers:
        return "blocked", "; ".join(blockers + work)
    if work:
        return "needs work", "; ".join(work)
    return "likely OK", "same profile as First Contact (x64, D3D11, " + "/".join(r.get("vr", [])) + ")"


def library_games():
    for g in library.games():
        if not str(g.get("package", "")).startswith("rift."):
            continue
        extra = (g.get("analysis") or {}).get("extra") or {}
        gd = g.get("game_dir") or extra.get("folder") or ""
        yield g.get("title") or g["package"], Path(gd), g.get("exe") or extra.get("exe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--folder", action="append", default=[])
    ap.add_argument("--out", default=str(user_data_dir() / "fexrift"))
    args = ap.parse_args()
    if args.folder:
        games = []
        for f in args.folder:
            a = rift.analyze(Path(f))
            games.append((a.label, Path(f), a.extra.get("exe")))
    else:
        games = list(library_games())
    results = []
    for title, d, exe in sorted(games, key=lambda t: t[0].lower()):
        try:
            r = survey_game(title, d, exe)
        except Exception as e:   # noqa: BLE001 - one broken folder shouldn't stop the survey
            r = {"title": title, "folder": str(d), "verdict": ("error", f"{type(e).__name__}: {e}")}
        results.append(r)
        print(f"{r['verdict'][0]:11s} {title}", flush=True)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "survey.json").write_text(json.dumps(results, indent=1))
    profiles = {re.sub(r"[^a-z0-9]+", "-", r["title"].lower()).strip("-"): r["profile"]
                for r in results if r.get("profile")}
    (out / "profiles.json").write_text(json.dumps(profiles, indent=2) + "\n")
    lines = ["# Rift-on-Frame static survey", "",
             "| Game | Verdict | Engine | Graphics | VR | Platform SDK | IME fix | AA config | Notes |",
             "|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        cfg = r.get("config") or {}
        aa = ", ".join(f"{k}={v}" for k, v in cfg.items() if "MSAA" in k or "AntiAliasing" in k) or r.get("pak", "")
        ime = r.get("ime_patch")
        if isinstance(ime, dict):
            ime = ime["rva"] + ("" if ime.get("module") == "exe" else f" ({ime['module']})")
        else:
            ime = str(ime)[:40] if r.get("engine", "").startswith("Unreal") else "n/a"
        graphics = ", ".join(r.get("graphics", [])) + (f" ({r['graphics_from']})" if r.get("graphics_from") else "")
        lines.append(f"| {r['title']} | {r['verdict'][0]} | {r.get('engine', '')} | {graphics} | "
                     f"{', '.join(r.get('vr', []))} | {'yes' if r.get('platform_sdk') else 'no'} | {ime} | {aa} | "
                     f"{r['verdict'][1]} |")
    (out / "survey.md").write_text("\n".join(lines) + "\n")
    print(f"report: {out / 'survey.md'}")


if __name__ == "__main__":
    main()
