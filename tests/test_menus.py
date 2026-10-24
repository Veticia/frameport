"""Menu structure: game menus (Library right-click, game page "…"), the Monitor process menu and menu_items."""
import flet as ft

from frameport.ui import components as C
from frameport.ui import theme as T
from frameport.ui.menus import (
    RECIPE_HELP,
    Header,
    MenuState,
    menu_sections,
    process_menu,
    recipe_actions,
    selection_menu,
)

DESTRUCTIVE = {"Uninstall from Frame…", "Uninstall from this PC…", "Remove from library…"}
QUEST = {"package": "com.q", "title": "Q"}
RIFT = {"package": "rift.r", "kind": "rift", "title": "R"}
LINUX = {"package": "linux.l", "kind": "linux", "title": "L"}


def _key(k):
    return k


def _play(where="Frame"):
    return (f"Play on {where}", ft.Icons.PLAY_ARROW_ROUNDED, f"play_{where}")


def _install(label="Reinstall on Frame"):
    return (label, "fp:frame", "install")


def _states():
    """(name, game, state) for the cases the menus have to handle."""
    return [
        ("quest on Frame", QUEST, MenuState(connected=True, on_frame=True, plays=[_play()], installs=[_install()],
                                            settings=True, selectable=True, android=True, saves=True)),
        ("quest not installed", QUEST, MenuState(connected=True, installs=[_install("Install on Frame")],
                                                 settings=True, selectable=True, android=True)),
        ("rift on PC", RIFT, MenuState(on_pc=True, plays=[_play("this PC")], programs=True, selectable=True,
                                       installs=[_install("Reinstall on this PC")], new_version=True)),
        ("linux on Frame", LINUX, MenuState(connected=True, on_frame=True, plays=[_play()], installs=[_install()],
                                            selectable=True, new_version=True)),
        ("linux job running", LINUX, MenuState(connected=True, on_frame=True, job=True, selectable=True,
                                               new_version=True)),
        ("job running", QUEST, MenuState(connected=True, on_frame=True, job=True, settings=True, selectable=True,
                                         android=True)),
    ]


def _items(entries):
    return [e for e in entries if e is not None and not isinstance(e, Header)]


def _section_of(entries, label):
    """The header the item sits under (None = top section)."""
    head = None
    for e in entries:
        if isinstance(e, Header):
            head = e.label
        elif e is not None and e[0] == label:
            return head
    raise AssertionError(f"{label} not in menu")


def test_library_menu_is_short():
    """GitHub #414: the right-click menu has only what's used all the time; the rest is behind More actions…"""
    for name, g, st in _states():
        entries = C.menu_entries(menu_sections(g, st, _key))
        labels = [e[0] for e in _items(entries)]
        assert not any(isinstance(e, Header) for e in entries), name  # no section headers
        assert len(labels) <= 6, (name, labels)
        assert all(e[1] for e in _items(entries)), name
        assert "Open game page" in labels
        if st.job:
            assert labels == ["Show progress", "Cancel", "Open game page"]
            continue
        # the game's own actions, a divider, then where to go: its page, Select, More actions…
        expected = [*(p[0] for p in st.plays[:1]), *(i[0] for i in st.installs[:1]),
                    *(["Game settings…"] if st.settings else [])]
        assert labels == [*expected, "Open game page", "Select", "More actions…"], (name, labels)
        assert entries[len(expected)] is None, name
        for gone in ("Find artwork…", "Analyze again", "Collect logs", "Screenshots", "Run launch test on Frame",
                     "Share working recipe…", "Type on Frame", "Rename…", "Update with a newer APK…",
                     "Update with a newer version…", *DESTRUCTIVE):
            assert gone not in labels, (name, gone)


def test_what_left_the_library_menu_is_on_the_game_page():
    """Every action the right-click menu dropped is in the page's "…" menu, or on the page itself: uninstalls on the
    "Where it's installed" cards (and the Frame page), not repeated in the "…" menu."""
    on_page = {"Uninstall from Frame…", "Uninstall from this PC…"}
    for name, g, st in _states():
        if st.job:
            continue
        before = {"Rename…", "Remove from library…"} | ({"Update with a newer APK…"} if st.android else set()) \
            | ({"Update with a newer version…"} if st.new_version else set())
        st.quick = False
        page = {e[0] for e in _items(menu_sections(g, st, _key))}
        assert before <= page, (name, before - page)
        assert not on_page & page


