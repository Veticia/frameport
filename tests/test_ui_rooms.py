from types import SimpleNamespace

import flet as ft

from frameport.mr.room import box_room, dumps, summary
from frameport.ui.views.frame import FrameView, room_state


def walk(control):
    yield control
    for child in control._get_children() if hasattr(control, "_get_children") else []:
        yield from walk(child)


def fake_app(answer):
    pending, refreshes, toasts = [], [], []
    target = SimpleNamespace(frame=SimpleNamespace(agent=lambda command: answer))
    return SimpleNamespace(target=target, frame_state="connected", run_bg=lambda fn: pending.append(fn),
                           refresh_view=lambda: refreshes.append(1), pending=pending, refreshes=refreshes,
                           toast=lambda message, **kw: toasts.append(message), toasts=toasts)


def test_room_status_loads_off_render_path_and_caches(tmp_path):
    from frameport.core.paths import user_data_dir

    room = box_room(4, 3)
    app = fake_app(dict(room_text=dumps(room), room=dict(source="scan", summary=summary(room)),
                        aligned=True, scanner_installed=True, default_circle=False))
    state = room_state(app)
    assert state["loading"] and len(app.pending) == 1
    # Building the card while loading does not write a preview or query SSH.
    assert FrameView(app).room_card()
    assert not (user_data_dir() / "room-previews").exists()
    app.pending.pop()()
    assert state["preview"].endswith(".svg")
    assert (user_data_dir() / state["preview"]).exists()
    assert len(app.refreshes) == 1
    assert FrameView(app).room_card()
    room_state(app)
    assert not app.pending
    room_state(app, force=True)
    assert len(app.pending) == 1


def test_room_status_error_and_reconnection():
    app = fake_app({})
    def fail(command):
        raise RuntimeError("query failed")
    app.target.frame.agent = fail
    state = room_state(app)
    app.pending.pop()()
    assert "query failed" in state["error"]
    assert FrameView(app).room_card()
    app.frame_state = "disconnected"
    assert room_state(app)["target"] is None
    app.frame_state = "connected"
    assert room_state(app) is not state


def test_card_shows_preview_and_default_circle_hint():
    app = fake_app({})
    app._room_state = dict(target=app.target, status=dict(default_circle=True, room=None),
                           preview=None, error=None, loading=False, time=float("inf"))
    control = FrameView(app).room_card()
    # Building with each documented state catches invalid Flet properties without a desktop or socket.
    assert isinstance(control, ft.Control)
    app._room_state["status"].update(default_circle=False, aligned=True, scanner_installed=True,
                                    room=dict(source="boundary", summary=summary(box_room(3, 4))))
    app._room_state["preview"] = "room-previews/example.svg"
    assert FrameView(app).room_card()


# ------------------------------------------------------------------------------------------------ the Room tab
class MeshFrame:
    """A Frame whose agent answers room_status and whose SFTP serves the room mesh (counts opens)."""

    def __init__(self, tmp_path):
        from frameport.mr.room import Mesh, write_mesh

        self.home = str(tmp_path / "frame")
        self.room = box_room(4, 3)
        self.room.source, self.room.universe = "scan", 7
        verts = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
        data = write_mesh(Mesh(vertices=verts, indices=[0, 2, 1, 0, 3, 2]))
        self.room.mesh = Mesh(vertex_count=4, index_count=6)
        folder = tmp_path / "frame/.local/share/frameport/rooms/7"
        folder.mkdir(parents=True)
        (folder / "room-mesh.bin").write_bytes(data)
        self.opens, self.calls = 0, []

    def agent(self, command, **kw):
        self.calls.append(command)
        if command == "room_status":
            return dict(room_text=dumps(self.room), room=dict(source="scan", summary=summary(self.room)),
                        aligned=True, scanner_installed=True, default_circle=False)
        return {}

    @property
    def sftp(self):
        outer = self

        class SFTP:
            def open(self, path, mode="rb"):
                outer.opens += 1
                return open(path, mode)  # noqa: SIM115
        return SFTP()


def sync_app(frame):
    refreshes, toasts = [], []
    target = SimpleNamespace(frame=frame, label="steamframe")
    return SimpleNamespace(target=target, frame_state="connected", run_bg=lambda fn, *a: fn(*a), page=None,
                           refresh_view=lambda: refreshes.append(1), refreshes=refreshes,
                           toast=lambda message, **kw: toasts.append(message), toasts=toasts,
                           go=lambda *a: None, open_url=lambda url: None, open_game=lambda pkg: None)


