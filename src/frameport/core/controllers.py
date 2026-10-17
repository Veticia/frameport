"""Controller preferences at the library boundary; recipe and configuration identities stay preference-free."""
from __future__ import annotations

from . import library

QUEST_BUTTONS = "adapter.quest_buttons"
CHOICES = ((0, "SteamVR default"), (1, "Mirror"), (2, "Quest pair"))


def quest_buttons_default() -> int:
    value = library.setting("input.quest_buttons", 0)
    return int(value) if value in (0, 1, 2, "0", "1", "2") else 0


def effective_adapter_settings(patches: dict) -> dict:
    from ..patches.frame.headset_model import adapter_settings

    return adapter_settings(patches, defaults={"quest_buttons": quest_buttons_default()})


def apply_default(target, reporter) -> list[str]:
    """Push only inherited Quest preferences. Read the library and device off the UI thread."""
    mode = quest_buttons_default()
    reporter.stage("Apply controller buttons")
    games = {g["package"]: g for g in library.games()}
    applied = []
    for installed in target.installed():
        reporter.check_cancel()
        package = installed["package"]
        game = games.get(package, {})
        if installed.get("kind", "quest") != "quest" or game.get("kind", "quest") != "quest":
            continue
        analysis = game.get("analysis") or {}
        if (analysis.get("extra") or {}).get("vr_kind", "quest") != "quest":
            continue
        patches = (game.get("recipe") or {}).get("patches") or {}
        recorded = (installed.get("recipe") or {}).get("patches") or []
        if QUEST_BUTTONS in patches or (not game and QUEST_BUTTONS in recorded):
            continue
        target.set_settings(package, {"quest_buttons": mode})
        applied.append(package)
        reporter.log(f"{package}: quest_buttons={mode} (next launch)")
    return applied
