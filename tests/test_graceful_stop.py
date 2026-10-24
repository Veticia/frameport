"""GitHub #480: a Lepton game's activity is paused (sent to the background inside its container) before the game is
ended, so Unity's OnApplicationPause(true) and the saves games write there run (Just Dance VR lost all progress)."""
import os
import subprocess
import sys
import time
from types import SimpleNamespace

import pytest
from test_agent import load_agent

needs_bash = pytest.mark.skipif(sys.platform == "win32", reason="bash/sh")


def _lepton_launcher(a, tmp_path, lepton, pkg="com.x.y", appid=7):
    base = tmp_path / "game"
    (base / "lepton-app").mkdir(parents=True, exist_ok=True)
    text = a.LAUNCH_SH.format(title="T", pkg=pkg, app_dir=f"app_dir={base}", appid=appid, lepton_q=str(lepton),
                              extra_env="", cloud_dirs="true", watchdog=a.WATCHDOG, dashboard="true",
                              logcat="true", single=a.SINGLE_LINE, plays_start="true", plays_end="true",
                              save_snapshot="echo snapshot >>$app_dir/../events", video_codec="",
                              restart='wait "$child"')
    return base, a.set_pause_lines(text, pkg)


@needs_bash
def test_launcher_pauses_the_game_before_lepton_and_the_container_end(monkeypatch, tmp_path):
    """Stopping the launcher (Steam's Exit game, the parent watchdog) first sends the game's activity to the
    background inside its container, then ends Lepton (whose teardown kills the container), then podman kill, then
    the save snapshot. A game that ended by itself isn't paused (nothing to wait for)."""
    a = load_agent(monkeypatch, tmp_path)
    events = tmp_path / "events"
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    (bin_dir / "podman").write_text(f'#!/bin/bash\necho "podman $1 $2" >>{events}\n')
    (bin_dir / "podman").chmod(0o755)
    lepton = tmp_path / "lepton"
    lepton.write_text(f"#!/bin/bash\ntrap 'echo lepton-term >>{events}; exit 0' TERM\necho started >>{events}\n"
                      "sleep ${FAKE_RUN:-30} & wait\n")
    lepton.chmod(0o755)
    base, text = _lepton_launcher(a, tmp_path, lepton)
    assert subprocess.run(["bash", "-n"], input=text, text=True).returncode == 0
    launcher = tmp_path / "launch.sh"
    launcher.write_text(text)
    launcher.chmod(0o755)
    env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}"}
    first = subprocess.Popen([str(launcher)], env=env, start_new_session=True)
    for _ in range(200):
        if events.exists() and "started" in events.read_text():
            break
        time.sleep(0.05)
    first.terminate()
    first.wait(timeout=10)
    got = events.read_text().split("\n")
    assert got[:5] == ["started", "podman exec lepton-steamlaunch-7", "lepton-term",
                       "podman kill lepton-steamlaunch-7", "snapshot"]
    assert "pausing" in (base / "stop-pause.log").read_text()
    events.unlink()
    subprocess.run([str(launcher)], env={**env, "FAKE_RUN": "0"}, timeout=20)
    assert "podman exec" not in events.read_text() and "podman kill" in events.read_text()


