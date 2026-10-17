"""Game settings (FrameBridge adapter settings) for non-technical users: only the settings that can matter for this
game, in plain words, with switches, sliders and choices instead of number fields; advanced ones on request.
Adapter values are kept in the recipe; foveation is a separate library preference.
Installed games receive settings and launcher changes without rebuilding the APK."""
from __future__ import annotations

from typing import TYPE_CHECKING

import flet as ft

from ... import pipeline
from ...core import library
from ...core.controllers import QUEST_BUTTONS, effective_adapter_settings, quest_buttons_default
from ...i18n import tr
from ...patches import base
from ...patches.settings import (
    BASE_SETTINGS,
    FOVEATION_UI,
    GROUPS,
    HIDDEN,
    SETTINGS,
    UI,
    Foveation,
    foveation_options,
    foveation_preference,
)
from .. import components as C
from .. import theme as T
from ..controllers import diagram, diagram_source

if TYPE_CHECKING:
    from ..app import FramePortApp

# the dialog's settings: numbers and switches (text settings such as vk_shader_fix are recipe data: saving the dialog
# keeps them as they are, as it keeps the HIDDEN ones)
SPECS = {key: (kind, default) for key, kind, default, _, _ in SETTINGS
         if kind in ("int", "float") and key not in HIDDEN}


def default(key: str):
    return BASE_SETTINGS.get(key, SPECS[key][1])


def _num(key: str, value):
    kind = SPECS[key][0]
    try:
        return float(value) if kind == "float" else int(float(value))
    except (TypeError, ValueError):
        return default(key)


def relevant(analysis, key: str, value) -> bool:
    """Shown when the setting can matter for this game (or is already changed from its default)."""
    if analysis is None:
        return True
    return base.get(f"adapter.{key}").applies(analysis) or value != default(key)


def recipe_values(g: dict) -> dict:
    vals = {key: default(key) for key in SPECS}
    patches = library.recipe_from_dict(g["recipe"]).patches
    vals.update({k: _num(k, v) for k, v in effective_adapter_settings(patches).items() if k in SPECS})
    return vals


def save_to_recipe(package: str, values: dict, *, inherit_quest_buttons: bool | None = None) -> None:
    """Adapter patches = the settings that differ from their defaults (BASE_SETTINGS are written anyway)."""
    g = library.game(package)
    r = library.recipe_from_dict(g["recipe"])
    before = {pid: dict(v or {}) for pid, v in r.patches.items() if pid.startswith("adapter.")}
    if inherit_quest_buttons is None:
        inherit_quest_buttons = (QUEST_BUTTONS not in r.patches
                                 and values.get("quest_buttons") == quest_buttons_default())
    for key in SPECS:
        pid = f"adapter.{key}"
        v = _num(key, values.get(key, default(key)))
        if (inherit_quest_buttons if key == "quest_buttons" else v == default(key)):
            r.patches.pop(pid, None)
            r.reasons.pop(pid, None)
        else:
            if (r.patches.get(pid) or {}).get("value") != v:
                r.reasons[pid] = tr("Set by you.")
            r.patches[pid] = {"value": v}
    after = {pid: dict(v or {}) for pid, v in r.patches.items() if pid.startswith("adapter.")}
    if after == before:  # saved without a change: the recipe keeps following FramePort's catalog (GitHub #10)
        return
    r.source = "user"
    pipeline.set_recipe(package, r)


def show_adapter_dialog(app: FramePortApp, package: str) -> None:
    g = library.game(package)
    if g is None:  # installed on the Frame but not in this library: read the values there first
        target = app.target
        if target is None:
            app.toast(tr("Connect your Frame first."), error=True)
            return

        def load():
            result = target.set_settings(package, {})  # no changes: returns the current values
            cur = result["settings"]
            vals = {key: default(key) for key in SPECS}
            vals.update({k: _num(k, v) for k, v in cur.items() if k in SPECS})
            env = result.get("foveation_env") or {}
            mode = ("off" if env.get("LEPTON_ENV_DISABLE_VULKAN_FDM_INJECTION_LAYER") == "1" else
                    ("fixed" if "disable_offsets" in env.get("FDM_DEBUG", "") else "tracked")
                    if env.get("ENABLE_VULKAN_FDM_INJECTION_LAYER") == "1" else "")
            pref = {"mode": mode}
            if mode in ("tracked", "fixed"):
                pref.update(level=env.get("FOVE_LEVEL") if env.get("FOVE_LEVEL") in ("lo", "med", "hi") else "",
                            test=env.get("LEPTON_ENV_FRAMEPORT_FOVEATION_TEST") == "1")
            _open(app, package, None, vals, pref)
        app.run_bg(load)
        return
    _open(app, package, g, recipe_values(g))


