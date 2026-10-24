"""FramePort's own menu (GitHub #414): drawn in page.overlay instead of Flet's stock popup menu, which can't nest,
caps its width at 280 logical px whatever the UI scale, and let long labels run past its edge.

One PopMenu per app (`app.popmenu`). `show(entries, at=(x, y))` opens it at a right-click point (window
coordinates); `show(entries, below=(left, top, right, bottom))` under a button. Entries are menus.py's: (label, icon,
handler) | menus.Item | menus.Submenu | menus.Header | None. Submenus open to the side on hover (after a short
delay, so a diagonal move to the submenu doesn't switch it), on click, or with → on the keyboard; ↑ ↓ move,
Enter/Space choose, ← / Esc close a level. It closes on a click outside, Esc, a chosen item or a view change
(app.go). Hovering only changes row colours (properties): no control is rebuilt while the menu is open, except a
submenu's panel when it opens.

Layout is computed here (layout/place/place_beside are pure and tested): panels size to their longest label
(estimated from the text: Flet can't measure) between MIN_W and MAX_W, longer labels wrap; a panel opens at the
point and flips left/up when there's no room, then is clamped into the window."""
from __future__ import annotations

import math
from collections.abc import Callable

import flet as ft

from . import components as C
from . import theme as T
from .menus import Header, Item, Submenu

MIN_W, MAX_W = 168, 380      # panel width limits (at 100 %)
MARGIN = 8                   # kept free between a panel and the window's edge
ROW_H, HEAD_H, SEP_H = 32, 26, 9
PAD_Y = 5                    # panel padding above the first / below the last row
PAD_X = 12                   # a row's inner padding (each side)
INSET = 4                    # a row's highlight inset from the panel's sides
GAP = 10                     # icon ↔ label
OPEN_DELAY, CLOSE_DELAY = 0.12, 0.3  # hover intent (seconds)

# rough glyph widths in em for the UI font (Flet can't measure text): generous, so a label estimated to fit really
# fits; a long label then wraps a little earlier than it would have to
_NARROW = set("iljtfrI.,;:'|!()[] ·")
_WIDE = set("mwMW@%…")


def text_width(text: str, size: float) -> float:
    """Estimated width of `text` at font `size` (px)."""
    em = 0.0
    for ch in text:
        em += (0.32 if ch in _NARROW else 0.86 if ch in _WIDE else 0.66 if ch.isupper() else
               0.58 if ch.isdigit() else 0.55)
    return em * size * 1.04


def _px(v: float) -> float:
    return T.px(v)


def entries_of(entries: list) -> list:
    """The entries as shown: C.menu_entries' trimming (no empty headers, no leading/trailing/double dividers)."""
    return C.menu_entries(entries)


def kind(e) -> str:
    return ("sep" if e is None else "head" if isinstance(e, Header) else "sub" if isinstance(e, Submenu)
            else "item")


def handler_of(e) -> Callable | None:
    return None if e is None or isinstance(e, (Header, Submenu)) else e[2]


def layout(entries: list) -> tuple[float, list[tuple[float, float, int]], float]:
    """(width, [(top, height, lines) per entry], height) of a panel showing `entries` (already trimmed)."""
    size = T.T_BODY
    chrome = 2 * _px(INSET) + 2 * _px(PAD_X) + T.ICON_S + _px(GAP)
    sub_extra = T.ICON_S + _px(GAP)
    want = 0.0
    for e in entries:
        k = kind(e)
        if k in ("item", "sub"):
            want = max(want, chrome + text_width(e[0], size) + (sub_extra if k == "sub" else 0))
        elif k == "head":
            want = max(want, 2 * _px(INSET) + 2 * _px(PAD_X) + text_width(e.label.upper(), T.T_SMALL) * 1.15)
    width = max(_px(MIN_W), min(_px(MAX_W), math.ceil(want) + 1))
    rows, y = [], _px(PAD_Y)
    line_h = size * 1.4
    for e in entries:
        k = kind(e)
        if k == "sep":
            h, lines = _px(SEP_H), 0
        elif k == "head":
            h, lines = _px(HEAD_H), 1
        else:
            room = width - chrome - (sub_extra if k == "sub" else 0)
            lines = max(1, math.ceil(text_width(e[0], size) / max(room, 1)))
            h = _px(ROW_H) if lines == 1 else lines * line_h + 2 * _px(7)
        rows.append((y, h, lines))
        y += h
    return width, rows, y + _px(PAD_Y)


