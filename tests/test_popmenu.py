"""FramePort's own menu (ui/popmenu.py, GitHub #414): layout, placement inside the window, submenus, keyboard."""
import flet as ft

from frameport.ui import popmenu as P
from frameport.ui import theme as T
from frameport.ui.menus import Header, Item, Submenu

LONG = "Update with a newer version from a folder that has a really long name somewhere on this PC…"


class FakePage:
    def __init__(self, width=1280, height=820):
        self.overlay, self.width, self.height, self.updates = [], width, height, 0

    def update(self):
        self.updates += 1

    def run_task(self, fn, *args):  # no event loop in tests: PopMenu then acts at once
        raise RuntimeError("no loop")


class FakeApp:
    def __init__(self, reduce_motion=False, **page):
        self.page, self.reduce_motion = FakePage(**page), reduce_motion


def _menu(calls):
    act = lambda name: (lambda e: calls.append(name))  # noqa: E731
    return [("Play on Frame", ft.Icons.PLAY_ARROW_ROUNDED, act("play")),
            ("Disabled", ft.Icons.LOCK_OUTLINE_ROUNDED, None),
            None,
            Submenu("Name and artwork", ft.Icons.IMAGE_OUTLINED, [
                ("Rename…", ft.Icons.EDIT, act("rename")), ("Find artwork…", ft.Icons.IMAGE, act("art"))]),
            Submenu("Troubleshoot", ft.Icons.BUILD_OUTLINED, [
                Header("Logs"), ("Collect logs", ft.Icons.FOLDER, act("logs")), ("Report", ft.Icons.BUG_REPORT,
                                                                                 act("report"))]),
            None,
            Item("Remove from library…", ft.Icons.DELETE_OUTLINE_ROUNDED, act("remove"), True)]


def test_panels_size_to_their_longest_label_and_wrap_beyond_the_limit():
    short = P.layout([("Copy", None, None)])
    assert short[0] == T.px(P.MIN_W)
    mid = P.layout([("Update with a newer APK…", "i", None)])
    assert T.px(P.MIN_W) < mid[0] < T.px(P.MAX_W) and mid[1][0][2] == 1 and mid[1][0][1] == T.px(P.ROW_H)
    long = P.layout([("Copy", None, None), (LONG, "i", None)])
    assert long[0] == T.px(P.MAX_W)  # no 280 px cap, but a sane maximum
    assert long[1][1][2] >= 2 and long[1][1][1] > T.px(P.ROW_H)  # wraps: a taller row
    assert long[1][1][0] == long[1][0][0] + long[1][0][1]  # rows stack
    sub = P.layout([Submenu("Name and artwork", "i", [])])[0]
    assert sub > P.layout([("Name and artwork", "i", None)])[0] or sub == T.px(P.MIN_W)  # room for the chevron


def test_layout_follows_the_ui_scale():
    base = P.layout([("Update with a newer APK…", "i", None), None, ("Rename…", "i", None)])
    try:
        T.set_scale(1.5)
        big = P.layout([("Update with a newer APK…", "i", None), None, ("Rename…", "i", None)])
    finally:
        T.set_scale(1.0)
    assert big[0] > base[0] * 1.4 and big[2] > base[2] * 1.4


def test_place_flips_and_clamps_into_the_window():
    m = T.px(P.MARGIN)
    assert P.place(100, 100, 200, 300, 1280, 820) == (100, 100)
    x, y = P.place(1250, 100, 200, 300, 1280, 820)  # no room on the right: it ends at the point
    assert x == 1050
    x, y = P.place(100, 700, 200, 300, 1280, 820)  # no room below: it ends at the point
    assert y == 400
    x, y = P.place(100, 700, 200, 900, 1280, 820)  # taller than the window: clamped to the top
    assert y == m
    # under a button (flip_x = its right edge, flip_y = its top)
    assert P.place(1200, 60, 200, 300, 1280, 820, flip_x=1240, flip_y=20) == (1040, 60)
    x, y = P.place(5, 5, 200, 300, 1280, 820)
    assert (x, y) == (m, m)


def test_submenus_open_on_the_side_with_room():
    x, y, side = P.place_beside(100, 200, 150, 220, 160, 1280, 820)
    assert side == "right" and x > 290 and y < 150
    x, y, side = P.place_beside(1000, 250, 150, 220, 160, 1280, 820)
    assert side == "left" and x + 220 <= 1002
    x, y, side = P.place_beside(100, 200, 780, 220, 160, 1280, 820)  # near the bottom: moved up into the window
    assert y + 160 <= 820 - T.px(P.MARGIN)


