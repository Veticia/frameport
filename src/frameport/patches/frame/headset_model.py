"""Per-game OpenXR headset identity (GitHub #373)."""
from __future__ import annotations

from ..base import Param, Patch, Suggestion, register

# Quest 2 deliberately leaves both layers untouched: the compatibility escape hatch.
MODELS = {"": (3, "Quest 3"), "quest3s": (4, "Quest 3S"),
          "questpro": (5, "Quest Pro"), "quest2": (2, "Quest 2")}


# Raw XrUuidEXT bytes, in comparison order in OVRPlugin 0x292f80 (not GUID field-endian order).
UUIDS = {"quest2": bytes.fromhex("3064f23915ed4722a6595ea1ecd899c2"),
         "questpro": bytes.fromhex("b58443c9df8640cd89182a278e18ab71"),
         "": bytes.fromhex("082746baa0714cf78f87bade5cce43ce"),
         "quest3s": bytes.fromhex("9510492b0d424872bcf4fb1a8def6f0e")}
# "Meta Quest Pro" plus NUL exceeds the original 14-byte slot. Its UUID selects Pro; use a short label.
SYSTEM_NAMES = {"": b"Meta Quest 3", "quest3s": b"Meta Quest 3S",
                "questpro": b"Quest Pro", "quest2": b"Oculus Quest2"}
# Verified OVRPort 3.4.3-aa54c3f arm64 dispatcher: file offsets (VAs differ).
# Guard both data and the instructions that load those exact answers, before changing either.
NAME_OFFSET, UUID_OFFSET = 0x6034, 0x21338
GUARDS = {0x18620: bytes.fromhex("61ffffd021d00091807200910220805203208052c401805262210094"),
          0x186e4: bytes.fromhex("1f2003d584620810"),
          0x18778: bytes.fromhex("8000c03de006803de2ffff17")}


def patch_dispatcher(data: bytes, key: str) -> bytes:
    """Same-size data-only edit; unknown dispatchers fail atomically. Quest 2 is an exact no-op."""
    if key not in SYSTEM_NAMES:
        raise ValueError(f"Unknown headset model: {key!r}")
    if key == "quest2":
        return data
    expected = {NAME_OFFSET: b"Oculus Quest2\0", UUID_OFFSET: UUIDS["quest2"], **GUARDS}
    if not all(data[off:off + len(value)] == value for off, value in expected.items()):
        raise ValueError("Unsupported OVRPort headset identity dispatcher: expected 3.4.3-aa54c3f arm64 "
                         "systemName/UUID and load instructions; select Quest 2 to preserve its identity.")
    out = bytearray(data)
    out[NAME_OFFSET:NAME_OFFSET + 14] = SYSTEM_NAMES[key].ljust(14, b"\0")
    out[UUID_OFFSET:UUID_OFFSET + 16] = UUIDS[key]
    return bytes(out)


def model(params):
    key = (params or {}).get("model", "")
    if key not in MODELS:
        raise ValueError(f"Unknown headset model: {key!r}")
    return MODELS[key]


def adapter_settings(recipe_patches):
    """Share the device choice with embedded settings and configuration-only updates."""
    from ..settings import adapter_settings as other_settings

    out = other_settings(recipe_patches)
    if "device.headset_model" in recipe_patches:
        out["headset_model"] = model(recipe_patches["device.headset_model"])[0]
    return out


class HeadsetModel(Patch):
    id = "device.headset_model"
    title = "Headset model"
    description = ("Choose the OpenXR headset name and headset UUID reported to Quest games. "
                   "Quest 3 is the default; Quest 2 preserves OVRPort's original answers. "
                   "Requires a rebuild. Android Build.MODEL remains Lepton.")
    category = "device"
    stage = "apk"
    order = 12
    default_on = True
    summary = "Choose the headset identity the game sees; Quest 3 is the default."
    CHOICES = (("", "Quest 3 (default)"), ("quest3s", "Quest 3S"), ("questpro", "Quest Pro"), ("quest2", "Quest 2"))
    params = [Param("model", "str", "", "Headset model reported to the game.")]

    def applies(self, a):
        return a.vr_kind == "quest" and (a.extra or {}).get("kind") not in ("rift", "linux")

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "Quest 3 hardware is closest to Steam Frame: report Quest 3 by default.")
        return None

    def apply(self, ctx):
        if not self.applies(ctx.analysis):
            return False
        key = (ctx.params or {}).get("model", "")
        model(ctx.params)  # validate before even a missing-library no-op
        ws = ctx.ws
        loader = "libopenxr_loader.so"
        if key == "quest2":
            return False
        if not ws.has(ws.lib(loader)):
            ctx.checks.append((self.id, None, "No OVRPort dispatcher; only FrameBridge's systemName override applies."))
            return False
        fixed = patch_dispatcher(ws.read_lib(loader), key)
        ws.put(ws.lib(loader), fixed)
        ctx.notes.append(f"{loader}: systemName={SYSTEM_NAMES[key].decode()}, HeadsetId={UUIDS[key].hex()}")
        return True



def migrate(data):
    """Once per library, include existing Quest recipes in the new default."""
    for game in (data.get("games") or {}).values():
        analysis, recipe = game.get("analysis"), game.get("recipe")
        if not isinstance(analysis, dict) or not isinstance(recipe, dict):
            continue
        extra = analysis.get("extra") or {}
        if (game.get("kind") in ("rift", "linux") or extra.get("kind") in ("rift", "linux")
                or extra.get("vr_kind", "quest") != "quest"):
            continue
        recipe.setdefault("patches", {}).setdefault(HeadsetModel.id, {})
        recipe.setdefault("reasons", {}).setdefault(HeadsetModel.id, "Report Quest 3 by default.")


register(HeadsetModel)
