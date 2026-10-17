#!/usr/bin/env python3
"""Generate controller illustrations and the user mapping table from the shared input contract."""
from __future__ import annotations

import argparse
import ast
import json
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "docs/input/quest_buttons.json"
DOC = ROOT / "docs/CONTROLLERS.md"
START = "<!-- quest-buttons-table:start -->"
END = "<!-- quest-buttons-table:end -->"


def portal_colors() -> dict:
    # Read literal tokens without importing the GUI or consulting user preferences.
    tree = ast.parse((ROOT / "src/frameport/ui/theme.py").read_text(encoding="utf-8"))
    return next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
                and any(isinstance(t, ast.Name) and t.id == "_PORTAL" for t in n.targets))


def inverse(mode: dict, hand: str) -> dict:
    return {frame: touch.upper() if touch != "menu" else "Menu"
            for touch, frames in mode.get(hand, {}).items() for frame in frames}


def mapping_table(data: dict) -> str:
    rows = ["| Frame input | SteamVR default (0) | Mirror (1) | Quest pair (2) |", "|---|---|---|---|"]
    inputs = (("left", "dpad_up", "Left d-pad ↑"), ("left", "dpad_down", "Left d-pad ↓"),
              ("left", "dpad_left", "Left d-pad ←"), ("left", "dpad_right", "Left d-pad →"),
              ("left", "view", "Left View"), ("right", "a", "Right A (bottom)"),
              ("right", "b", "Right B (right)"), ("right", "x", "Right X (left)"),
              ("right", "y", "Right Y (top)"), ("right", "menu", "Right Menu"),
              ("left", "bumper", "Left bumper"), ("right", "bumper", "Right bumper"))
    for hand, key, label in inputs:
        values = [inverse(data["modes"][str(n)], hand).get(key, "—") for n in range(3)]
        rows.append("| " + " | ".join([label, *values]) + " |")
    return "\n".join(rows)


def illustration(data: dict, colors: dict, mode: str | None = None) -> str:
    compact = mode is not None
    width, height = (520, 334) if compact else (1080, 750)
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
             f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
             '<title id="title">Quest Touch buttons on Steam Frame controllers</title>',
             '<desc id="desc">' + escape(
                 (data["modes"][mode]["name"] + ": " + data["modes"][mode]["note"] if compact else
                  "Quest Touch reference and three Frame mappings: SteamVR default, Mirror, and Quest pair.")
                 + " Each button shows its physical input above and the Touch input it sends below. "
                   "A dash means unbound. Left Touch buttons are X and Y; right are A and B.") + '</desc>',
             f'<rect width="{width}" height="{height}" rx="16" fill="{colors["BG"]}"/>',
             '<g font-family="DejaVu Sans,Arial,sans-serif">']

    def text(x, y, label, size=14, color=None, anchor="start", weight=400):
        parts.append(f'<text x="{x}" y="{y}" fill="{color or colors["TEXT"]}" font-size="{size}" '
                     f'font-weight="{weight}" text-anchor="{anchor}">{escape(label)}</text>')

    def button(x, y, physical, touch):
        color = colors["SECONDARY"] if touch in ("X", "A") else colors["ACCENT"]
        if touch == "—":
            color = colors["TEXT_3"]
        parts.append(f'<circle cx="{x}" cy="{y}" r="25" fill="{colors["SURFACE_2"]}" '
                     f'stroke="{color}" stroke-width="1.5"/>')
        text(x, y - 6, physical, 11, colors["TEXT_2"], "middle")
        text(x, y + 15, touch, 19 if len(touch) == 1 else 13, color, "middle", 600)

    def small(x, y, physical, touch):
        parts.append(f'<rect x="{x - 57}" y="{y - 16}" width="114" height="32" rx="8" '
                     f'fill="{colors["SURFACE_2"]}"/>')
        text(x, y + 5, f"{physical}: {touch}", 12, colors["TEXT_2"], "middle")

    def panel(x, y, key):
        parts.append(f'<g transform="translate({x} {y})">')
        parts.append(f'<rect x="0" y="0" width="520" height="334" rx="12" '
                     f'fill="{colors["SURFACE"]}" stroke="{colors["BORDER"]}"/>')
        title = "Quest Touch reference" if key is None else data["modes"][key]["name"]
        text(20, 31, title, 20, weight=600)
        text(20, 55, "Physical button above · Touch input below", 13, colors["TEXT_2"])
        for hand, cx in (("left", 140), ("right", 380)):
            text(cx, 84, hand.capitalize(), 13, colors["TEXT_2"], "middle")
            if key is None:
                low, high = ("X", "Y") if hand == "left" else ("A", "B")
                button(cx - 23, 196, low, low)
                button(cx + 23, 141, high, high)
                small(cx, 257, "Menu", "Menu" if hand == "left" else "—")
            else:
                mapped = inverse(data["modes"][key], hand)
                positions = [(0, -50, "dpad_up", "↑"), (-50, 0, "dpad_left", "←"),
                             (50, 0, "dpad_right", "→"), (0, 50, "dpad_down", "↓")] if hand == "left" else [
                                 (0, -50, "y", "Y"), (-50, 0, "x", "X"),
                                 (50, 0, "b", "B"), (0, 50, "a", "A")]
                for dx, dy, name, label in positions:
                    button(cx + dx, 161 + dy, label, mapped.get(name, "—"))
                small(cx, 257, "View" if hand == "left" else "Menu", mapped.get("view", "—"))
                small(cx, 302, "Bumper", "—")
        if key is None:
            text(260, 306, "Lower: X / A · Upper: Y / B", 14, colors["TEXT_2"], "middle")
        parts.append('</g>')

    if compact:
        panel(0, 0, mode)
    else:
        text(20, 32, "Controller buttons", 24, weight=600)
        text(20, 57, "Blue: X / A · Orange: Y / B or Menu · —: no Touch input", 14, colors["TEXT_2"])
        for x, y, key in ((16, 72, None), (544, 72, "0"), (16, 414, "1"), (544, 414, "2")):
            panel(x, y, key)
    return "\n".join([*parts, '</g>', '</svg>', ''])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if generated files differ")
    parser.add_argument("--compact", action="store_true", help="generate one compact controller pair")
    parser.add_argument("--mode", choices=("0", "1", "2"), default="1")
    parser.add_argument("--out", type=Path, help="destination for --compact (default: the bundled app asset)")
    args = parser.parse_args()
    data = json.loads(CONTRACT.read_text(encoding="utf-8"))
    colors = portal_colors()
    assets = ROOT / "src/frameport/ui/icons"
    if args.compact:
        outputs = {args.out or assets / f"quest-buttons-{args.mode}.svg": illustration(data, colors, args.mode)}
    else:
        outputs = {ROOT / "docs/images/controllers/quest-buttons.svg": illustration(data, colors)}
        outputs.update({assets / f"quest-buttons-{n}.svg": illustration(data, colors, str(n)) for n in range(3)})
        doc = DOC.read_text(encoding="utf-8")
        before, rest = doc.split(START, 1)
        _, after = rest.split(END, 1)
        outputs[DOC] = before + START + "\n" + mapping_table(data) + "\n" + END + after
    stale = []
    for path, content in outputs.items():
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != content:
                stale.append(str(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path))
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
    if stale:
        print("Regenerate with uv run python scripts/controller_diagram.py: " + ", ".join(stale))
    return int(bool(stale))


if __name__ == "__main__":
    raise SystemExit(main())
