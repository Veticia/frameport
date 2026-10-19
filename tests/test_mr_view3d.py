"""mr/view3d.py: room geometry for the Room tab's canvas, the orbit camera and the mesh reduction."""
import math
import re

import pytest

from frameport.mr import view3d as V
from frameport.mr.room import Mesh, Volume, box_room, preview_svg


def grid_mesh(n=60, size=4.0):
    """A floor of n x n quads facing up (+Y) = into the room, like a scan's floor."""
    verts, idx = [], []
    step = size / n
    for i in range(n + 1):
        for j in range(n + 1):
            verts.append((-size / 2 + j * step, 0.0, -size / 2 + i * step))
    for i in range(n):
        for j in range(n):
            a = i * (n + 1) + j
            b, c, d = a + 1, a + n + 1, a + n + 2
            idx += [a, c, b, b, c, d]  # counter-clockwise seen from above
    return Mesh(vertices=verts, indices=idx)


def test_box_corners_match_preview_svg():
    room = box_room(4, 3)
    room.volumes.append(Volume("0" * 32, "TABLE", 0.5, -0.4, 0.75, 30.0, 1.2, 0.6, 0.75))
    svg = preview_svg(room)
    # the preview's furniture polygon, mapped back to room x/z with the same scale/offset preview_svg uses
    points = re.findall(r'<polygon points="([^"]+)" fill="#275772"', svg)[0].split()
    xs, zs = [x for x, _ in room.outline], [z for _, z in room.outline]
    scale = 440 / max(max(xs) - min(xs), max(zs) - min(zs))
    back = [(min(xs) + (float(p.split(",")[0]) - 30) / scale, max(zs) - (float(p.split(",")[1]) - 30) / scale)
            for p in points]
    ours = V.box_corners(0.5, -0.4, 30.0, 1.2, 0.6)
    assert ours == [pytest.approx(p, abs=0.01) for p in back]


def test_prepare_walls_face_inward_and_boxes_have_six_sides():
    room = box_room(4, 3)
    room.volumes.append(Volume("0" * 32, "COUCH", 0, 0, 0.8, 0, 2, 1, 0.8))
    sc = V.prepare(room)
    walls = [f for f in sc.faces if f.kind == "wall"]
    boxes = [f for f in sc.faces if f.kind == "box"]
    assert len(walls) == 4 and len(boxes) == 6 and len(sc.box_edges) == 12
    for w in walls:  # the normal points towards the room's middle
        mid = [sum(p[k] for p in w.points) / 4 for k in range(3)]
        assert w.normal[0] * -mid[0] + w.normal[2] * -mid[2] > 0
    top = [f for f in boxes if f.normal[1] > 0.9][0]
    assert all(p[1] == pytest.approx(0.8) for p in top.points)
    assert sc.lo[0] == pytest.approx(-2) and sc.hi[2] == pytest.approx(1.5)


def test_projection_centre_and_behind_camera():
    cam = V.Camera(yaw=0, pitch=0, distance=5, target=(0, 1, 0), width=800, height=600)
    assert cam.eye == pytest.approx((0, 1, 5))
    (x, y), behind = cam.project([(0, 1, 0), (0, 1, 6)])
    assert (x, y) == pytest.approx((400, 300)) and behind is None
    right, up = cam.project([(1, 1, 0), (0, 2, 0)])
    assert right[0] > 400 and up[1] < 300  # +X right, +Y up on screen
    pr = cam.frame()
    seg = pr.segment((0, 1, 0), (0, 1, 10))  # crosses the near plane: clipped, not dropped
    assert seg is not None and seg[0] == pytest.approx((400, 300))
    poly = pr.polygon([(-1, 1, 2), (1, 1, 2), (1, 1, 6), (-1, 1, 6)])  # half behind the camera: clipped
    assert poly is not None and len(poly) == 4
    assert pr.polygon([(-1, 1, 5.5), (1, 1, 5.5), (0, 1, 6)]) is None


def test_dollhouse_hides_near_walls():
    room = box_room(4, 4)
    sc = V.prepare(room)
    cam = V.Camera(yaw=0, pitch=0.5, distance=10, target=(0, 1, 0))
    f = V.render(sc, cam)
    assert len(f.walls) == 3  # the wall between the camera (+Z) and the room is left out
    cam.distance = 0.5  # inside the room every wall faces the camera
    assert len(V.render(sc, cam).walls) >= 3


def test_mesh_reduction_fits_budget_and_culls_back_faces():
    mesh = grid_mesh()
    lods = V.reduce_mesh(mesh, {"rest": 400, "drag": 120})
    assert len(lods["rest"].edges) <= 400 and len(lods["drag"].edges) <= 120
    assert lods["rest"].cell <= lods["drag"].cell
    assert all(n[1] > 0.99 for n in lods["rest"].normals)
    room = box_room(4, 4)
    sc = V.prepare(room, mesh, budget=400, drag_budget=120)
    above = V.Camera(yaw=0, pitch=1.2, distance=8, target=(0, 0, 0))
    below = V.Camera(yaw=0, pitch=-0.15, distance=8, target=(0, 0.5, 0))
    assert V.render(sc, above).mesh and not V.render(sc, below).mesh
    assert len(V.render(sc, above, "drag").mesh) <= 120


def test_real_scan_sized_mesh_stays_within_budget():
    mesh = grid_mesh(n=180, size=7.0)  # ~65k triangles
    lods = V.reduce_mesh(mesh, {"rest": V.REST_BUDGET, "drag": V.DRAG_BUDGET})
    assert 0 < len(lods["rest"].edges) <= V.REST_BUDGET and len(lods["drag"].edges) <= V.DRAG_BUDGET


def test_camera_controls_and_fit():
    sc = V.prepare(box_room(6, 4))
    cam = V.Camera(width=900, height=500)
    cam.fit(sc)
    pts = [p for p in cam.project([(x, y, z) for x in (-3, 3) for y in (0, 2.5) for z in (-2, 2)])]
    assert all(p is not None and 0 <= p[0] <= 900 and 0 <= p[1] <= 500 for p in pts)
    d = cam.distance
    cam.zoom(0.5)
    assert cam.distance == pytest.approx(d / 2)
    cam.orbit(0, 10_000)
    assert cam.pitch == pytest.approx(V.Camera.MAX_PITCH)
    t = cam.target
    cam.pan(100, 0)
    assert cam.target != t
    cam.fit(sc)
    assert cam.target[0] == pytest.approx(0) and not math.isnan(cam.distance)
