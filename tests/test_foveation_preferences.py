"""Discussion #365: personal runtime choices never become recipe/build inputs."""
import copy
from types import SimpleNamespace

import pytest
import yaml

from frameport.core import library
from frameport.core.models import Recipe
from frameport.game_update import apk_recipe_fingerprint
from frameport.patches.settings import UI, foveation_env, foveation_preference


def test_migration_extracts_choice_once_without_overwriting_preference():
    data = {"games": {
        "old": {"recipe": {"patches": {"device.foveation": {"mode": "fixed", "radius": .2, "test": True},
                                        "frame.adapter": {}}}},
        "chosen": {"recipe": {"patches": {"device.foveation": {"mode": "tracked"}}},
                   "foveation": {"mode": "off"}},
        "plain": {"recipe": {"patches": {}}}}}
    library._migrate(data)
    old = data["games"]["old"]
    assert old["foveation"] == {"mode": "fixed", "radius": .2, "test": True}
    assert old["recipe"]["patches"]["device.foveation"] == {"support": True}
    assert "frame.adapter" in old["recipe"]["patches"]
    assert data["games"]["chosen"]["foveation"] == {"mode": "off"}
    assert "foveation" not in data["games"]["plain"]
    snapshot = copy.deepcopy(data)
    library._migrate(data)
    assert data == snapshot


def test_working_config_excludes_legacy_foveation_in_all_outputs():
    from frameport.diag import issue
    game = {"package": "test.game", "title": "Test", "foveation": {"mode": "fixed"}}
    recipe = yaml.safe_dump({"package": "test.game", "patches": {
        "device.foveation": {"mode": "tracked", "radius": .2, "test": True}, "frame.adapter": {}},
        "reasons": {"device.foveation": "Eye tracking"}})
    args = (game, recipe, "works", "", {})
    assert "device.foveation" not in issue.working_config_body(*args)
    assert "device.foveation" not in issue.working_config_url(*args)
    links = issue.working_config_links(*args)
    assert "device.foveation" not in str(links)
    assert "frame.adapter" in issue.working_config_body(*args)
    from frameport.recommend import catalog
    exported = catalog.to_yaml(catalog.CatalogEntry(package="test.game", title="Test", status="works",
                               foveation="fixed", foveation_radius=.3, foveation_test=True))
    args = (game, exported, "works", "", {})
    assert "foveation" not in issue.working_config_body(*args)
    assert "foveation" not in issue.working_config_url(*args)
    assert "foveation" not in str(issue.working_config_links(*args))


@pytest.mark.parametrize("mode,expected", [("tracked", "enable"), ("fixed", "enable,disable_offsets"),
                                          ("off", None), ("", None)])
def test_launcher_preference_update_uses_settings_command_without_changing_recipe(monkeypatch, mode, expected):
    from frameport.targets.frame_lepton import FrameLeptonTarget
    recipe = library.recipe_to_dict(Recipe("test.game", patches={"frame.adapter": {}}))
    entry = {"package": "test.game", "recipe": recipe, "foveation": {"mode": mode}}
    original = copy.deepcopy(recipe)
    before = apk_recipe_fingerprint(recipe)
    monkeypatch.setattr(library, "game", lambda pkg: entry)
    calls = []
    target = object.__new__(FrameLeptonTarget)
    target.frame = SimpleNamespace(agent=lambda command, **args: calls.append((command, args)))
    monkeypatch.setattr(target, "connect", lambda: target)
    target.set_settings("test.game", {"scale": 1.0}, foveation={"mode": mode})
    assert len(calls) == 1 and calls[0][0] == "set_settings"
    assert calls[0][1]["foveation_env"]["FDM_DEBUG"] == expected
    assert recipe == original and apk_recipe_fingerprint(recipe) == before


def test_catalog_defaults_survive_and_explicit_choice_wins():
    from frameport.install.installer import configuration_args
    recipe = Recipe("com.crytek.climb2", patches={"device.foveation": {"mode": "tracked"}})
    entry = {"recipe": library.recipe_to_dict(recipe)}
    assert foveation_preference(entry) == {"mode": "tracked"}
    assert configuration_args(entry, recipe)["env"]["FDM_DEBUG"] == "enable,yflip"
    entry["foveation"] = {"mode": "off"}
    env = configuration_args(entry, recipe)["env"]
    assert env["VK_INSTANCE_LAYERS"] == "" and "FDM_DEBUG" not in env
    assert not any("CLIMB" in k for k in env)
    # Default restores the catalog recommendation.
    entry["foveation"] = {"mode": ""}
    assert configuration_args(entry, recipe)["env"]["FDM_DEBUG"] == "enable,yflip"


