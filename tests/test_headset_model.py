"""Per-game identity, default migration, UI choice and host OpenXR rewrite."""
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

from frameport.core import library
from frameport.core.models import Recipe
from frameport.game_update import apk_recipe_fingerprint, configuration, configuration_fingerprint
from frameport.install.installer import install_context
from frameport.patches import base
from frameport.patches.frame.adapter import settings_text
from frameport.patches.frame.headset_model import MODELS, HeadsetModel, migrate
from frameport.recommend import engine


@pytest.mark.parametrize("key", MODELS)
def test_identity_layers(key):
    patches = {HeadsetModel.id: {"model": key}, "device.hide_navbar": {}, "device.language": {"tag": "de-DE"}}
    ctx = install_context(Recipe("test.game", patches=patches))
    number, _ = MODELS[key]
    assert ctx.adapter_settings["headset_model"] == number
    assert configuration({"patches": patches})["settings"]["headset_model"] == number
    if key == "quest2":
        assert settings_text(patches) == settings_text({k: v for k, v in patches.items() if k != HeadsetModel.id})
    else:
        assert f"headset_model={number}\n".encode() in settings_text(patches)
    assert ctx.env["LEPTON_GFXRECON_FP_PROPS"] == "0\nqemu.hw.mainkeys=1"
    assert ctx.env["LEPTON_GFXRECON_FP_LOCALE"] == "0\npersist.sys.locale=de-DE"
    assert "LEPTON_GFXRECON_FP_HEADSET" not in ctx.env


@pytest.mark.parametrize("value", ["unknown", "quest3\nro.debuggable=1", 3, None])
def test_invalid_choices(value):
    with pytest.raises(ValueError):
        install_context(Recipe("test.game", patches={HeadsetModel.id: {"model": value}}))


@pytest.mark.parametrize("extra,enabled", [({}, True), ({"vr_kind": "quest"}, True),
                                         ({"kind": "rift"}, False), ({"kind": "linux"}, False),
                                         ({"vr_kind": "none"}, False), ({"vr_kind": "other"}, False)])
def test_default_scope(extra, enabled):
    analysis = library.analysis_from_dict({"package": "test.game", "extra": extra})
    patch = base.get(HeadsetModel.id)
    assert patch.default_on
    assert patch.applies(analysis) == enabled
    assert (HeadsetModel.id in engine.suggest(analysis, use_catalog=False).patches) == enabled


def test_migration_preserves_user_choices_and_excludes_other_apps():
    def game(extra=None, kind=None):
        return {"analysis": {"extra": extra or {}}, "recipe": {"patches": {}}, "kind": kind}

    games = {"quest": game(), "chosen": game(), "rift": game({"kind": "rift"}),
             "linux": game(kind="linux"), "flat": game({"vr_kind": "none"})}
    games["chosen"]["recipe"]["patches"][HeadsetModel.id] = {"model": "quest2"}
    data = {"games": games}
    migrate(data)
    assert games["quest"]["recipe"]["patches"][HeadsetModel.id] == {}
    assert games["chosen"]["recipe"]["patches"][HeadsetModel.id] == {"model": "quest2"}
    assert all(HeadsetModel.id not in games[k]["recipe"]["patches"] for k in ("rift", "linux", "flat"))
    library._migrate(data)
    assert "headset_model_quest3" in data["settings"]["migrations"]
    games["quest"]["recipe"]["patches"][HeadsetModel.id] = {"model": "questpro"}
    assert not library._migrate(data)
    assert games["quest"]["recipe"]["patches"][HeadsetModel.id] == {"model": "questpro"}


def test_new_default_requires_adapter_delivery():
    before = {"patches": {"frame.adapter": {}}}
    after = {"patches": {"frame.adapter": {}, HeadsetModel.id: {}}}
    assert apk_recipe_fingerprint(before) != apk_recipe_fingerprint(after)
    assert configuration_fingerprint(before) != configuration_fingerprint(after)