def test_open_hover_submenu_and_choose():
    calls, app = [], FakeApp()
    menu = P.PopMenu(app)
    menu.show(_menu(calls), at=(200, 100))
    assert menu.is_open and len(app.page.overlay) == 1
    root = menu.root
    assert (root.left, root.top) == (200, 100)
    assert len(menu.layer.controls) == 2  # the outside-click barrier + the panel
    sub_row = 3
    menu._hover(root, sub_row, True)  # (no event loop: the hover intent acts at once)
    assert root.child is not None and len(menu.layer.controls) == 3
    assert root.child.left >= root.left + root.width - T.px(4)  # on the right
    assert menu.row_center("Rename…")[0] > root.left + root.width
    menu._hover(root, 0, True)  # back on a plain row: the submenu closes
    assert root.child is None and len(menu.layer.controls) == 2
    menu._activate(root, 0, "click")
    assert calls == ["play"] and not menu.is_open and app.page.overlay == []


def test_disabled_rows_and_danger_rows():
    calls, app = [], FakeApp()
    menu = P.PopMenu(app)
    menu.show(_menu(calls), at=(10, 10))
    root = menu.root
    disabled = root.controls[1]
    assert disabled.on_click is None and disabled.on_hover is None
    menu._activate(root, 1, None)
    assert menu.is_open and calls == []
    danger = root.controls[len(root.entries) - 1]
    assert danger.content.controls[0].content.color == T.ERROR


def test_keyboard():
    calls, app = [], FakeApp()
    menu = P.PopMenu(app)
    assert menu.handle_key("Arrow Down") is False  # nothing open: the key isn't ours
    menu.show(_menu(calls), at=(10, 10))
    root = menu.root
    assert menu.handle_key("Arrow Down") and root.hi == 0
    menu.handle_key("Arrow Down")
    assert root.hi == 3  # the disabled row and the divider are skipped
    menu.handle_key("Arrow Up")
    menu.handle_key("Arrow Up")
    assert root.hi == 6  # wraps round to the last row
    menu.handle_key("Arrow Up")
    menu.handle_key("Arrow Up")
    assert root.hi == 3
    menu.handle_key("Arrow Right")  # opens the submenu, its first item highlighted
    assert root.child is not None and root.child.hi == 0
    menu.handle_key("Arrow Left")
    assert root.child is None and menu.is_open
    menu.handle_key("Arrow Down")
    menu.handle_key("Arrow Right")
    assert root.child.hi == 1  # Troubleshoot: its header is skipped
    menu.handle_key("Escape")  # closes the submenu only
    assert root.child is None and menu.is_open
    assert menu.handle_key("a")  # typing doesn't reach the Library's search behind it
    menu.handle_key("Arrow Up")
    menu.handle_key("Arrow Up")
    menu.handle_key("Enter")
    assert calls == ["play"] and not menu.is_open
    menu.show(_menu(calls), at=(10, 10))
    menu.handle_key("Escape")
    assert not menu.is_open


def test_reopening_replaces_the_menu_and_reduce_motion_skips_the_fade():
    app = FakeApp(reduce_motion=True)
    menu = P.PopMenu(app)
    menu.show(_menu([]), at=(10, 10))
    first = menu.layer
    assert menu.root.control.opacity == 1 and menu.root.control.animate_opacity is None
    menu.show(_menu([]), at=(50, 50))
    assert app.page.overlay == [menu.layer] and menu.layer is not first
    menu.show([], at=(10, 10))  # nothing to show: closed
    assert not menu.is_open and app.page.overlay == []
    faded = P.PopMenu(FakeApp())
    faded.show(_menu([]), at=(10, 10))
    assert faded.root.control.animate_opacity is not None


def test_a_menu_near_the_corner_stays_inside_and_its_submenu_opens_left():
    app = FakeApp(width=900, height=600)
    menu = P.PopMenu(app)
    menu.show(_menu([]), at=(890, 590))
    root = menu.root
    m = T.px(P.MARGIN)
    assert root.left + root.width <= 900 - m and root.top + root.height <= 600 - m
    menu._open_child(root, 3)
    assert root.child.left < root.left


def test_under_a_button():
    app = FakeApp()
    menu = P.PopMenu(app)
    menu.show(_menu([]), below=(500, 300, 540, 340))
    assert menu.root.left == 500 and menu.root.top == 340 + T.px(4)
