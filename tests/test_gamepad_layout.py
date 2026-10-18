"""device.steam_gamepad's own button layout (GitHub #162: Steam Input's per-game remapping doesn't reach a 2D app):
the patch's options become launcher env, the agent's Podman wrapper turns them into the app's key layout."""
import pytest
from test_steam_gamepad import ENV, NEEDS_POSIX, RUN, agent_module, frame_inputs

from frameport.patches import base
from frameport.patches.base import InstallContext


def install_env(**params):
    ctx = InstallContext(package="org.smarttube.stable", params=params, files={}, env={}, adapter_settings={})
    base.get("device.steam_gamepad").install(ctx)
    return ctx.env


def test_options_become_launcher_env():
    assert install_env() == {}
    assert install_env(swap_sticks=True, swap_ab=True, keys=" key 304  DPAD_DOWN ;; key 305 BACK") == {
        "FRAMEPORT_GAMEPAD_LAYOUT": "swap_sticks,swap_ab", "FRAMEPORT_GAMEPAD_KEYS": "key 304 DPAD_DOWN; key 305 BACK"}
    with pytest.raises(ValueError):
        install_env(keys="key 304 DPAD_DOWN; rm -rf /")


def test_layout_options(monkeypatch, tmp_path):
    agent = agent_module(monkeypatch, tmp_path)
    assert agent.gamepad_kl() == (agent.GAMEPAD_KL, [])
    text, skipped = agent.gamepad_kl("swap_sticks,swap_ab", "key 304 DPAD_DOWN; key 316 HOME; axis 0x10 HAT_X; nope")
    lines = text.splitlines()
    assert skipped == ["nope"]
    assert "axis 0x00 Z flat 4096" in lines and "axis 0x03 X flat 4096" in lines  # right stick = main stick
    assert "axis 0x01 RZ flat 4096" in lines and "axis 0x04 Y flat 4096" in lines
    assert "key 305   BUTTON_A" in lines
    assert "key 304 DPAD_DOWN" in lines and "key 304   BUTTON_B" not in lines  # own lines replace that key's line
    assert "key 316 HOME" in lines and sum(ln.startswith("key 316") for ln in lines) == 1
    assert len([ln for ln in lines if not ln.startswith("#")]) == len(agent.GAMEPAD_KL.splitlines()) - 1


@NEEDS_POSIX
def test_wrapper_mounts_the_games_own_layout(monkeypatch, tmp_path):
    agent = agent_module(monkeypatch, tmp_path)
    frame_inputs(tmp_path)
    env = dict(ENV, FRAMEPORT_GAMEPAD_LAYOUT="swap_sticks", FRAMEPORT_GAMEPAD_KEYS="key 304 DPAD_DOWN")
    out = agent.podman_run_args(RUN, env, str(tmp_path / "sys"), str(tmp_path / "dev"))
    mount = next(a for a in out if "Vendor_28de_Product_11ff.kl" in a)
    source = mount.split("source=")[1].split(",")[0]
    assert source.endswith(".kl") and "steam-gamepad-" in source
    text = open(source).read()
    assert "key 304 DPAD_DOWN" in text and "axis 0x03 X flat 4096" in text
    # the default layout keeps its shared file
    out = agent.podman_run_args(RUN, ENV, str(tmp_path / "sys"), str(tmp_path / "dev"))
    assert any("agent/steam-gamepad.kl," in a for a in out)
