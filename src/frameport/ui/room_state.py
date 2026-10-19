"""The connected Frame's room, shared by the Frame page's room card and the Room tab: one cache per connected Frame,
filled on background workers (SSH, previews, the mesh download and its 3D preparation never run in a render path).

state(app) -> dict with: target, status (agent room_status), preview (a top-down SVG in the assets dir), error,
loading, working (an action runs), scan (the newest scan status while scanning), and, once the Room tab asked for
it (model=True): model (dict room / room_text / mesh / scene / dir), model_loading, model_error."""
from __future__ import annotations

import hashlib
import re
import shutil
import time

from ..errors import explain
from ..i18n import tr

REFRESH_SECONDS = 30
CACHE_KEEP = 3  # rooms kept per Frame in <data>/rooms-cache


def state(app, force: bool = False, model: bool = False) -> dict:
    target = app.target if app.frame_state == "connected" else None
    cached = getattr(app, "_room_state", None)
    if cached is None or cached["target"] is not target:
        cached = app._room_state = dict(target=target, status=None, preview=None, error=None, loading=False, time=0,
                                        model=None, model_loading=False, model_error=None, want_model=False)
    if model and not cached.get("want_model"):
        cached["want_model"] = True
        _maybe_load_model(app, cached)
    if target is None or cached["loading"] or (not force and time.monotonic() - cached["time"] < REFRESH_SECONDS):
        return cached
    cached["loading"] = True

    def fetch():
        from ..core.paths import user_data_dir
        from ..install import rooms
        from ..mr.room import parse, preview_svg

        try:
            status = rooms.status(target.frame)
            preview = None
            if status.get("room_text"):
                svg = preview_svg(parse(status["room_text"]))
                name = hashlib.sha256(svg.encode()).hexdigest()[:24] + ".svg"
                path = user_data_dir() / "room-previews" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(svg, encoding="utf-8")
                preview = "room-previews/" + name
            cached.update(status=status, preview=preview, error=None)
        except Exception as exc:  # noqa: BLE001 - a failed room query must not hide the rest of the page
            cached.update(status=None, preview=None, error=explain(exc))
        finally:
            cached.update(loading=False, time=time.monotonic())
        if app.target is target and app.frame_state == "connected":
            _maybe_load_model(app, cached)
            app.refresh_view()

    app.run_bg(fetch)
    return cached


def model_key(status: dict | None) -> tuple | None:
    """What identifies the stored room's 3D model (a new scan, a deletion or a new play area changes it)."""
    text = (status or {}).get("room_text")
    if not text:
        return None
    from ..mr.room import parse

    room = parse(text)
    return room.universe, room.time, room.uuid, room.mesh.uuid if room.mesh else None


def _cache_root(target):
    from ..core.paths import user_data_dir

    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(getattr(target, "label", "") or "frame"))[:64] or "frame"
    return user_data_dir() / "rooms-cache" / name


def _maybe_load_model(app, cached: dict) -> None:
    """Fetch + prepare the room's 3D model in the background when the Room tab wants it and the room changed."""
    if not cached.get("want_model") or cached.get("model_loading") or cached["target"] is None:
        return
    try:
        key = model_key(cached.get("status"))
    except ValueError as exc:
        cached.update(model=None, model_error=str(exc))
        return
    current = cached.get("model")
    if key is None:
        if current is not None:
            cached["model"] = None
        return
    if current is not None and current.get("key") == key:
        return
    cached.update(model_loading=True, model_error=None)
    target, text = cached["target"], cached["status"]["room_text"]

    def work():
        try:
            cached["model"] = load_model(target, text, key)
        except Exception as exc:  # noqa: BLE001 - the tab shows the reason, the rest of the tab still works
            cached["model_error"] = explain(exc)
        finally:
            cached["model_loading"] = False
        if app.target is target:
            app.refresh_view()

    app.run_bg(work)


