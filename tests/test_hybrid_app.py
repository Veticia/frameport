"""device.hybrid_app (GitHub #484): a Horizon OS hybrid app (YouTube) gets the system feature it checks before it
switches from its 2D panel into VR, as a permissions file FramePort's Podman wrapper mounts into its container."""
from types import SimpleNamespace

import pytest
from test_steam_gamepad import ENV, LEPTON, NEEDS_POSIX, RUN, agent_module, anchor, frame_inputs

from frameport.patches import base
from frameport.patches.base import InstallContext
from frameport.validate import triage

FEATURE = "oculus.software.vr.app.hybrid"
FEATURES_ENV = {"FRAMEPORT_FEATURES": FEATURE, "SteamAppId": "123"}


def test_patch_is_offered_only_to_hybrid_apps_and_sets_the_feature():
    patch = base.get("device.hybrid_app")
    hybrid = SimpleNamespace(package="com.google.android.apps.youtube.vr.oculus", extra={"features": {FEATURE: False}})
    assert patch.applies(hybrid) and not patch.applies(SimpleNamespace(package="com.example.game", extra={}))
    assert patch.detect(hybrid) is None  # opt-in: whether the VR part runs depends on the app
    ctx = InstallContext(package="x.y", params={}, files={}, env={}, adapter_settings={})
    patch.install(ctx)
    assert ctx.env == {"FRAMEPORT_FEATURES": FEATURE}


@NEEDS_POSIX
def test_run_args_get_the_permissions_file(monkeypatch, tmp_path):
    agent = agent_module(monkeypatch, tmp_path)
    out = agent.podman_run_args(RUN, FEATURES_ENV, str(tmp_path / "sys"), str(tmp_path / "dev"))
    assert out[:2] == ["run", "--mount"] and out[3:] == RUN[1:]
    source = out[2].split(",")[1].partition("=")[2]
    assert out[2] == f"type=bind,source={source},destination={agent.FEATURES_TARGET},ro"
    text = open(source).read()
    assert f'<feature name="{FEATURE}" />' in text and text.startswith("<?xml")
    # names only: anything else in the variable is left out
    assert agent.system_features(f'{FEATURE}, x" /><evil, a.b') == ["a.b", FEATURE]
    unchanged = dict(FEATURES_ENV, FRAMEPORT_FEATURES='x" />')
    assert agent.podman_run_args(RUN, unchanged, str(tmp_path / "sys"), str(tmp_path / "dev")) == RUN
    assert agent.podman_run_args(RUN, dict(FEATURES_ENV, SteamAppId="9"), "/x", "/x") == RUN  # another container


@NEEDS_POSIX
def test_features_and_gamepad_together(monkeypatch, tmp_path):
    agent = agent_module(monkeypatch, tmp_path)
    frame_inputs(tmp_path)
    out = " ".join(agent.podman_run_args(RUN, {**ENV, **FEATURES_ENV}, str(tmp_path / "sys"), str(tmp_path / "dev")))
    assert "destination=/dev/fp-input,rw" in out and "Vendor_28de_Product_11ff.kl,ro" in out
    assert f"destination={agent.FEATURES_TARGET},ro" in out


@pytest.mark.parametrize("hw_video,gamepad", [(False, False), (True, False), (True, True)])
def test_launchers_get_the_features_line_last(monkeypatch, tmp_path, hw_video, gamepad):
    agent = agent_module(monkeypatch, tmp_path)
    directory = tmp_path / "apps/game"
    directory.mkdir(parents=True)
    agent.write_launcher(str(directory), str(tmp_path / "base"), "com.example.game", "Game", 123, "/lepton/lepton",
                         {"FRAMEPORT_FEATURES": FEATURE}, hw_video=hw_video, gamepad=gamepad, features=True)
    text = (directory / "launch.sh").read_text()
    assert text.count(agent.FEATURES_LINE) == 1 and f"export FRAMEPORT_FEATURES={FEATURE}\n" in text
    for line in (agent.VIDEO_CODEC_LINE, agent.GAMEPAD_LINE):
        if line in text:  # FramePort's wrapper first on PATH
            assert text.index(line) < text.index(agent.FEATURES_LINE)
    upgraded = agent.set_features_line(agent.set_gamepad_line(agent.set_codec_line(text, hw_video), gamepad), True)
    assert upgraded == text  # upgrades keep it


def test_upgrade_adds_the_line_after_a_codec_added_later(monkeypatch, tmp_path):
    agent = agent_module(monkeypatch, tmp_path)
    plain = LEPTON.format(lines=f"export FRAMEPORT_FEATURES={FEATURE}\n" + agent.FEATURES_LINE + "\n")
    directory = anchor(tmp_path, "com.game", plain, recipe={"patches": ["frame.hw_video_decode"]})
    assert agent.upgrade_launchers() == ["com.game"]
    text = (directory / "launch.sh").read_text()
    assert text.index(agent.VIDEO_CODEC_LINE) < text.index(agent.FEATURES_LINE)
    assert text.count(agent.FEATURES_LINE) == 1 and agent.upgrade_launchers() == []
    # the patch turned off: the next launcher has neither the export nor the line
    gone = anchor(tmp_path, "com.other", LEPTON.format(lines=agent.FEATURES_LINE + "\n"))
    agent.upgrade_launchers()
    assert agent.FEATURES_LINE not in (gone / "launch.sh").read_text()


def test_finalize_uses_the_wrapper_for_features():
    from pathlib import Path
    source = (Path(__file__).resolve().parents[1] / "agent/frameport_agent.py").read_text()
    assert "if gamepad or features:\n        ensure_podman_wrapper()" in source


def test_triage_suggests_the_patch():
    log = ("10-25 05:18:21.264  1220  1220 W gob     : "
           "Missing hybrid app system feature. Aborting launch of immersive mode.")
    result = triage.triage(log, "RUNNING")
    assert "hybrid-feature-missing" in [f.id for f in result.findings]
    assert "device.hybrid_app" in result.suggestions()
