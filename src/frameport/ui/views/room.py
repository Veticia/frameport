"""Room: the connected Frame's saved room for mixed-reality games. Scan it in the headset (with progress and Cancel),
use the play area instead, delete it, and look at it in 3D: in the window (ui/room3d.RoomCanvas) and, with every
detail, in the browser (mr/room_viewer.py, a local page stopped on disconnect / window close).

Persistent like the Monitor (built once per connection, re-mounted on every visit): state changes (the room cache in
ui/room_state.py, scan progress every 2 s) update properties of the controls built here; buttons are never rebuilt."""
from __future__ import annotations

from typing import TYPE_CHECKING

import flet as ft

from ...core import library
from ...i18n import tr
from ...mr import TESTER_MANIFEST, TESTER_PACKAGE
from .. import components as C
from .. import room_state as RS
from .. import theme as T
from ..help import HELP

if TYPE_CHECKING:
    from ..app import FramePortApp

CANVAS_HEIGHT = 480


def furniture_name(label: str) -> str:
    """A room label in plain words (literal tr() calls so scripts/i18n_extract.py finds them)."""
    return {"TABLE": tr("Table"), "COUCH": tr("Couch"), "BED": tr("Bed"), "STORAGE": tr("Storage"),
            "SCREEN": tr("Screen"), "LAMP": tr("Lamp"), "PLANT": tr("Plant"), "DOOR_FRAME": tr("Door"),
            "WINDOW_FRAME": tr("Window"), "WALL_ART": tr("Wall art"), "OTHER": tr("Other")}.get(
        label, label.replace("_", " ").capitalize())


def room_summary(summary: dict | None) -> str:
    s = summary or {}
    return tr("{walls} walls · {area:.1f} m² · {height:.1f} m high").format(
        walls=s.get("walls", 0), area=s.get("floor_area", 0), height=s.get("ceiling_height", 0))