def load_model(target, room_text: str, key: tuple) -> dict:
    """The room file + mesh (from <data>/rooms-cache/<frame>/ when this room was fetched before, else exported from
    the Frame), parsed and prepared for the canvas."""
    from ..install import rooms
    from ..mr import view3d
    from ..mr.room import parse, read_mesh

    room = parse(room_text)
    root = _cache_root(target)
    folder = root / f"{room.universe}-{room.time}-{room.uuid[:8]}"
    mesh_bytes = None
    path = folder / "room.txt"
    fresh = path.is_file() and (room.mesh is None or (folder / room.mesh.filename).is_file())
    if fresh:
        try:
            cached_room = parse(path.read_text(encoding="utf-8"))
            fresh = cached_room.uuid == room.uuid and (cached_room.mesh and cached_room.mesh.uuid) == (
                room.mesh and room.mesh.uuid)
        except (OSError, ValueError):
            fresh = False
    if not fresh:
        tmp = root / (folder.name + ".part")
        shutil.rmtree(tmp, ignore_errors=True)
        rooms.export(target.frame, tmp)
        shutil.rmtree(folder, ignore_errors=True)
        tmp.rename(folder)
        _prune(root, keep=folder)
    room = parse(path.read_text(encoding="utf-8"))
    mesh = None
    if room.mesh:
        mesh_bytes = (folder / room.mesh.filename).read_bytes()
        mesh = read_mesh(mesh_bytes)
    scene = view3d.prepare(room, mesh)
    return dict(key=key, room=room, room_text=path.read_text(encoding="utf-8"), mesh=mesh_bytes, scene=scene,
                dir=folder)


def _prune(root, keep) -> None:
    try:
        old = sorted((p for p in root.iterdir() if p.is_dir() and p != keep), key=lambda p: p.stat().st_mtime)
    except OSError:
        return
    for p in old[:max(0, len(old) - (CACHE_KEEP - 1))]:
        shutil.rmtree(p, ignore_errors=True)


def scan_line(scan: dict | None) -> str:
    """One plain sentence about a running scan."""
    if not scan:
        return ""
    up = scan.get("looked_up", 0) >= 5
    down = scan.get("looked_down", 0) >= 5
    return {"waiting": tr("Put on the headset and turn on passthrough. The scan starts by itself."),
            "recording": tr("Scanning: {directions} of 12 directions seen{up}").format(
                directions=scan.get("directions", 0),
                up=(tr(", ceiling and floor seen") if up and down else tr(", ceiling seen") if up
                    else tr(", floor seen") if down else "")),
            "building": tr("Building the room…")}.get(scan.get("state"), "")


def operate(app, action: str) -> None:
    """capture (scan in the headset) / boundary (use the play area) / delete / cancel (a running scan), on a background
    worker; the cache refreshes afterwards."""
    cached = state(app)
    target = app.target
    if target is None:
        return
    if action == "cancel":
        if not cached.get("scan"):
            return
        cached["cancelling"] = True
        app.refresh_view()

        def cancel():
            from ..install import rooms
            try:
                rooms.cancel_scan(target.frame)
            except Exception as exc:  # noqa: BLE001
                cached["cancelling"] = False
                app.toast(explain(exc), error=True)
                app.refresh_view()
        app.run_bg(cancel)
        return
    if cached["loading"] or cached.get("working"):
        return
    cached["working"] = action
    app.refresh_view()

    def scan_progress(st):  # every agent status while the player scans in the headset
        cached["scan"] = st
        if app.target is target:
            app.refresh_view()

    def work():
        from ..install import rooms
        try:
            if action == "capture":
                cached["scan"] = {"state": "waiting"}
                rooms.scan(target.frame, progress=scan_progress)
                app.toast(tr("Room scanned"))
            else:
                {"boundary": rooms.from_boundary, "delete": rooms.delete}[action](target.frame)
        except rooms.ScanCancelled:
            app.toast(tr("Scan cancelled. Your saved room hasn't changed."))
        except Exception as exc:  # noqa: BLE001
            app.toast(explain(exc), error=True)
        finally:
            cached.update(working=False, scan=None, cancelling=False)
        if app.target is target:
            state(app, force=True)
            app.refresh_view()
    app.run_bg(work)