def test_dropdown_keeps_quest3_enabled(monkeypatch):
    import flet as ft

    from frameport.ui.views import game

    patch = base.get(HeadsetModel.id)
    assert set(dict(patch.CHOICES)) == set(MODELS)
    monkeypatch.setattr(game.engine, "visible_patches", lambda *a: ([patch], []))
    monkeypatch.setattr(library, "analysis_from_dict", lambda a: None)
    monkeypatch.setattr(library, "setting", lambda *a: False)
    monkeypatch.setattr(game.engine, "warnings", lambda *a: [])
    saved = []
    monkeypatch.setattr(game.pipeline, "set_recipe", lambda package, r: saved.append(library.recipe_to_dict(r)))
    recipe = Recipe("test.game", patches={HeadsetModel.id: {"model": "quest2"}})
    view = SimpleNamespace(app=SimpleNamespace(has_game_settings=lambda g: False),
                           g={"recipe": library.recipe_to_dict(recipe), "analysis": {}},
                           package="test.game", show_all=False)

    def walk(control):
        yield control
        for child in getattr(control, "controls", []) or []:
            yield from walk(child)
        if getattr(control, "content", None) is not None:
            yield from walk(control.content)

    picker = next(c for c in walk(game.GameView.advanced_panel(view)) if isinstance(c, ft.Dropdown))
    assert picker.value == "quest2"
    picker.on_select(SimpleNamespace(control=SimpleNamespace(value="")))
    assert saved[-1]["patches"][HeadsetModel.id] == {}


def test_system_name_host(tmp_path):
    root = Path(__file__).resolve().parents[1]
    cc = shutil.which("cc") or shutil.which("clang")
    inc = next((root / "native/.cache").glob("openxr-*"), None)
    if not cc or not inc:
        pytest.skip("needs host C compiler and cached OpenXR headers")
    source = tmp_path / "headset_model_test.c"
    source.write_text(r'''
#include <openxr/openxr.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "headset_model.c"
int main(void) {
    XrSystemProperties p = {0};
    p.vendorId = 10291;
    p.graphicsProperties.maxLayerCount = 16;
    int sentinel;
    p.next = &sentinel;
    strcpy(p.systemName, "Oculus Quest2");
    headset_model_properties(&p);
    assert(!strcmp(p.systemName, "Oculus Quest2"));
    const char *names[] = {"Meta Quest 3", "Meta Quest 3S", "Meta Quest Pro"};
    for (int i = 0; i < 3; ++i) {
        char line[32];
        snprintf(line, sizeof(line), "headset_model=%d\n", i + 3);
        headset_model_setting(line);
        headset_model_properties(&p);
        assert(!strcmp(p.systemName, names[i]));
        assert(p.vendorId == 10291 && p.graphicsProperties.maxLayerCount == 16 && p.next == &sentinel);
    }
    headset_model_setting("headset_model=2\n");
    strcpy(p.systemName, "Oculus Quest2");
    headset_model_properties(&p);
    assert(!strcmp(p.systemName, "Oculus Quest2"));
    headset_model_setting("headset_model=99\n");
    headset_model_setting("headset_model=3oops\n");
    assert(headset_model == 2);
    headset_model_properties(NULL);
    return 0;
}
''')
    exe = tmp_path / "headset_model_test"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(inc),
                    "-I", str(root / "native/adapter"), str(source), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def dispatcher_blob():
    from frameport.patches.frame.headset_model import GUARDS, NAME_OFFSET, UUID_OFFSET, UUIDS
    blob = bytearray(b'\xa5' * (UUID_OFFSET + 32))
    for off, value in {**GUARDS, NAME_OFFSET: b'Oculus Quest2\0', UUID_OFFSET: UUIDS['quest2']}.items():
        blob[off:off + len(value)] = value
    return bytes(blob)