def test_radius_test_and_off_reset_managed_exports():
    env = foveation_env("test.game", {"mode": "fixed", "radius": .2, "test": True})
    assert env["FOVE_INNER"] == "0.2" and env["FOVE_OUTER"] == "0.4"
    assert env["LEPTON_ENV_FRAMEPORT_FOVEATION_TEST"] == "1"
    off = foveation_env("test.game", {"mode": "off"}, baseline={k: v for k, v in env.items() if v})
    assert off["FOVE_INNER"] is None and off["LEPTON_ENV_FRAMEPORT_FOVEATION_TEST"] is None


def test_resolution_label_explains_actual_effect():
    assert UI["scale"]["label"] == "Render resolution (supersampling)"
    assert "render resolution" in UI["scale"]["help"]
    assert "sharper but slower" in UI["scale"]["help"]


def test_game_dialog_saves_personal_choice_without_touching_recipe(monkeypatch):
    import flet as ft

    from frameport.ui.views import adapter_dialog as ad

    recipe = library.recipe_to_dict(Recipe("test.game", patches={"frame.adapter": {}}))
    library.upsert_game("test.game", title="Test", recipe=recipe)
    game = library.game("test.game")
    dialogs = []
    app = SimpleNamespace(page=SimpleNamespace(show_dialog=dialogs.append, pop_dialog=lambda: None),
                          target=None, toast=lambda *a, **kw: None, refresh_view=lambda: None,
                          _title=lambda pkg: "Test")
    ad._open(app, "test.game", game, ad.recipe_values(game))

    def walk(control):
        yield control
        for child in getattr(control, "controls", []) or []:
            yield from walk(child)
        for attr in ("content", "title"):
            child = getattr(control, attr, None)
            if child is not None and not isinstance(child, str):
                yield from walk(child)
        for child in getattr(control, "actions", []) or []:
            yield from walk(child)

    controls = list(walk(dialogs[-1]))
    mode = next(c for c in controls if isinstance(c, ft.Dropdown) and
                any(o.key == "tracked" for o in c.options))
    mode.on_select(SimpleNamespace(control=SimpleNamespace(value="fixed")))
    radius = next(c for c in controls if isinstance(c, ft.Dropdown) and
                  any(o.key == "0.3" for o in c.options))
    radius.on_select(SimpleNamespace(control=SimpleNamespace(value="0.3")))
    advanced_row = next(c for c in controls if isinstance(c, ft.Row) and c.data == "switch" and
                        any(getattr(t, "value", None) == "Show advanced settings" for t in walk(c)))
    advanced = next(c for c in walk(advanced_row) if isinstance(c, ft.Switch))
    advanced.on_change(SimpleNamespace(control=SimpleNamespace(value=True)))
    test_row = next(c for c in controls if isinstance(c, ft.Container) and
                    isinstance(c.content, ft.Row) and any(
                        getattr(t, "value", None) == "Darken coarse fragments (test)" for t in walk(c)))
    assert test_row.visible
    test_switch = next(c for c in walk(test_row) if isinstance(c, ft.Switch))
    test_switch.on_change(SimpleNamespace(control=SimpleNamespace(value=True)))
    # The dialog's primary Save button calls the only recipe-independent preference write.
    save = dialogs[-1].actions[-1]
    save.on_click(None)
    saved = library.game("test.game")
    assert saved["foveation"] == {"mode": "fixed", "radius": .3, "test": True}
    assert saved["recipe"] == game["recipe"]
    ad._open(app, "test.game", saved, ad.recipe_values(saved))
    mode = next(c for c in walk(dialogs[-1]) if isinstance(c, ft.Dropdown) and
                any(o.key == "tracked" for o in c.options))
    mode.on_select(SimpleNamespace(control=SimpleNamespace(value="off")))
    dialogs[-1].actions[-1].on_click(None)
    assert library.game("test.game")["foveation"] == {"mode": "off"}
    assert library.game("test.game")["recipe"] == game["recipe"]


def test_adapter_build_prepares_internal_foveation_support():
    from frameport.build import apk_patches
    assert "device.foveation" in [p.id for p in apk_patches({"frame.adapter": {}})]
    assert "device.foveation" not in [p.id for p in apk_patches({"frame.launcher": {}})]
