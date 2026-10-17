"""device.language (GitHub #354): the game's Android language, passed to Lepton as persist.sys.locale."""
import pytest

from frameport.core.models import Recipe
from frameport.install.installer import install_context
from frameport.patches import base
from frameport.patches.settings import LANGUAGE_TAG, Language


def _env(patches):
    base.load_all()
    return install_context(Recipe(package="com.example.game", patches=patches)).env


def test_language_rides_in_its_own_boot_property_line():
    env = _env({"device.language": {"tag": "de-DE"}, "device.hide_navbar": {}})
    assert env["LEPTON_GFXRECON_FP_LOCALE"] == "0\npersist.sys.locale=de-DE"
    assert env["LEPTON_GFXRECON_FP_PROPS"] == "0\nqemu.hw.mainkeys=1"  # both can be on


def test_default_changes_nothing():
    assert "LEPTON_GFXRECON_FP_LOCALE" not in _env({"device.language": {"tag": ""}})


@pytest.mark.parametrize("tag", ["de-DE\nro.debuggable=1", "de DE", "../x", "DE"])
def test_only_language_tags_reach_the_boot_properties(tag):
    with pytest.raises(ValueError):
        _env({"device.language": {"tag": tag}})


def test_choices_are_valid_tags():
    assert all(not k or LANGUAGE_TAG.match(k) for k, _label in Language.CHOICES)
    assert Language.CHOICES[0][0] == "" and len({k for k, _ in Language.CHOICES}) == len(Language.CHOICES)


def test_language_dropdown_is_visible_and_default_removes_recipe_patch(monkeypatch):
    from types import SimpleNamespace

    import flet as ft

    from frameport.core import library
    from frameport.ui.views import game

    base.load_all()
    analysis = library.analysis_from_dict({"package": "test.game", "engine": "Unreal"})
    # Use the real visibility filter: the language setting must be offered even while disabled.
    from frameport.recommend import engine
    patch = base.get("device.language")
    shown, _ = engine.visible_patches(analysis, Recipe("test.game"))
    assert patch in shown
    monkeypatch.setattr(game.engine, "visible_patches", lambda *a: ([patch], []))
    monkeypatch.setattr(library, "analysis_from_dict", lambda a: None)
    monkeypatch.setattr(library, "setting", lambda *a: False)
    monkeypatch.setattr(game.engine, "warnings", lambda *a: [])
    saved = []
    monkeypatch.setattr(game.pipeline, "set_recipe", lambda package, r: saved.append(library.recipe_to_dict(r)))
    view = SimpleNamespace(app=SimpleNamespace(has_game_settings=lambda g: False),
                           g={"recipe": library.recipe_to_dict(Recipe("test.game")), "analysis": {}},
                           package="test.game", show_all=False)
    panel = game.GameView.advanced_panel(view)

    def walk(control):
        yield control
        for child in getattr(control, "controls", []) or []:
            yield from walk(child)
        if getattr(control, "content", None) is not None:
            yield from walk(control.content)

    picker = next(c for c in walk(panel) if isinstance(c, ft.Dropdown))
    assert picker.value == ""
    assert any(o.key == "de-DE" for o in picker.options)
    picker.on_select(SimpleNamespace(control=SimpleNamespace(value="de-DE")))
    assert saved[-1]["patches"]["device.language"] == {"tag": "de-DE"}
    assert library.recipe_from_dict(saved[-1]).params("device.language") == {"tag": "de-DE"}
    picker.on_select(SimpleNamespace(control=SimpleNamespace(value="")))
    assert "device.language" not in saved[-1]["patches"]