def place(x: float, y: float, w: float, h: float, win_w: float, win_h: float,
          flip_x: float | None = None, flip_y: float | None = None) -> tuple[float, float]:
    """Top-left of a panel (w, h) that opens at (x, y). No room on the right: it ends at flip_x (default x); no
    room below: it ends at flip_y (default y). Then it's kept inside the window."""
    m = _px(MARGIN)
    if x + w > win_w - m:
        x = (x if flip_x is None else flip_x) - w
    if y + h > win_h - m:
        up = (y if flip_y is None else flip_y) - h
        y = up if up >= m else y  # no room above either: stay below and be clamped
    x = max(m, min(x, win_w - w - m))
    y = max(m, min(y, win_h - h - m))
    return x, y


def place_beside(left: float, width: float, row_top: float, w: float, h: float, win_w: float,
                 win_h: float) -> tuple[float, float, str]:
    """A submenu next to its parent panel (left, width), its first row level with the parent's row: on the right
    when it fits, else on the left, else the side with more room (clamped). Returns (x, y, side)."""
    m = _px(MARGIN)
    overlap = _px(2)
    right, left_x = left + width - overlap, left - w + overlap
    if right + w <= win_w - m:
        x, side = right, "right"
    elif left_x >= m:
        x, side = left_x, "left"
    else:
        x, side = (right, "right") if win_w - (left + width) >= left else (left_x, "left")
    y = row_top - _px(PAD_Y)
    x = max(m, min(x, win_w - w - m))
    y = max(m, min(y, win_h - h - m))
    return x, y, side


class Panel:
    """One open panel: its entries, geometry, row controls and highlighted row."""

    def __init__(self, entries: list, parent: Panel | None = None, parent_row: int | None = None):
        self.entries = entries_of(entries)
        self.parent, self.parent_row = parent, parent_row
        self.child: Panel | None = None
        self.width, self.rows, self.height = layout(self.entries)
        self.left = self.top = 0.0
        self.hi: int | None = None
        self.controls: list[ft.Container | None] = []
        self.control: ft.Container | None = None

    def actionable(self) -> list[int]:
        """Rows ↑ ↓ can reach: enabled items and submenus."""
        return [i for i, e in enumerate(self.entries) if kind(e) == "sub" or (kind(e) == "item" and handler_of(e))]