@pytest.mark.parametrize('key', MODELS)
def test_dispatcher_exact_edit(key):
    from frameport.patches.frame.headset_model import NAME_OFFSET, SYSTEM_NAMES, UUID_OFFSET, UUIDS, patch_dispatcher
    original = dispatcher_blob()
    fixed = patch_dispatcher(original, key)
    assert len(fixed) == len(original)
    if key == 'quest2':
        assert fixed == original
    else:
        assert fixed[NAME_OFFSET:NAME_OFFSET + 14] == SYSTEM_NAMES[key].ljust(14, b'\0')
        assert fixed[UUID_OFFSET:UUID_OFFSET + 16] == UUIDS[key]
        assert all(a == b for i, (a, b) in enumerate(zip(original, fixed, strict=True))
                   if not (NAME_OFFSET <= i < NAME_OFFSET + 14 or UUID_OFFSET <= i < UUID_OFFSET + 16))


def test_dispatcher_rejects_unknown_and_partial_matches():
    from frameport.patches.frame.headset_model import GUARDS, NAME_OFFSET, UUID_OFFSET, patch_dispatcher
    for offset in [*GUARDS, NAME_OFFSET, UUID_OFFSET]:
        blob = bytearray(dispatcher_blob())
        blob[offset] ^= 1
        with pytest.raises(ValueError, match='Unsupported OVRPort'):
            patch_dispatcher(bytes(blob), '')
    with pytest.raises(ValueError, match='Unsupported OVRPort'):
        patch_dispatcher(b'', '')
    assert patch_dispatcher(b'unknown runtime', 'quest2') == b'unknown runtime'


def test_real_dispatcher_when_available():
    import os

    from frameport.core.paths import user_data_dir
    from frameport.patches.frame.headset_model import patch_dispatcher
    default = user_data_dir() / 'overport-workspace/libraries/3.4.3-aa54c3f/lib/arm64-v8a/libopenxr_loader.so'
    path = Path(os.environ.get('FRAMEPORT_TEST_DISPATCHER', default))
    if not path.is_file():
        pytest.skip('verified dispatcher not available; set FRAMEPORT_TEST_DISPATCHER')
    original = path.read_bytes()
    for key in MODELS:
        fixed = patch_dispatcher(original, key)
        assert len(fixed) == len(original)


@pytest.mark.parametrize('key', MODELS)
def test_apk_stage_patches_dispatcher_and_embeds_settings(tmp_path, key):
    import zipfile

    from frameport.apk.workspace import ApkWorkspace
    from frameport.build import apply_frame_fixes
    from frameport.core.events import Reporter
    from frameport.patches.frame.headset_model import patch_dispatcher

    apk = tmp_path / 'input.apk'
    original = dispatcher_blob()
    with zipfile.ZipFile(apk, 'w') as z:
        z.writestr('lib/arm64-v8a/libopenxr_loader.so', original)
        z.writestr('lib/arm64-v8a/libopenxr_loader_generic.so', b'original downstream loader')
        z.writestr('assets/game', b'untouched asset')
    patches = {'frame.adapter': {}, HeadsetModel.id: {'model': key}}
    recipe = Recipe('test.game', patches=patches)
    analysis = library.analysis_from_dict({'package': 'test.game', 'abis': ['arm64-v8a']})
    output = tmp_path / 'output.apk'
    applied, _, _ = apply_frame_fixes(apk, output, analysis, recipe, Reporter())
    assert (HeadsetModel.id in applied) == (key != 'quest2')
    with ApkWorkspace(output) as ws:
        assert ws.read_lib('libopenxr_loader.so') == patch_dispatcher(original, key)
        assert ws.read_lib('libframe_settings.so') == settings_text(patches)
        assert ws.read('assets/game') == b'untouched asset'


def test_identity_triage():
    from frameport.validate.triage import triage
    findings = triage('OVRPlugin: venderId 10291, systemName Oculus Quest2').findings
    assert any(f.id == 'quest2-headset-identity' for f in findings)
    assert not any(f.id == 'quest2-headset-identity'
                   for f in triage('OVRPlugin: venderId 10291, systemName Meta Quest 3').findings)