def test_library_menu_on_a_selection_acts_on_all_of_it():
    labels = [e[0] if e else None for e in selection_menu(3, 1, True, True, _key)]
    assert labels == ["Install 3 on Frame", "Install 1 on this PC", None, "Select all shown", "Clear selection"]
    keys = [e[2] for e in selection_menu(3, 0, False, True, _key) if e]
    assert keys[0] is None  # no Frame: shown, but disabled
    assert "install_pc" not in keys  # no PC VR game in the selection
    assert [e[2] for e in selection_menu(2, 2, True, False, _key) if e][1] is None  # no Windows: disabled


def test_recipe_actions_are_for_games_with_a_recipe():
    assert [e[2] for e in recipe_actions(QUEST, _key)] == ["reset_recipe", "save_recipe", "share"]
    assert [e[0] for e in recipe_actions(RIFT, _key)][0] == "Reset to suggested recipe…"
    assert recipe_actions(LINUX, _key) == []
    assert len(RECIPE_HELP) == len(recipe_actions(QUEST, _key))
    from frameport.ui.help import HELP
    assert all(HELP[k] for k in RECIPE_HELP)


def test_menu_rows_wrap_instead_of_running_past_the_edge():
    """GitHub #414: Flet's own icon row gave a long label no width limit; ours lets it take the rest and wrap."""
    item = C.menu_items([("Update with a newer APK…", ft.Icons.UPGRADE_ROUNDED, lambda e: None)])[0]
    assert item.icon is None
    row = item.content
    assert isinstance(row, ft.Row) and len(row.controls) == 2
    text = row.controls[1]
    assert isinstance(text, ft.Text) and text.expand and text.value == "Update with a newer APK…"
    assert text.max_lines is None and text.no_wrap is not True
    plain = C.menu_items([("Plain", None, lambda e: None)])[0].content
    assert plain.controls[0].expand
    disabled = C.menu_items([("Protected", ft.Icons.LOCK_OUTLINE_ROUNDED, None)])[0]
    assert disabled.disabled and disabled.content.controls[1].color == T.TEXT_3


def test_menu_buttons_widen_with_the_ui_scale():
    try:
        T.set_scale(1.5)
        wide = C.menu_button([]).size_constraints
        assert wide.max_width == T.px(C.MENU_MAX_W + 40) > C.MENU_MAX_W * 1.5
    finally:
        T.set_scale(1.0)
    assert C.menu_button([]).size_constraints.max_width == C.MENU_MAX_W + 40


def test_page_menu_is_the_full_one_without_its_buttons():
    for name, g, st in _states():
        st.quick = False
        entries = menu_sections(g, st, _key)
        labels = [e[0] for e in _items(entries)]
        for gone in ("Select", "Open game page", "Show progress", "Cancel", "Play on Frame", "Reinstall on Frame",
                     "Run launch test on Frame", "Uninstall from Frame…", "More actions…"):
            assert gone not in labels, (name, gone)
        assert labels[-1] == "Remove from library…"
        assert entries[-2] is None
        for i, e in enumerate(entries):  # every header has items under it
            if isinstance(e, Header):
                assert i + 1 < len(entries) and entries[i + 1] is not None and not isinstance(entries[i + 1], Header)
        assert all(e[1] for e in _items(entries)), name
        assert "Rename…" in labels, name  # every kind of game (Quest, PC VR, Linux), also while a job runs


def test_sections_and_conditions():
    _, g, st = _states()[0]
    st.quick = False
    entries = menu_sections(g, st, _key)
    heads = [e.label for e in entries if isinstance(e, Header)]
    assert heads == ["Saves", "Name and artwork", "Troubleshoot"]  # the recipe's actions are on its card
    assert _section_of(entries, "Analyze again") == "Troubleshoot"
    assert _section_of(entries, "Update art on Frame") == "Name and artwork"
    assert _section_of(entries, "Rename…") == "Name and artwork"
    assert _section_of(entries, "Restore saves…") == "Saves"
    labels = [e[0] for e in _items(entries)]
    assert "Back up saves" not in labels  # a button next to Play
    assert not {e[0] for e in recipe_actions(QUEST, _key)} & set(labels)
    rift_st = _states()[2][2]
    rift_st.quick = False
    rift = [e[0] for e in _items(menu_sections(RIFT, rift_st, _key))]
    assert {"Check game files", "Change program…"} <= set(rift)
    assert "Analyze again" not in rift
    # the reset icon isn't the Frame's Restart icon
    reset = next(e for e in recipe_actions(QUEST, _key) if e[0] == "Reset to suggested recipe…")
    assert reset[1] != ft.Icons.RESTART_ALT_ROUNDED


