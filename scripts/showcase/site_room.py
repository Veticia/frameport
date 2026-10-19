"""Write the site demo's room (site/public/demo/room.json) for the Room tab of the clickable app on the project page:
the showcase's made-up bedroom (fakes.fake_room, no real scan), its scanned surfaces, the label rules' decisions and
the door suggestion, plus what a raw scan has before the clean-up (pieces floating in the air, the sweep of an arm
held up while looking at the ceiling, surfaces seen through the walls, and the boxes the label rules remove).

    uv run python scripts/showcase/site_room.py

Coordinates are centimetres (integers), metres in the JSON's box fields like room.txt; y is up, the play area's
middle is the origin, -z is forward.
"""
from __future__ import annotations

import json
import math
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from fakes import FakeRoom, fake_room, fake_room_mesh  # noqa: E402

from frameport.mr.room import plane_corners  # noqa: E402

OUT = ROOT / "site/public/demo/room.json"
CELL = 0.25  # coarser than the app's renders: the page draws every edge


def cm(p):
    return [round(c * 100) for c in p]


def junk(seed: int = 11):
    """Triangles a raw scan has and the clean-up removes, as (kind, [vertex], [index])."""
    rnd = random.Random(seed)
    pieces = []

    def blob(cx, cy, cz, r, n=7):  # a small crumpled patch in the air
        verts = [(cx + rnd.uniform(-r, r), cy + rnd.uniform(-r, r) * 0.6, cz + rnd.uniform(-r, r)) for _ in range(n)]
        idx = []
        for i in range(1, n - 1):
            idx += [0, i, i + 1]
        return verts, idx

    for c in [(0.2, 1.7, 0.3), (-0.5, 2.1, 0.8), (1.0, 1.1, 1.1), (-0.9, 1.6, -0.4), (0.6, 2.3, -0.6), (1.3, 0.4, 0.9)]:
        pieces.append(("speck", *blob(*c, r=0.09)))
    # an arm and controller held up while looking at the ceiling: a thin tube swept from 1.5 m to the ceiling
    a, b, r, seg, ring = (0.85, 1.5, 0.35), (1.2, 2.48, 0.5), 0.035, 10, 8
    verts, idx = [], []
    for i in range(seg + 1):
        t = i / seg
        c = [a[k] + (b[k] - a[k]) * t for k in range(3)]
        for j in range(ring):
            th = 2 * math.pi * j / ring
            verts.append((c[0] + r * math.cos(th), c[1] + rnd.uniform(-0.01, 0.01), c[2] + r * math.sin(th)))
    for i in range(seg):
        for j in range(ring):
            p, q = i * ring + j, i * ring + (j + 1) % ring
            idx += [p, q, q + ring, p, q + ring, p + ring]
    pieces.append(("arm", verts, idx))
    # seen through the glass and a door gap: surfaces outside the room
    for cx, cz, w in [(2.35, 0.3, 0.7), (-2.3, 0.9, 0.5)]:
        verts = [(cx + rnd.uniform(-0.03, 0.03), 0.3 + y * 0.35, cz + (x - 1) * w / 2)
                 for y in range(4) for x in range(3)]
        idx = []
        for y in range(3):
            for x in range(2):
                p = y * 3 + x
                idx += [p, p + 1, p + 4, p, p + 4, p + 3]
        pieces.append(("outside", verts, idx))
    return pieces


def main():
    room = fake_room()
    mesh = fake_room_mesh(room, cell=CELL)
    verts = [cm(v) for v in mesh.vertices]
    tris = list(mesh.indices)
    kinds = {}
    raw_from = len(tris) // 3
    for kind, jv, ji in junk():
        base = len(verts)
        verts += [cm(v) for v in jv]
        pieces, count = kinds.get(kind, (0, 0))
        kinds[kind] = (pieces + 1, count + len(ji) // 3)  # (pieces, triangles)
        tris += [base + i for i in ji]

    labels = FakeRoom.LABELS
    kept = [lb for lb in labels if lb["to"]]
    boxes = []
    for vol, lb in zip(room.volumes, kept, strict=True):
        boxes.append({"label": lb["to"], "from": lb["from"], "reason": lb["reason"], "cx": vol.cx, "cz": vol.cz,
                      "top": vol.top_y, "yaw": vol.yaw_deg, "w": vol.w, "d": vol.d, "h": vol.h})
    removed_at = [(2.15, 0.2, 0.8, 0), (0.3, 0.75, 0.03, 0)]  # behind the right wall; a rug on the floor
    removed = []
    for lb, (cx, cz, top, yaw) in zip([lb for lb in labels if not lb["to"] and not lb.get("suggest")], removed_at,
                                      strict=True):
        removed.append({"label": lb["from"], "reason": lb["reason"], "cx": cx, "cz": cz, "top": top, "yaw": yaw,
                        **{k: lb["box"][k] for k in ("w", "d", "h")}})
    sug = FakeRoom.SUGGESTIONS[0]
    slab = next(lb for lb in labels if lb.get("suggest"))
    suggestion = {"kind": sug["kind"], "reason": sug["reason"], "from": slab["from"], "detail": slab["reason"],
                  "cx": sug["box"]["cx"], "cz": sug["box"]["cz"], "top": sug["box"]["top_y"],
                  "yaw": sug["box"]["yaw_deg"], **{k: sug["box"][k] for k in ("w", "d", "h")}}

    data = {
        "outline": [[round(x, 2), round(z, 2)] for x, z in room.outline],
        "floor": room.floor, "ceiling": room.ceiling,
        "walls": [[w.x0, w.z0, w.x1, w.z1, w.y0, w.y1] for w in room.walls],
        "planes": [{"label": p.label, "corners": cm(sum(plane_corners(p), ()))} for p in room.planes],
        "boxes": boxes, "removed": removed, "suggestion": suggestion,
        "mesh": {"v": sum(verts, []), "t": tris, "raw_from": raw_from, "junk": kinds},
    }
    OUT.write_text(json.dumps(data, separators=(",", ":")) + "\n")
    print(f"{OUT.relative_to(ROOT)}: {len(verts)} vertices, {len(tris) // 3} triangles "
          f"({raw_from} kept), {OUT.stat().st_size // 1024} KiB")


if __name__ == "__main__":
    main()