def _open(app: FramePortApp, package: str, g: dict | None, values: dict, foveation_values=None,
          inherit_quest_buttons: bool | None = None) -> None:
    analysis = library.analysis_from_dict(g["analysis"]) if g and g.get("analysis") else None
    suggested = recipe_values({"recipe": g["suggested"]}) if g and g.get("suggested") else \
        {key: default(key) for key in SPECS}
    values = dict(values)
    global_mode = quest_buttons_default()
    inherited = {"on": (g is not None and QUEST_BUTTONS not in (g.get("recipe") or {}).get("patches", {}))
                 if inherit_quest_buttons is None else inherit_quest_buttons}
    controller_preview = diagram(values["quest_buttons"])
    foveation = dict(foveation_values) if foveation_values is not None else foveation_preference(g or {})
    foveation_controls = []
    foveation_rows = {}

    def foveation_change(key, value):
        foveation[key] = value
        for name in ("level", "test"):
            if name in foveation_rows:
                foveation_rows[name].visible = (foveation.get("mode") in ("tracked", "fixed") and
                                               (name != "test" or advanced["on"]))
                C.update(foveation_rows[name])

    foveation_specs = (("mode", Foveation.CHOICES), ("level", Foveation.LEVEL_CHOICES), ("test", None))
    for key, choices in (foveation_specs if not g or g.get("kind", "quest") == "quest" else ()):
        ui = FOVEATION_UI[key]
        if choices:
            value = str(foveation.get(key, ""))
            choices = list(choices)
            if value not in [str(v) for v, _ in choices]:
                choices.append((value, value))
            control = C.dropdown(value=value, width=T.px(220),
                options=[ft.DropdownOption(key=str(v), text=tr(label)) for v, label in choices],
                on_select=lambda e, k=key: foveation_change(k, e.control.value))
        else:
            control = C.switch(value=bool(foveation.get(key)),
                               on_change=lambda e: foveation_change("test", bool(e.control.value)))
        foveation_rows[key] = ft.Container(ft.Row([
            ft.Column([C.body(tr(ui["label"]), T.TEXT, weight=ft.FontWeight.W_500),
                       C.meta(tr(ui["help"]), T.TEXT_2)], spacing=T.px(2), expand=True), control],
            spacing=T.S4, vertical_alignment=ft.CrossAxisAlignment.CENTER),
            padding=ft.Padding(0, T.px(8), 0, T.px(8)))
        foveation_controls.append(foveation_rows[key])
    controls: dict[str, ft.Control] = {}   # key -> the input control
    rows: dict[str, ft.Container] = {}     # key -> its row (visibility)
    readouts: dict[str, ft.Text] = {}      # slider value labels
    advanced = {"on": bool(foveation.get("test"))
                or any(UI[k]["level"] == "advanced" and values[k] != default(k) for k in SPECS)}

    def set_value(key, v):
        if key == "quest_buttons":
            inherited["on"] = v == "global"
            v = global_mode if inherited["on"] else v
        values[key] = _num(key, v)
        if key == "quest_buttons":
            controller_preview.src = diagram_source(values[key])
            C.update(controller_preview)
        if key in readouts:
            readouts[key].value = UI[key]["control"][4].format(values[key])
            C.update(readouts[key])
        if any(UI[k].get("depends") == key for k in SPECS):
            apply_visibility()

    def control_for(key):
        kind, *spec = UI[key]["control"]
        v = values[key]
        if kind == "switch":
            return C.switch(value=bool(v), on_change=lambda e: set_value(key, 1 if e.control.value else 0))
        if kind == "slider":
            lo, hi, step, fmt = spec
            readouts[key] = C.body(fmt.format(v), T.TEXT, weight=ft.FontWeight.W_600)
            return ft.Row([ft.Slider(min=lo, max=hi, divisions=round((hi - lo) / step), value=min(max(v, lo), hi),
                                     active_color=T.ACCENT, width=T.px(200),
                                     on_change=lambda e: set_value(key, round(e.control.value / step) * step)),
                           ft.Container(readouts[key], width=T.px(64))], spacing=0, tight=True)
        choices = list(spec[0])
        if v not in [c for c, _ in choices]:
            choices.append((v, str(v)))  # a value set elsewhere (CLI, catalog) stays selectable
        if key == "quest_buttons" and g is not None:
            name = next(label for mode, label in UI[key]["control"][1] if mode == global_mode)
            choices.insert(0, ("global", tr("Use global default ({mode})").format(mode=tr(name))))
        return C.dropdown(value="global" if key == "quest_buttons" and inherited["on"] else str(v), width=T.px(260),
                           options=[ft.DropdownOption(key=str(c), text=tr(label)) for c, label in choices],
                           on_select=lambda e: set_value(key, e.control.value))

    def row(key):
        ui = UI[key]
        label = tr(ui["label"]) if ui.get("label") else tr(base.get(f"adapter.{key}").title)
        help_text = tr(ui["help"]) if ui.get("help") else tr(base.get(f"adapter.{key}").description)
        controls[key] = control_for(key)
        line = ft.Row([
            ft.Column([C.body(label, T.TEXT, weight=ft.FontWeight.W_500), C.meta(help_text, T.TEXT_2)],
                      spacing=T.px(2), expand=True),
            controls[key]], spacing=T.S4, vertical_alignment=ft.CrossAxisAlignment.CENTER)
        rows[key] = ft.Container(ft.Column([line, controller_preview], spacing=T.S2)
                                if key == "quest_buttons" else line,
            padding=ft.Padding(0, T.px(8), 0, T.px(8)))
        return rows[key]

    sections: list[tuple[str, ft.Control, list[str]]] = []
    body = []
    for group, title in GROUPS:
        keys = [k for k in UI if UI[k]["group"] == group and relevant(analysis, k, values[k])]
        if not keys:
            continue
        heading = C.body(tr(title), T.TEXT_2, weight=ft.FontWeight.W_600)
        section = ft.Column([heading, *[row(k) for k in keys],
                             *(foveation_controls if group == "picture" else [])], spacing=0)
        sections.append((group, section, keys))
        body.append(section)

    def apply_visibility():
        foveation_change("mode", foveation.get("mode", ""))
        for _group, section, keys in sections:
            shown = 0
            for k in keys:
                parent = UI[k].get("depends")
                vis = (advanced["on"] or UI[k]["level"] == "common") and (not parent or bool(values.get(parent)))
                rows[k].visible = vis
                shown += vis
            section.visible = bool(shown)
        C.update(*[s for _, s, _ in sections])

    def toggle_advanced(e):
        advanced["on"] = bool(e.control.value)
        apply_visibility()

    def reset(e):
        for k in list(values):
            values[k] = suggested.get(k, default(k))
        app.page.pop_dialog()
        recommended = (g.get("suggested") or {}).get("patches", {}) if g else {}
        _open(app, package, g, values, {}, inherit_quest_buttons=g is not None and QUEST_BUTTONS not in recommended)

    before = dict(values)

    def save(e):
        if foveation.get("mode", "") not in ("tracked", "fixed"):
            foveation.pop("level", None)
            foveation.pop("test", None)
        foveation_options(foveation)
        app.page.pop_dialog()
        if g is not None:
            library.set_fields(package, foveation=dict(foveation))
            save_to_recipe(package, values, inherit_quest_buttons=inherited["on"])
        reinstall = any(UI[k].get("reinstall") and values[k] != before[k] for k in SPECS)
        installed = app.target is not None and (g is None or C.install_state(g, app.frame_info) in
                                                ("installed", "outdated"))
        if not installed:
            app.toast(tr("Saved. They apply when the game is installed."))
            app.refresh_view()
            return
        target = app.target

        def push():
            target.set_settings(package, {k: values[k] for k in SPECS}, foveation=foveation)
            app.toast(tr("Saved. Restart the game to use the new settings.") +
                      (tr(" Reinstall it for the controller models.") if reinstall else ""))
            app.refresh_view()
        app.run_bg(push)

    show_adv = bool(foveation_controls) or any(UI[k]["level"] == "advanced" for _, _, keys in sections for k in keys)
    content = ft.Column([
        C.body(tr("Changes take effect the next time the game starts. Not sure? Leave a setting as it is."), T.TEXT_2),
        *body,
        *([C.switch(tr("Show advanced settings"), value=advanced["on"], on_change=toggle_advanced)]
          if show_adv else []),
    ], spacing=T.S3, scroll=ft.ScrollMode.AUTO, tight=True)
    title = tr("Game settings · {title}").format(title=app._title(package) if g else package)
    app.page.show_dialog(C.dialog(
        title, content, [C.ghost(tr("Reset to recommended"), ft.Icons.RESTART_ALT_ROUNDED, reset),
                         C.ghost(tr("Cancel"), on_click=lambda e: app.page.pop_dialog()),
                         C.primary(tr("Save"), on_click=save)], height=T.px(560)))
    apply_visibility()