def test_pause_lines_follow_the_template_once(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    _, text = _lepton_launcher(a, tmp_path, "/opt/lepton")
    assert text.count("pause_game() {") == 1 and text.count("\n    pause_game\n") == 1
    assert text.index("pause_game() {") < text.index("\nstop() {")
    assert text.index("\n    pause_game\n") < text.index('[[ -n "$child" ]] && kill -TERM')
    assert a.set_pause_lines(text, "com.x.y") == text
    assert f"timeout -k 1 {a.PAUSE_LIMIT} podman exec" in text
    # a template change replaces the old line instead of adding a second one
    old = text.replace(f"timeout -k 1 {a.PAUSE_LIMIT} ", "timeout -k 1 9 ")
    assert a.set_pause_lines(old, "com.x.y") == text
    with pytest.raises(a.AgentError):
        a.pause_script("x'; reboot; '")


@needs_bash
def test_upgrade_launchers_adds_the_pause_to_lepton_launchers_only(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    monkeypatch.setattr(a, "deployment", lambda pkg: {})
    _, text = _lepton_launcher(a, tmp_path, "/opt/lepton", pkg="com.Ubisoft.ACNexusVR")
    old = "\n".join(ln for ln in text.split("\n") if "pause_game" not in ln)
    old = old.replace("#!/usr/bin/env bash\n", "#!/usr/bin/env bash\nexport FOVE_LEVEL=lo\n", 1)
    anchors = tmp_path / "Applications/quest-frame"
    (anchors / "com.Ubisoft.ACNexusVR").mkdir(parents=True)
    path = anchors / "com.Ubisoft.ACNexusVR/launch.sh"
    path.write_text(old)
    (anchors / "rift.game").mkdir()
    (anchors / "rift.game/launch.sh").write_text("#!/bin/bash\n# Proton\nexec /x/proton run game.exe\n")
    assert "com.Ubisoft.ACNexusVR" in a.upgrade_launchers()
    new = path.read_text()
    assert "export FOVE_LEVEL=lo\n" in new and "com.Ubisoft.ACNexusVR" in new.split("pause_game() {")[1]
    assert new.count("pause_game") == 2 and subprocess.run(["bash", "-n", str(path)]).returncode == 0
    assert "pause_game" not in (anchors / "rift.game/launch.sh").read_text()
    a.upgrade_launchers()
    assert path.read_text() == new


@needs_bash
@pytest.mark.parametrize("states, out, home", [
    (["RESUMED", "PAUSED", "STOPPED"], "state=STOPPED after 1 polls", True),
    (["RESUMED", "PAUSING", "PAUSED", ""], "gone after 2 polls", True),
    (["STOPPED"], "not in front (state=STOPPED)", False),
    ([""], "not in front (no activity)", False),
])
def test_pause_script_sends_the_game_home_and_waits_until_it_stopped(monkeypatch, tmp_path, states, out, home):
    """The in-container script against fake dumpsys/am, shaped like Android 11's output in Lepton (`* Hist #0:
    ActivityRecord{... <pkg>/<activity> t7}` ... `state=RESUMED stopped=false ...`; the launcher's record follows)."""
    a = load_agent(monkeypatch, tmp_path)
    pkg = "com.Ubisoft.ACNexusVR"
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    st = tmp_path / "states"
    st.write_text("\n".join(states) + "\n")
    (bin_dir / "dumpsys").write_text(f"""#!/bin/sh
s=$(head -n 1 {st}); n=$(wc -l < {st})
[ "$n" -gt 1 ] && sed -i 1d {st}
echo "    mResumedActivity: ActivityRecord{{1 u0 com.android.launcher3/.Launcher t6}}"
if [ -n "$s" ]; then
  echo "      * Hist #0: ActivityRecord{{14ecec1 u0 {pkg}/com.unity3d.player.UnityPlayerActivity t7}}"
  echo "        app=ProcessRecord{{1 1200:{pkg}/u0a1}}"
  echo "          state=$s stopped=false delayedResume=false finishing=false"
fi
echo "      * Hist #0: ActivityRecord{{74a0076 u0 com.android.launcher3/.Launcher t6}}"
echo "          state=STOPPED stopped=true"
""")
    (bin_dir / "am").write_text(f'#!/bin/sh\necho "$@" >{tmp_path}/am\n')
    for f in bin_dir.iterdir():
        f.chmod(0o755)
    monkeypatch.setattr(a, "PAUSE_SETTLE", 0)
    p = subprocess.run(["sh", "-c", a.pause_script(pkg)], capture_output=True, text=True, timeout=20,
                       env={**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}"})
    assert p.returncode == 0 and p.stdout.strip() == out
    assert (tmp_path / "am").exists() == home
    if home:
        assert "android.intent.category.HOME" in (tmp_path / "am").read_text()


def test_stop_pauses_a_lepton_game_before_it_is_killed(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    calls = []
    monkeypatch.setattr(a, "deployment", lambda pkg: {"appid": 5, "base": "/b"})
    monkeypatch.setattr(a, "container_running", lambda appid: True)
    monkeypatch.setattr(a, "run", lambda cmd, **kw: calls.append(cmd) or SimpleNamespace(
        returncode=0, stdout="state=STOPPED after 3 polls\n", stderr=""))
    assert a.cmd_stop({"package": "com.x.y"}) == {"stopped": True, "paused": "state=STOPPED after 3 polls"}
    assert calls[0][:3] == ["podman", "exec", "lepton-steamlaunch-5"] and "com.x.y" in calls[0][-1]
    assert calls[1][:3] == ["systemctl", "--user", "stop"] and calls[2] == ["podman", "kill", "lepton-steamlaunch-5"]

    def hung(cmd, **kw):  # a container that doesn't answer is cut off: the stop goes on
        calls.append(cmd)
        if cmd[:2] == ["podman", "exec"]:
            assert kw["timeout"] == a.PAUSE_LIMIT
            raise subprocess.TimeoutExpired(cmd, kw["timeout"])
        return SimpleNamespace(returncode=0, stdout="", stderr="")
    calls.clear()
    monkeypatch.setattr(a, "run", hung)
    assert a.cmd_stop({"package": "com.x.y"}) == {"stopped": True, "paused": None}
    assert calls[-1] == ["podman", "kill", "lepton-steamlaunch-5"]
    calls.clear()  # Proton games aren't Android apps
    monkeypatch.setattr(a, "deployment", lambda pkg: {"appid": 5, "base": "/b", "kind": "pcvr"})
    monkeypatch.setattr(a, "stop_pcvr", lambda dep: None)
    assert a.cmd_stop({"package": "com.x.y"}) == {"stopped": True}
    assert not any(c[:2] == ["podman", "exec"] for c in calls)
