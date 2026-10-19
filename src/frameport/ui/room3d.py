"""The Room tab's 3D view inside the window: a Flet canvas with a fixed handful of layers (floor grid, floor, walls,
furniture, the scanned mesh as lines, where you stand), each one shape, sent anew on each redraw (see draw()).
Projection is mr/view3d.py's (pure Python).

Kept light (CLAUDE.md performance rules): redraws run on one background worker, at most ~18 a second, and go out
through a components.LoopUpdater; while the view moves the mesh uses its smaller set of edges, a moment after the
pointer stops the full one is drawn. Points are whole pixels (a smaller patch: ~23 bytes a segment).

Gestures: drag = turn, scroll / pinch = zoom, right-drag or two-finger drag = move, double-click = fit."""
from __future__ import annotations

import threading
import time

import flet as ft
import flet.canvas as cv

from ..i18n import tr
from ..mr import view3d as V
from . import components as C
from . import theme as T

FPS = 18           # most redraws a second while the view moves
SETTLE = 0.25      # seconds without movement before the full detail is drawn
MAX_DRAG_BOX_FACES = 120  # more furniture sides than this: only their outlines while the view moves


class RoomCanvas:
    """Created once per Room tab; set_scene() (any thread) shows a room, None clears it."""

    def __init__(self, page: ft.Page | None, height: int = 460):
        self.page = page
        self.scene: V.Scene | None = None
        self.cam = V.Camera()
        self.width, self.height = 0.0, float(T.px(height))
        self.stats = {"ms": 0.0, "segments": 0, "lod": "rest", "redraws": 0}
        self._lock = threading.Lock()
        self._want: str | None = None   # the detail of the pending redraw ("rest" / "drag")
        self._running = False
        self._moved = 0.0                # monotonic time of the last movement
        self._scale = 1.0
        self._right: tuple[float, float] | None = None
        self._push = C.LoopUpdater(page) if page is not None else None
        blue, stroke, fill = T.SECONDARY, ft.PaintingStyle.STROKE, ft.PaintingStyle.FILL
        # layer -> (kind, paint settings), bottom to top; read from the theme now (theme rule)
        self.layers = {
            "grid": ("lines", dict(color=T.soft(T.TEXT_3, 0.18), stroke_width=1, style=stroke)),
            "floor": ("fill", dict(color=T.soft(blue, 0.06), style=fill)),
            "hidden": ("fill", dict(color=T.soft(blue, 0.03), style=fill)),
            "walls": ("fill", dict(color=T.soft(blue, 0.11), style=fill)),
            "mesh": ("lines", dict(color=T.soft(blue, 0.42), stroke_width=1, style=stroke)),
            "wall_lines": ("lines", dict(color=T.soft(blue, 0.6), stroke_width=T.px(1.4), style=stroke)),
            "boxes": ("fill", dict(color=T.soft(T.ACCENT, 0.16), style=fill)),
            "box_lines": ("lines", dict(color=T.soft(T.ACCENT, 0.85), stroke_width=T.px(1.3), style=stroke)),
            "planes": ("fill", dict(color=T.soft(T.OK, 0.35), style=fill)),
            "selected": ("lines", dict(color=T.TEXT, stroke_width=T.px(2.5), style=stroke)),
            "marker": ("fill", dict(color=T.ACCENT, style=fill)),
        }
        self.on_pick = None  # callback(("box", i) / ("plane", i) / None) for a click on the room (the box editor)
        self.canvas = cv.Canvas([], expand=True, on_resize=self._resized)
        self.detector = ft.GestureDetector(
            self.canvas, mouse_cursor=ft.MouseCursor.MOVE, drag_interval=16,
            on_scale_start=self._scale_start, on_scale_update=self._scale_update, on_scale_end=self._scale_end,
            on_scroll=self._scroll, on_double_tap=lambda e: self.fit(), on_tap_up=self._tap,
            on_right_pan_start=self._right_start, on_right_pan_update=self._right_update,
            on_right_pan_end=lambda e: self._end_move())
        self.control = ft.Container(self.detector, height=self.height, expand=True, bgcolor=T.BG,
                                    border_radius=T.RADIUS_SM, clip_behavior=ft.ClipBehavior.HARD_EDGE)

    # ------------------------------------------------------------------ data
    def set_scene(self, scene: V.Scene | None, fit: bool = True) -> None:
        self.scene = scene
        if scene is not None and fit:
            self.cam.fit(scene)
        self.request("rest")

    def fit(self) -> None:
        if self.scene is not None:
            self.cam.fit(self.scene)
            self.request("rest")

    def _resized(self, e) -> None:
        self.width, self.height = float(e.width or 0), float(e.height or self.height)
        first = self.cam.width != self.width and self.cam.width == 800.0  # the camera's default size
        self.cam.width, self.cam.height = self.width, self.height
        if first and self.scene is not None:
            self.cam.fit(self.scene)
        self.request("rest")

    # ------------------------------------------------------------------ gestures
    def _scale_start(self, e) -> None:
        self._scale = 1.0

    def _scale_update(self, e) -> None:
        d = e.focal_point_delta
        if e.pointer_count >= 2:  # trackpad / touch: two fingers move the view and pinch zooms
            if e.scale and self._scale:
                self.cam.zoom(self._scale / e.scale)
                self._scale = e.scale
            self.cam.pan(d.x, d.y)
        else:
            self.cam.orbit(d.x, d.y)
        self._move()

    def _scale_end(self, e) -> None:
        self._end_move()

    def _scroll(self, e) -> None:
        dy = e.scroll_delta.y if e.scroll_delta else 0
        if dy:
            self.cam.zoom(1.0015 ** dy)
            self._move()
            self._end_move()

    def _tap(self, e) -> None:
        if self.on_pick is None or self.scene is None or e.local_position is None:
            return
        self.on_pick(V.pick(self.scene, self.cam, e.local_position.x, e.local_position.y))

    def _right_start(self, e) -> None:
        self._right = (e.local_position.x, e.local_position.y)

    def _right_update(self, e) -> None:
        p = (e.local_position.x, e.local_position.y)
        if self._right is not None:
            self.cam.pan(p[0] - self._right[0], p[1] - self._right[1])
            self._move()
        self._right = p

    def _move(self) -> None:
        self._moved = time.monotonic()
        self.request("drag")

    def _end_move(self) -> None:
        self._right = None
        self._moved = time.monotonic()
        self.request("drag")  # the worker draws the full detail once the pointer has rested SETTLE seconds

    # ------------------------------------------------------------------ drawing
    def request(self, lod: str) -> None:
        with self._lock:
            if self._want != "drag":
                self._want = lod
            if self._running:
                return
            self._running = True
        if self.page is not None:
            try:
                self.page.run_thread(self._loop)
                return
            except Exception:  # noqa: BLE001 - no session (tests): draw right here
                pass
        self._loop()

    def _loop(self) -> None:
        """The one redraw worker: draws the newest camera at most FPS times a second, then the full detail once the
        view has been still for SETTLE seconds, then ends."""
        while True:
            with self._lock:
                want, self._want = self._want, None
                if want is None:
                    if self.stats["lod"] != "drag" or self.scene is None:
                        self._running = False
                        return
                    wait = SETTLE - (time.monotonic() - self._moved)
                    if wait <= 0:
                        want = "rest"
            if want is None:
                time.sleep(min(SETTLE, max(0.02, wait)))
                continue
            t0 = time.perf_counter()
            self.draw(want)
            spent = time.perf_counter() - t0
            if self._push is not None:
                self._push(self.canvas)
            else:
                C.update(self.canvas)
            time.sleep(max(0.0, 1.0 / FPS - spent))

    def draw(self, lod: str = "rest") -> V.Frame2D | None:
        """Project the scene into new shapes for the canvas (no update sent). New shape controls rather than new
        points in the old ones: Flet diffs a changed list of points item by item, which took 0.3-1 s per redraw
        for ~2000 segments; a replaced shape is sent whole (~14 bytes a segment) and costs ~10 ms."""
        t0 = time.perf_counter()
        if self.scene is None or self.width <= 0:
            self.canvas.shapes = []
            self.stats.update(lod="rest", segments=0)
            return None
        f = V.render(self.scene, self.cam, lod)
        data = {"grid": f.grid, "floor": [f.floor] if f.floor else [], "hidden": f.hidden_walls, "walls": f.walls,
                "mesh": f.mesh, "wall_lines": f.wall_lines, "boxes": f.boxes, "box_lines": f.box_lines,
                "planes": f.planes, "selected": f.selected, "marker": [f.marker] if f.marker else []}
        if lod == "drag" and len(f.boxes) > MAX_DRAG_BOX_FACES:  # a room full of furniture: outlines while moving
            data["boxes"] = []
        shapes = []
        for name, (kind, paint) in self.layers.items():
            if not data[name]:
                continue
            if kind == "lines":
                shapes.append(cv.Points(_lines(data[name]), point_mode=cv.PointMode.LINES, paint=ft.Paint(**paint)))
            else:
                shapes.append(cv.Path(_polys(data[name]), paint=ft.Paint(**paint)))
        self.canvas.shapes = shapes
        self.stats.update(ms=(time.perf_counter() - t0) * 1000, segments=f.segments, lod=lod,
                          redraws=self.stats["redraws"] + 1)
        return f

    def legend(self) -> ft.Control:
        """What the colours mean (under the canvas)."""
        def key(color, text, line=False):
            mark = ft.Container(width=T.px(14), height=T.px(2 if line else 10), bgcolor=color,
                                border_radius=T.px(2))
            return ft.Row([mark, C.meta(text)], spacing=T.px(6), tight=True,
                          vertical_alignment=ft.CrossAxisAlignment.CENTER)
        return ft.Row([key(T.soft(T.SECONDARY, 0.5), tr("Walls")),
                       key(T.soft(T.SECONDARY, 0.7), tr("Scanned surfaces"), line=True),
                       key(T.soft(T.ACCENT, 0.6), tr("Furniture")),
                       key(T.soft(T.OK, 0.6), tr("Doors and windows")),
                       key(T.ACCENT, tr("Middle of your play area, facing forward"))],
                      spacing=T.S4, run_spacing=T.S2, wrap=True)


def _lines(segments) -> list[ft.Offset]:
    out = []
    for (ax, ay), (bx, by) in segments:
        out.append(ft.Offset(round(ax), round(ay)))
        out.append(ft.Offset(round(bx), round(by)))
    return out


def _polys(polys) -> list:
    out = []
    for poly in polys:
        out.append(cv.Path.MoveTo(round(poly[0][0]), round(poly[0][1])))
        out += [cv.Path.LineTo(round(x), round(y)) for x, y in poly[1:]]
        out.append(cv.Path.Close())
    return out