def test_room_model_loads_once_and_reuses_the_cache(tmp_path):
    from frameport.core.paths import user_data_dir
    from frameport.ui import room_state as RS

    frame = MeshFrame(tmp_path)
    app = sync_app(frame)
    st = RS.state(app, model=True)
    model = st["model"]
    assert model is not None and st["model_error"] is None
    assert model["scene"].triangles == 2 and model["scene"].lods and frame.opens == 1
    assert (user_data_dir() / "rooms-cache" / "steamframe").is_dir()
    # a new connection (same Frame, same room): read from the cache, not the Frame
    app.target = SimpleNamespace(frame=frame, label="steamframe")
    st = RS.state(app, model=True)
    assert st["model"]["key"] == model["key"] and frame.opens == 1
    # a new scan: fetched again
    frame.room.time += 60
    st = RS.state(app, force=True)
    assert st["model"]["key"] != model["key"] and frame.opens == 2


def test_room_tab_builds_in_every_state(tmp_path):
    from frameport.ui import room_state as RS
    from frameport.ui.views.room import RoomView

    app = sync_app(MeshFrame(tmp_path))
    view = RoomView(app)
    root = view.mount()
    assert view.root is root and view.canvas.scene is not None
    assert not view.placeholder.visible and not view.full_btn.disabled
    assert view.summary.value.startswith("4 walls") and "mesh triangles" in view.source.value
    cached = app._room_state
    cached.update(working="capture", scan={"state": "recording", "directions": 6, "looked_up": 7})
    view.refresh(update=False)
    assert view.cancel_btn.visible and not view.scan_btn.visible and view.scan_bar.value == 0.5
    assert "6 of 12" in view.scan_text.value
    cached.update(working=False, scan=None, status=dict(room=None, default_circle=True), model=None)
    view.refresh(update=False)
    assert view.placeholder.visible and view.place_actions.visible and view.boundary_btn.disabled
    app.frame_state = "none"
    assert RoomView(app).mount() is not None  # not connected: the empty state
    assert RS.state(app)["target"] is None


def test_room_tab_full_view_server_stops(tmp_path):
    from frameport.ui.views.room import RoomView

    opened = []
    app = sync_app(MeshFrame(tmp_path))
    app.open_url = opened.append
    view = RoomView(app)
    view.mount()
    view.open_full()
    assert opened and opened[0].startswith("http://127.0.0.1:") and view.viewer.running
    server = view.viewer
    view.stop()
    assert view.viewer is None and server.stopped


def test_cancel_goes_to_the_agent(tmp_path, monkeypatch):
    from frameport.install import rooms
    from frameport.ui import room_state as RS

    frame = MeshFrame(tmp_path)
    app = sync_app(frame)
    RS.state(app)
    RS.operate(app, "cancel")  # nothing running: nothing sent
    assert "room_capture_stop" not in frame.calls
    app._room_state["scan"] = {"state": "recording"}
    RS.operate(app, "cancel")
    assert "room_capture_stop" in frame.calls

    def cancelled(frame, progress=None):
        raise rooms.ScanCancelled("cancelled")
    monkeypatch.setattr(rooms, "scan", cancelled)
    RS.operate(app, "capture")
    assert app.toasts[-1].startswith("Scan cancelled") and not app._room_state["working"]


def test_scan_line_wording():
    from frameport.ui.room_state import scan_line

    assert scan_line(None) == ""
    assert "passthrough" in scan_line({"state": "waiting"})
    assert scan_line({"state": "recording", "directions": 4, "looked_up": 6, "looked_down": 6}).endswith(
        "ceiling and floor seen")


def test_room_canvas_draws_whole_pixel_layers():
    import flet.canvas as cv

    from frameport.mr import view3d
    from frameport.mr.room import Volume
    from frameport.ui.room3d import RoomCanvas

    room = box_room(4, 3)
    room.volumes.append(Volume("1" * 32, "TABLE", 0, 0, 0.7, 0, 1, 1, 0.7))
    rc = RoomCanvas(None)
    rc.set_scene(view3d.prepare(room))
    assert rc.canvas.shapes == []  # no size yet: nothing to draw
    rc.width = rc.cam.width = 800.0
    rc.height = rc.cam.height = 480.0
    rc.fit()
    shapes = rc.canvas.shapes
    assert shapes and rc.stats["lod"] == "rest" and rc.stats["segments"] > 0
    points = [p for s in shapes if isinstance(s, cv.Points) for p in s.points]
    assert points and all(isinstance(p.x, int) and isinstance(p.y, int) for p in points)
    old = list(shapes)
    rc.cam.orbit(30, 0)
    rc.draw("drag")
    assert all(s not in old for s in rc.canvas.shapes)  # new shapes each redraw (a small, cheap patch)
    rc.set_scene(None)
    assert rc.canvas.shapes == []