class RoomView:
    def __init__(self, app: FramePortApp):
        self.app = app
        self.root: ft.Control | None = None
        self.canvas = None              # room3d.RoomCanvas
        self.viewer = None              # mr.room_viewer.RoomViewerServer, while the browser view was opened
        self._push = None               # components.LoopUpdater (refresh() runs on background threads)
        self._shown_key = "unset"       # the model on the canvas
        self._status_seen = None        # the status object the checklist shows

    # ---------------------------------------------------------------- building
    def _heading(self, actions: list[ft.Control] | None = None) -> ft.Control:
        pill = C.pill(tr("experimental"), T.WARN, tooltip=C.tip(HELP["room_experimental"]))
        head = ft.Column([ft.Row([C.title(tr("Room")), pill], spacing=T.S3,
                                 vertical_alignment=ft.CrossAxisAlignment.CENTER),
                          C.body(tr("The room mixed-reality games see: walls, furniture and the scanned surfaces"))],
                         spacing=T.px(2), expand=1)
        if not actions:
            return head
        return ft.Row([head, ft.Container(ft.Row(actions, spacing=T.S3, run_spacing=T.S2, wrap=True,
                                                 alignment=ft.MainAxisAlignment.END,
                                                 vertical_alignment=ft.CrossAxisAlignment.CENTER), expand=1)],
                      vertical_alignment=ft.CrossAxisAlignment.CENTER, spacing=T.S3)

    def _tester(self) -> ft.Control:
        """Test Quest MR: Get (its install link: FramePort's own install question) or Open once it's in the library."""
        if library.game(TESTER_PACKAGE):
            button = C.secondary(tr("Open Test Quest MR"), ft.Icons.VIEW_IN_AR_OUTLINED,
                                 lambda e: self.app.open_game(TESTER_PACKAGE), tooltip=C.tip(HELP["room_tester"]))
        else:
            from .link_dialog import open_link

            button = C.secondary(tr("Get Test Quest MR"), ft.Icons.VIEW_IN_AR_OUTLINED,
                                 lambda e: open_link(self.app, TESTER_MANIFEST), tooltip=C.tip(HELP["room_tester"]))
        return C.callout(ft.Column([
            C.body(tr("Try it out: Test Quest MR shows the room, furniture and anchors a game receives from this "
                      "scan."), T.TEXT),
            button], spacing=T.S3, tight=True), "info", ft.Icons.LIGHTBULB_OUTLINE_ROUNDED)

    def _build(self) -> None:
        from ..room3d import RoomCanvas

        app = self.app
        op = lambda action: (lambda e: RS.operate(app, action))  # noqa: E731
        self._push = C.LoopUpdater(app.page)
        self.canvas = RoomCanvas(app.page, height=CANVAS_HEIGHT)
        self.refresh_btn = C.ghost(tr("Refresh"), ft.Icons.REFRESH_ROUNDED, lambda e: RS.state(app, force=True))
        self.full_btn = C.secondary(tr("Open full 3D view"), ft.Icons.OPEN_IN_NEW_ROUNDED, self.open_full,
                                    tooltip=C.tip(tr("Every detail of the scan, in your browser.")))
        # the canvas, with what's missing in its place while there's nothing to draw
        self.place_icon = ft.Container(C.spinner(), visible=True)
        self.place_title = ft.Text("", size=T.px(16), weight=ft.FontWeight.W_600, color=T.TEXT,
                                   text_align=ft.TextAlign.CENTER)
        self.place_text = C.body("", text_align=ft.TextAlign.CENTER)
        self.place_scan = C.primary(tr("Scan room in the headset"), ft.Icons.VIEW_IN_AR_ROUNDED, op("capture"))
        self.place_boundary = C.secondary(tr("Use my play area"), ft.Icons.CROP_FREE_ROUNDED, op("boundary"))
        self.place_actions = ft.Row([self.place_scan, self.place_boundary], spacing=T.S3, wrap=True,
                                    alignment=ft.MainAxisAlignment.CENTER, visible=False)
        self.placeholder = ft.Container(
            ft.Column([self.place_icon, self.place_title, ft.Container(self.place_text, width=T.px(420)),
                       self.place_actions], spacing=T.S3, tight=True,
                      horizontal_alignment=ft.CrossAxisAlignment.CENTER),
            alignment=ft.Alignment.CENTER, left=0, right=0, top=0, bottom=0, bgcolor=T.soft(T.BG, 0.88))
        self.reset_btn = C.icon_btn(ft.Icons.CENTER_FOCUS_STRONG_OUTLINED, tr("Show the whole room"),
                                    lambda e: self.canvas.fit())
        view = ft.Stack([self.canvas.control, self.placeholder,
                         ft.Container(self.reset_btn, right=T.S2, top=T.S2)], height=T.px(CANVAS_HEIGHT))
        self.canvas_card = C.card(ft.Column([
            view,
            ft.Row([ft.Container(self.canvas.legend(), expand=True), C.help_icon("room_view")],
                   vertical_alignment=ft.CrossAxisAlignment.CENTER),
        ], spacing=T.S3), padding=T.S3, col={"xs": 12, "lg": 8})

        # status
        self.checks = ft.Container(C.checklist([C.Check(None, tr("Checking the room…"))]))
        self.error = C.callout("", "warn")
        self.error.visible = False
        # scanning
        self.scan_text = C.body("", T.TEXT)
        self.scan_bar = C.progress_bar(None)
        self.scan_box = ft.Container(ft.Column([self.scan_text, self.scan_bar], spacing=T.S2, tight=True),
                                     bgcolor=T.soft(T.INFO, 0.08), border=ft.Border.all(1, T.soft(T.INFO, 0.35)),
                                     border_radius=T.RADIUS_SM, padding=T.S3, visible=False)
        self.scan_btn = C.primary(tr("Scan room in the headset"), ft.Icons.VIEW_IN_AR_ROUNDED, op("capture"),
                                  tooltip=C.tip(tr("Put on the headset: the scan starts when passthrough is on and "
                                                   "takes about a minute of looking around.")))
        self.cancel_btn = C.secondary(tr("Cancel scan"), ft.Icons.CLOSE_ROUNDED, op("cancel"))
        self.cancel_btn.visible = False
        self.boundary_btn = C.secondary(tr("Use my play area"), ft.Icons.CROP_FREE_ROUNDED, op("boundary"))
        self.delete_btn = C.ghost(tr("Delete room"), ft.Icons.DELETE_OUTLINE_ROUNDED, self._ask_delete)
        # what's in it
        self.summary = C.body("", T.TEXT)
        self.source = C.meta("")
        self.furniture = ft.Row([], spacing=T.S2, run_spacing=T.S2, wrap=True)
        side = ft.Column([
            C.card(ft.Column([self.checks, self.error], spacing=T.S2),
                   padding=ft.Padding(T.S4, T.S2, T.S4, T.S2)),
            C.card(ft.Column([C.section(tr("Scan"), self.scan_box,
                                        ft.Row([self.scan_btn, self.cancel_btn], spacing=T.S2, wrap=True),
                                        ft.Row([self.boundary_btn, self.delete_btn], spacing=T.S2, wrap=True),
                                        help="mr_room")], spacing=T.S3), padding=T.S4),
            C.card(ft.Column([C.meta(tr("In this room")), self.summary, self.source, self.furniture],
                             spacing=T.S2), padding=T.S4),
            self._tester(),
        ], spacing=T.S3, col={"xs": 12, "lg": 4}, horizontal_alignment=ft.CrossAxisAlignment.STRETCH)
        self.root = ft.Column([
            self._heading([self.refresh_btn, self.full_btn]),
            ft.ResponsiveRow([self.canvas_card, side], spacing=T.S4, run_spacing=T.S4,
                             vertical_alignment=ft.CrossAxisAlignment.START),
            ft.Container(height=T.S4),
        ], spacing=T.S4, scroll=ft.ScrollMode.AUTO, expand=True)

    def mount(self) -> ft.Control:
        app = self.app
        if not (app.target and app.frame_state == "connected"):
            return ft.Column([
                self._heading(),
                C.empty_state(ft.Icons.VIEW_IN_AR_ROUNDED, tr("Your room in 3D"),
                              tr("Connect to your Steam Frame to scan your room for mixed-reality games and look at "
                                 "it from every side."),
                              C.primary(tr("Connect"), ft.Icons.LINK_ROUNDED, lambda e: app.go("frame")),
                              preview=C.ghost_preview("room"))], expand=True)
        if self.root is None:
            self._build()
        self.refresh(update=False)
        return self.root

    # ---------------------------------------------------------------- state -> controls
    def refresh(self, update: bool = True) -> None:
        """Bring the controls up to date with the room cache (any thread)."""
        if self.root is None:
            return
        app = self.app
        cached = RS.state(app, model=True)
        status = cached.get("status") or {}
        room = status.get("room")
        scan = cached.get("scan")
        working = cached.get("working")
        busy = bool(cached["loading"] and not status) or bool(working)
        changed = [self.scan_box, self.scan_btn, self.cancel_btn, self.boundary_btn, self.delete_btn, self.error,
                   self.refresh_btn, self.full_btn, self.placeholder, self.summary, self.source, self.furniture,
                   self.place_scan, self.place_boundary]
        # checklist: rebuilt only when the status changed (it has no buttons; scan ticks don't touch it)
        if cached.get("status") is not self._status_seen or not status:
            self._status_seen = cached.get("status")
            self.checks.content = C.checklist(self._checks(cached, status, room))
            changed.append(self.checks)
        self.error.visible = bool(cached.get("error") or cached.get("model_error"))
        if self.error.visible:
            text = cached.get("error") or cached.get("model_error")
            self.error.content.controls[1].content.value = tr("Couldn't load the room: {error}").format(error=text)
        # scan progress
        line = RS.scan_line(scan)
        self.scan_box.visible = bool(working == "capture" or line)
        self.scan_text.value = (tr("Cancelling the scan…") if cached.get("cancelling") else line
                                or tr("Starting the scan…"))
        state = (scan or {}).get("state")
        self.scan_bar.value = (min(1.0, (scan or {}).get("directions", 0) / 12) if state == "recording" else None)
        scanning = working == "capture"
        self.scan_btn.visible, self.cancel_btn.visible = not scanning, scanning
        self.cancel_btn.disabled = bool(cached.get("cancelling")) or not scan
        default = status.get("default_circle")
        for b in (self.boundary_btn, self.place_boundary):
            b.disabled = busy or default is not False
            b.tooltip = C.tip(tr("Use your SteamVR play area as walls, without furniture.") if default is False else
                              tr("Set up a play area in SteamVR first. Valve's default circle does not describe your "
                                 "room."))
        self.scan_btn.disabled = self.place_scan.disabled = busy
        self.delete_btn.disabled = busy or not room
        self.refresh_btn.disabled = bool(cached["loading"] or working)
        model = cached.get("model")
        self.full_btn.disabled = model is None
        # summary
        summ = (room or {}).get("summary") or {}
        if room:
            self.summary.value = room_summary(summ)
            self.source.value = {"scan": tr("Scanned in the headset"), "boundary": tr("Walls from your play area"),
                                 "box": tr("A simple box room")}.get(room.get("source"), tr("Imported room"))
            if model is not None and model["scene"].triangles:
                self.source.value += " · " + tr("{n} mesh triangles").format(n=f"{model['scene'].triangles:,}")
            counts = summ.get("volumes") or {}
            self.furniture.controls = [C.pill(f"{furniture_name(k)} × {n}" if n > 1 else furniture_name(k),
                                              T.ACCENT) for k, n in sorted(counts.items())] or \
                [C.meta(tr("No furniture found"))]
        else:
            self.summary.value = tr("No room saved yet") if status else ""
            self.source.value = ""
            self.furniture.controls = []
        # the canvas, or what's in its place
        key = model.get("key") if model else None
        if key != self._shown_key:
            self._shown_key = key
            self.canvas.set_scene(model["scene"] if model else None)
            if self.viewer is not None and model is not None:
                self.viewer.set_room(model["room_text"], model["mesh"])  # the open page reloads by itself
        self._placeholder(cached, status, room, model)
        if update:
            self._push(*changed)

    def _checks(self, cached: dict, status: dict, room: dict | None) -> list:
        if cached["loading"] and not status:
            return [C.Check(None, tr("Checking the room…"))]
        if not status:
            return [C.Check(False, tr("Room data not available"), "", "mr_room")]
        scanned = bool(room and room.get("source") == "scan")
        return [C.Check(True if scanned else "warn", tr("Room scanned"),
                        tr("Room scan saved") if scanned else
                        tr("Using your play area; furniture is not included") if room else tr("No room saved yet"),
                        "mr_room"),
                C.Check(True if status.get("aligned") else "warn", tr("Lines up with the current play area"),
                        tr("Same play area") if status.get("aligned")
                        else tr("Set up a room in your current play area"),
                        "mr_alignment"),
                C.Check(bool(status.get("scanner_installed")), tr("Scanner installed"),
                        tr("Ready") if status.get("scanner_installed") else tr("Installed with the first scan"),
                        "mr_scanner")]

    def _placeholder(self, cached: dict, status: dict, room: dict | None, model: dict | None) -> None:
        show, spin, title, text, actions = True, False, "", "", False
        if cached["loading"] and not status:
            spin, title = True, tr("Checking the room on your Frame…")
        elif not status:
            title = tr("Couldn't check the room")
            text = tr("Try Refresh. The Frame needs to be on and connected.")
        elif not room:
            title = tr("No room saved yet")
            text = tr("Scan your room in the headset: look around for about a minute with passthrough on. Or use "
                      "your SteamVR play area for walls without furniture.")
            actions = True
        elif cached.get("model_loading") or (model is None and not cached.get("model_error")):
            spin, title = True, tr("Loading the room in 3D…")
        elif model is None:
            title = tr("Couldn't show the room in 3D")
        else:
            show = False
        self.placeholder.visible = show
        self.place_icon.visible = spin
        self.place_title.value, self.place_text.value = title, text
        self.place_text.visible = bool(text)
        self.place_actions.visible = actions and not cached.get("working")

    # ---------------------------------------------------------------- actions
    def _ask_delete(self, e) -> None:
        C.confirm(self.app.page, tr("Delete room?"), tr("Games will use a simple room until you save another room."),
                  tr("Delete room"), lambda: RS.operate(self.app, "delete"), danger=True)

    def open_full(self, e=None) -> None:
        """The browser view: a local page with the whole mesh (started once, kept until disconnect / close)."""
        cached = RS.state(self.app, model=True)
        model = cached.get("model")
        if model is None:
            self.app.toast(tr("The room is still loading."))
            return
        from ...mr.room_viewer import RoomViewerServer

        if self.viewer is None or not self.viewer.running:
            self.viewer = RoomViewerServer().start()
        self.viewer.set_room(model["room_text"], model["mesh"])
        self.app.open_url(self.viewer.url)

    def stop(self) -> None:
        """Disconnect / window close: the browser page's server ends."""
        if self.viewer is not None:
            self.viewer.stop()
            self.viewer = None