class PopMenu:
    def __init__(self, app):
        self.app = app
        self.root: Panel | None = None
        self.layer: ft.Stack | None = None
        self._gen = 0  # bumped by every hover-intent decision: a pending open/close with an older number is dropped

    # ------------------------------------------------------------------ public
    @property
    def is_open(self) -> bool:
        return self.root is not None

    def show(self, entries: list, at=None, below: tuple | None = None) -> None:
        """Open at a point `at` ((x, y) or an Offset) or under a box `below` = (left, top, right, bottom)."""
        self.close(update=False)
        panel = Panel(entries)
        if not panel.entries:
            return
        win_w, win_h = self._window()
        if below is not None:
            left, t, r, b = below
            panel.left, panel.top = place(left, b + _px(4), panel.width, panel.height, win_w, win_h,
                                          flip_x=r, flip_y=t - _px(4))
        else:
            x, y = (at.x, at.y) if hasattr(at, "x") else (at or (win_w / 2, win_h / 3))
            panel.left, panel.top = place(x, y, panel.width, panel.height, win_w, win_h)
        self.root = panel
        self._build(panel)
        # a click anywhere outside closes it (the click doesn't go on to what's below, as in other apps' menus)
        barrier = ft.GestureDetector(content=ft.Container(bgcolor="#00000000", width=win_w, height=win_h),
                                     on_tap_down=lambda e: self.close(),
                                     on_secondary_tap_down=lambda e: self.close())
        self.layer = ft.Stack([barrier, panel.control], width=win_w, height=win_h)
        page = self.app.page
        page.overlay.append(self.layer)
        page.update()
        self._fade_in(panel)

    def close(self, update: bool = True) -> None:
        self._gen += 1
        layer, self.layer, self.root = self.layer, None, None
        page = self.app.page
        if layer is not None and layer in page.overlay:
            page.overlay.remove(layer)
            if update:
                page.update()

    def row_center(self, label: str) -> tuple[float, float] | None:
        """Window position of the row with this label in an open panel (smoke tests hover/click it)."""
        p = self.root
        while p is not None:
            for i, e in enumerate(p.entries):
                if kind(e) in ("item", "sub") and e[0] == label:
                    top, h, _ = p.rows[i]
                    return p.left + p.width / 2, p.top + top + h / 2
            p = p.child
        return None

    def handle_key(self, key: str) -> bool:
        """A key while the menu is open (app._on_key asks first). Every key is taken while it's open (typing must
        not reach the Library's search behind it)."""
        if not self.is_open:
            return False
        k = (key or "").replace(" ", "").lower()
        p = self.deepest()
        if k == "escape":
            if p.parent is not None:
                self._close_child(p.parent)
            else:
                self.close()
        elif k in ("arrowdown", "arrowup", "tab"):
            rows = p.actionable()
            if rows:
                step = -1 if k == "arrowup" else 1
                if p.hi in rows:
                    nxt = rows[(rows.index(p.hi) + step) % len(rows)]
                else:
                    nxt = rows[0] if step > 0 else rows[-1]
                self._highlight(p, nxt)
        elif k == "arrowright":
            if p.hi is not None and kind(p.entries[p.hi]) == "sub":
                self._open_child(p, p.hi, focus=True)
        elif k == "arrowleft":
            if p.parent is not None:
                self._close_child(p.parent)
        elif k in ("enter", "numpadenter", "space", ""):
            if p.hi is not None:
                self._activate(p, p.hi, None)
        return True

    def deepest(self) -> Panel:
        p = self.root
        while p.child is not None:
            p = p.child
        return p

    # ------------------------------------------------------------------ building
    def _window(self) -> tuple[float, float]:
        page = self.app.page
        return float(getattr(page, "width", None) or 1280), float(getattr(page, "height", None) or 820)

    def _build(self, panel: Panel) -> None:
        controls: list = []
        for i, e in enumerate(panel.entries):
            k = kind(e)
            top, h, lines = panel.rows[i]
            if k == "sep":
                controls.append(ft.Container(height=1, bgcolor=T.BORDER,
                                             margin=ft.Margin(0, (h - 1) / 2, 0, (h - 1) / 2)))
                panel.controls.append(None)
                continue
            if k == "head":
                controls.append(ft.Container(
                    ft.Text(e.label.upper(), size=T.T_SMALL, color=T.TEXT_3, weight=ft.FontWeight.W_600,
                            style=ft.TextStyle(letter_spacing=_px(1)), no_wrap=True),
                    height=h, padding=ft.Padding(_px(INSET + PAD_X), _px(8), _px(PAD_X), 0)))
                panel.controls.append(None)
                continue
            enabled = k == "sub" or handler_of(e) is not None
            danger = isinstance(e, Item) and e.danger
            icon_color = (T.ERROR if danger else T.TEXT_2) if enabled else T.TEXT_3
            icon = C.as_icon(e[1], T.ICON_S, icon_color) if e[1] else None
            parts = [ft.Container(icon, width=T.ICON_S, alignment=ft.Alignment.CENTER),
                     ft.Text(e[0], size=T.T_BODY, color=T.TEXT if enabled else T.TEXT_3, expand=True)]
            if k == "sub":
                parts.append(ft.Icon(ft.Icons.CHEVRON_RIGHT_ROUNDED, size=T.ICON_S, color=T.TEXT_3))
            row = ft.Container(
                ft.Row(parts, spacing=_px(GAP), vertical_alignment=ft.CrossAxisAlignment.CENTER),
                height=h, padding=ft.Padding(_px(PAD_X), 0, _px(PAD_X), 0),
                margin=ft.Margin(_px(INSET), 0, _px(INSET), 0), border_radius=T.RADIUS_XS,
                on_hover=(lambda ev, i=i: self._hover(panel, i, ev.data in (True, "true"))) if enabled else None,
                on_click=(lambda ev, i=i: self._activate(panel, i, ev)) if enabled else None)
            controls.append(row)
            panel.controls.append(row)
        win_w, win_h = self._window()
        too_tall = panel.height > win_h - 2 * _px(MARGIN)
        column = ft.Column(controls, spacing=0, tight=True,
                           scroll=ft.ScrollMode.AUTO if too_tall else None)
        inner = ft.Container(column, bgcolor=T.SURFACE_2, padding=ft.Padding(0, _px(PAD_Y), 0, _px(PAD_Y)),
                             width=panel.width - (_px(3) if T.DUAL else 0),
                             height=(win_h - 2 * _px(MARGIN) - 2) if too_tall else None)
        reduce = getattr(self.app, "reduce_motion", False)
        panel.control = ft.Container(
            inner, left=panel.left, top=panel.top, width=panel.width,
            # dual themes: the portal's blue → orange along the left edge (the gradient shows in the left padding)
            gradient=C.portal_gradient(vertical=True) if T.DUAL else None,
            padding=ft.Padding(_px(3), 0, 0, 0) if T.DUAL else 0, bgcolor=T.SURFACE_2,
            border=ft.Border.all(1, T.BORDER_STRONG), border_radius=T.RADIUS_SM,
            clip_behavior=ft.ClipBehavior.ANTI_ALIAS,
            shadow=ft.BoxShadow(blur_radius=_px(18), spread_radius=0, color=T.soft("#000000", 0.55),
                                offset=ft.Offset(0, _px(6))),
            opacity=1 if reduce else 0,
            animate_opacity=None if reduce else ft.Animation(110, ft.AnimationCurve.EASE_OUT),
            on_hover=lambda ev: self._entered(panel) if ev.data in (True, "true") else None)
        if too_tall:
            panel.top = _px(MARGIN)
            panel.control.top = panel.top

    def _fade_in(self, panel: Panel) -> None:
        if panel.control.opacity == 1:
            return

        async def show():
            import asyncio

            await asyncio.sleep(0.02)
            panel.control.opacity = 1
            C.update(panel.control)
        try:
            self.app.page.run_task(show)
        except Exception:  # noqa: BLE001 - no running loop (tests): just show it
            panel.control.opacity = 1

    # ------------------------------------------------------------------ behaviour
    def _paint(self, panel: Panel, i: int | None, on: bool) -> None:
        if i is None or i >= len(panel.controls) or panel.controls[i] is None:
            return
        row = panel.controls[i]
        if T.DUAL:
            row.gradient = C.portal_gradient(opacity=0.22) if on else None
            row.bgcolor = None
        else:
            row.bgcolor = T.soft(T.ACCENT, 0.16) if on else None
        C.update(row)

    def _highlight(self, panel: Panel, i: int | None) -> None:
        if panel.hi == i:
            return
        # the row whose submenu is open stays lit
        if panel.hi is not None and not (panel.child is not None and panel.child.parent_row == panel.hi):
            self._paint(panel, panel.hi, False)
        panel.hi = i
        self._paint(panel, i, True)

    def _hover(self, panel: Panel, i: int, inside: bool) -> None:
        if self.root is None:
            return
        if not inside:
            if panel.hi == i and not (panel.child is not None and panel.child.parent_row == i):
                self._paint(panel, i, False)
                panel.hi = None
            return
        self._highlight(panel, i)
        self._gen += 1
        gen = self._gen
        if kind(panel.entries[i]) == "sub":
            if panel.child is not None and panel.child.parent_row == i:
                return
            self._later(OPEN_DELAY, gen, lambda: self._open_child(panel, i))
        elif panel.child is not None:
            self._later(CLOSE_DELAY, gen, lambda: self._close_child(panel))

    def _entered(self, panel: Panel) -> None:
        """The pointer reached a panel: a pending close of it (from its parent's rows) is off."""
        if panel.parent is not None:
            self._gen += 1

    def _later(self, delay: float, gen: int, fn: Callable[[], None]) -> None:
        async def run():
            import asyncio

            await asyncio.sleep(delay)
            if gen == self._gen and self.root is not None:
                fn()
        try:
            self.app.page.run_task(run)
        except Exception:  # noqa: BLE001 - no loop (tests): at once
            fn()

    def _activate(self, panel: Panel, i: int, ev) -> None:
        e = panel.entries[i]
        if kind(e) == "sub":
            self._open_child(panel, i, focus=ev is None)
            return
        handler = handler_of(e)
        if handler is None:
            return
        self.close()
        handler(ev)

    def _open_child(self, panel: Panel, i: int, focus: bool = False) -> None:
        if self.root is None:
            return
        if panel.child is not None:
            if panel.child.parent_row == i:
                if focus and panel.child.actionable():
                    self._highlight(panel.child, panel.child.actionable()[0])
                return
            self._close_child(panel, update=False)
        sub = Panel(panel.entries[i].entries, parent=panel, parent_row=i)
        win_w, win_h = self._window()
        sub.left, sub.top, _ = place_beside(panel.left, panel.width, panel.top + panel.rows[i][0], sub.width,
                                            sub.height, win_w, win_h)
        panel.child = sub
        self._highlight(panel, i)
        self._paint(panel, i, True)
        self._build(sub)
        self.layer.controls.append(sub.control)
        C.update(self.layer)
        self._fade_in(sub)
        if focus and sub.actionable():
            self._highlight(sub, sub.actionable()[0])

    def _close_child(self, panel: Panel, update: bool = True) -> None:
        child = panel.child
        if child is None:
            return
        if child.child is not None:
            self._close_child(child, update=False)
        panel.child = None
        if self.layer is not None and child.control in self.layer.controls:
            self.layer.controls.remove(child.control)
        if panel.hi != child.parent_row:
            self._paint(panel, child.parent_row, False)
        if update and self.layer is not None:
            C.update(self.layer)