def test_update_app_only_for_quest_games_on_the_frame():
    """Discussion #180: the new build without replacing game data changed on the Frame (e.g. a modded OBB)."""
    def has(g, st):
        st.quick = False
        return "Update app only…" in [e[0] for e in _items(menu_sections(g, st, _key))]
    states = {name: (g, st) for name, g, st in _states()}
    assert has(*states["quest on Frame"])
    assert _section_of(menu_sections(QUEST, states["quest on Frame"][1], _key), "Update app only…") is None
    for name in ("quest not installed", "rift on PC", "linux on Frame", "job running"):
        assert not has(*states[name]), name
    assert "Update app only…" not in [e[0] for e in _items(menu_sections(QUEST, _states()[0][2], _key))]  # quick


def test_menu_entries_trim_headers_and_dividers():
    act = ("A", ft.Icons.ADD_ROUNDED, None)
    raw = [None, Header("Empty"), None, Header("Kept"), act, None, None, Header("Last")]
    assert C.menu_entries(raw) == [Header("Kept"), act]
    items = C.menu_items([act, None, Header("H"), act])
    assert items[0].height == items[3].height == T.px(32) and items[2].height == T.px(24)
    assert items[1].content is None  # divider
    assert items[2].disabled and items[0].disabled  # no handler: shown disabled


def test_process_menu():
    proc = {"name": "x", "pid": 5, "game": "com.q"}
    labels = [e[0] if e else "—" for e in process_menu(proc, _key)]
    assert labels == ["Copy name", "Copy PID 5", "—", "End…", "Force quit…", "—", "End game…"]
    locked = process_menu({**proc, "locked": True}, _key)
    prot = next(e for e in locked if e and e[0] == "Protected by SteamOS")
    assert prot[2] is None and "End…" not in [e[0] for e in locked if e]
    group = [e[0] if e else "—" for e in process_menu({"kind": "group", "count": 3, "name": "g"}, _key)]
    assert group == ["Show its processes", "Copy name", "—", "End all 3…", "Force quit all 3…"]
    end = next(e for e in process_menu(proc, _key) if e and e[0] == "End…")
    assert end[1] != ft.Icons.CLOSE_ROUNDED


def test_newer_apk_for_quest_games_in_both_menus():
    """GitHub #161: update a game with a newer APK from the page's "…" menu (right-click → More actions… since
    #414 shortened the right-click menu)."""
    for name, g, st in _states():
        quick = [e[0] for e in _items(menu_sections(g, st, _key))]
        st.quick = False
        full = [e[0] for e in _items(menu_sections(g, st, _key))]
        want = g is QUEST and not st.job
        assert "Update with a newer APK…" not in quick, name
        assert ("Update with a newer APK…" in full) == want, name


def test_newer_version_for_linux_apps_and_pc_vr_games():
    """GitHub #161: Linux apps and PC VR games get "Update with a newer version…" (a file or folder), not the APK
    entry, in the page's "…" menu; never while a job runs."""
    for name, g, st in _states():
        quick = [e[0] for e in _items(menu_sections(g, st, _key))]
        st.quick = False
        full = [e[0] for e in _items(menu_sections(g, st, _key))]
        want = g is not QUEST and not st.job
        assert "Update with a newer version…" not in quick, name
        assert ("Update with a newer version…" in full) == want, name
        if g is not QUEST:
            assert "Update with a newer APK…" not in quick + full, name


def test_save_backups_in_the_full_menu_of_quest_games():
    """Discussion #352: restore a Quest game's saves (installed on the Frame, no job running; Back up saves is a
    button next to Play); the backups
    folder for every Quest game; nothing for PC VR games and Linux apps; never in the short right-click menu."""
    for name, g, st in _states():
        quick = [e[0] for e in _items(menu_sections(g, st, _key))]
        assert not {"Back up saves", "Restore saves…", "Open backups folder"} & set(quick), name
        st.quick = False
        entries = menu_sections(g, st, _key)
        full = [e[0] for e in _items(entries)]
        live = g is QUEST and st.saves and not st.job
        assert "Back up saves" not in full, name  # a button next to Play (game page hero)
        assert ("Restore saves…" in full) == live, name
        assert ("Open backups folder" in full) == (g is QUEST), name
        if g is QUEST:
            assert _section_of(entries, "Open backups folder") == "Saves"


def test_check_for_updates_for_games_from_install_links():
    """Games installed from an install link get "Check for updates" in the page's full menu (asks the link now);
    never while a job runs, never for other games, never in the short right-click menu."""
    for name, g, st in _states():
        st.quick = False
        assert "Check for updates" not in [e[0] for e in _items(menu_sections(g, st, _key))], name
        st.link = True
        full = menu_sections(g, st, _key)
        assert ("Check for updates" in [e[0] for e in _items(full)]) == (not st.job), name
        st.quick = True
        assert "Check for updates" not in [e[0] for e in _items(menu_sections(g, st, _key))], name
