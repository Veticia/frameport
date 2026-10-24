"""GitHub #480: Steam's own Exit game (SteamClient.Apps.TerminateApp in Steam's UI) signals a game's whole process tree
at once, so a FramePort game's launcher can't pause it first. The launcher's _exit_hook worker hooks TerminateApp in
Steam's UI through the CEF devtools connection: Exit game for that game waits until the worker paused it."""
import json
import os
import shutil
import subprocess
import sys

import pytest
from test_agent import load_agent
from test_graceful_stop import _lepton_launcher

needs_bash = pytest.mark.skipif(sys.platform == "win32", reason="bash")
NODE = shutil.which("node")
GID = str((7 << 32) | 0x02000000)


def _node(a, scenario):
    """Run `scenario` (async JS) against a fake Steam UI (SteamClient.Apps.TerminateApp records its calls) with the
    agent's hook scripts as functions: install(games, binding, cap, ack, lease), unregister(games, binding),
    ack(gid), done(gid, why), unhooked(gid). Returns what the scenario passes to out()."""
    js = r"""
const vm = require('vm');
const calls = [], bound = [];
const ctx = {setTimeout, clearTimeout, Date, JSON, String};
ctx.window = ctx;
ctx.SteamClient = {Apps: {TerminateApp: function (g, b) {
  calls.push([String(g), b, this === ctx.SteamClient.Apps]); }}};
vm.createContext(ctx);
const ev = (code) => vm.runInContext(code, ctx);
const fill = (t, args) => t.replace('%s', args.map((x) => JSON.stringify(x)).join(', '));
const T = __TEMPLATES__;
const install = (...a) => ev(fill(T.install, a));
const unregister = (...a) => ev(fill(T.unregister, a));
const ack = (g) => ev(fill(T.ack, [g]));
const done = (g, why) => ev(fill(T.done, [g, why]));
const unhooked = (g) => ev(T.unhooked.replace('__GID__', g));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const binding = (name) => { ctx[name] = (payload) => bound.push([name, JSON.parse(payload)]); };
const out = (x) => process.stdout.write(JSON.stringify(x));
(async () => { __SCENARIO__ })().catch((e) => { console.error(e); process.exit(1); });
"""
    templates = {"install": a.EXIT_HOOK_JS, "unregister": a.EXIT_HOOK_UNREGISTER_JS, "ack": a.EXIT_HOOK_ACK_JS,
                 "done": a.EXIT_HOOK_DONE_JS, "unhooked": a.terminate_app_js("__GID__")}
    js = js.replace("__TEMPLATES__", json.dumps(templates)).replace("__SCENARIO__", scenario)
    p = subprocess.run([NODE, "-e", js], capture_output=True, text=True, timeout=30)
    assert p.returncode == 0, p.stderr
    return json.loads(p.stdout)


@pytest.mark.skipif(not NODE, reason="node")
def test_hook_waits_for_the_pause_and_passes_other_games_through(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    r = _node(a, f"""
const G = '{GID}';
binding('fpA');
const states = [install([G], 'fpA', 2000, 300, 10000), install([G], 'fpA', 2000, 300, 10000)];
ctx.SteamClient.Apps.TerminateApp('730', false);          // a Steam game: straight through
const other = calls.length;
ctx.SteamClient.Apps.TerminateApp(G, false);              // ours: the worker is asked, Steam waits
ctx.SteamClient.Apps.TerminateApp(G, false);              // pressed again meanwhile: one exit follows
const waiting = calls.length;
const acked = ack(G);
await sleep(400);                                         // past the answer deadline: acked, so still waiting
const afterAck = calls.length;
const finished = done(G, 'paused');
out({{states, other, waiting, acked, afterAck, finished, again: done(G, 'paused'), calls, bound,
      log: ctx.__fpExit.log}});
""")
    assert r["states"] == ["installed", "ok"]
    assert r["other"] == 1 and r["calls"][0] == ["730", False, True]
    assert r["waiting"] == 1 and r["afterAck"] == 1 and r["acked"] is True
    assert r["bound"] == [["fpA", {"gameid": GID}]]  # once, although Exit game was pressed twice
    assert r["finished"] is True and r["again"] is False
    assert r["calls"][1] == [GID, False, True]  # Steam's own TerminateApp, with its arguments and `this`
    assert r["log"] == [["730", "passed", 0], [GID, "paused", r["log"][1][2]]]


@pytest.mark.skipif(not NODE, reason="node")
def test_hook_never_holds_exit_game_long(monkeypatch, tmp_path):
    """No answer at once (worker gone or hung) → Steam's exit after `ack` ms; answered but never done → after `cap`
    ms; a registration older than its lease, a missing binding or an unregistered game → no wait at all."""
    a = load_agent(monkeypatch, tmp_path)
    r = _node(a, f"""
const G = '{GID}', H = '42949672960';
binding('fpA');
install([G, H], 'fpA', 600, 150, 10000);
ctx.SteamClient.Apps.TerminateApp(G, false);
await sleep(300);
const noAnswer = calls.length;                           // 1: the answer deadline passed
ctx.SteamClient.Apps.TerminateApp(H, false);
ack(H);
await sleep(300);
const acked = calls.length;                              // still 1: answered, below the cap
await sleep(500);
const capped = calls.length;                             // 2: the cap
const late = done(H, 'paused');
ctx.__fpExit.games[G].seen = 0;                          // the worker stopped renewing its lease
ctx.SteamClient.Apps.TerminateApp(G, false);
const stale = calls.length;
install([G], 'fpA', 600, 150, 10000);
delete ctx.fpA;                                          // no binding in this context
ctx.SteamClient.Apps.TerminateApp(G, false);
const unbound = calls.length;
binding('fpA');
const removed = unregister([G, H], 'fpA');
ctx.SteamClient.Apps.TerminateApp(G, false);
out({{noAnswer, acked, capped, late, stale, unbound, removed, after: calls.length, bound: bound.length,
      why: ctx.__fpExit.log.map((l) => l[1])}});
""")
    assert (r["noAnswer"], r["acked"], r["capped"]) == (1, 1, 2)
    assert r["late"] is False
    assert (r["stale"], r["unbound"], r["after"]) == (3, 4, 5)
    assert r["removed"] == 2 and r["bound"] == 2
    assert r["why"] == ["no answer", "cap", "passed", "passed", "passed"]


@pytest.mark.skipif(not NODE, reason="node")
def test_hook_install_is_idempotent_and_steps_aside(monkeypatch, tmp_path):
    """Re-applied every few seconds: once installed it only renews; another game's worker adds its own ids; a script
    that wrapped TerminateApp after us is never wrapped again (no loop); the Monitor's End game (already paused)
    calls Steam's TerminateApp past the hook; no SteamClient (UI loading) → nothing done."""
    a = load_agent(monkeypatch, tmp_path)
    r = _node(a, f"""
const G = '{GID}';
binding('fpA'); binding('fpB');
const s = [install([G], 'fpA', 600, 150, 10000), install(['99'], 'fpB', 600, 150, 10000),
           install([G], 'fpB', 600, 150, 10000)];
unhooked(G);
const direct = [calls.length, bound.length];
const ours = ctx.SteamClient.Apps.TerminateApp;
ctx.SteamClient.Apps.TerminateApp = function (g, b) {{ return ours.apply(this, arguments); }};
s.push(install([G], 'fpA', 600, 150, 10000));
const steam = ctx.SteamClient;
delete ctx.SteamClient;
s.push(install([G], 'fpA', 600, 150, 10000));
ctx.SteamClient = steam;
out({{s, direct, games: Object.keys(ctx.__fpExit.games).sort()}});
""")
    assert r["s"] == ["installed", "registered", "registered", "TerminateApp was replaced by another script",
                      "no SteamClient"]
    assert r["direct"] == [1, 0]
    assert r["games"] == sorted([GID, "99"])


def test_exit_gameids(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    monkeypatch.setattr(a, "devkit_gameid", lambda pkg: None)
    assert a.exit_gameids("com.x.y", "7", env={}) == [GID]
    monkeypatch.setattr(a, "devkit_gameid", lambda pkg: "fp_x")
    monkeypatch.setattr(a, "devkit_appid", lambda gid: 9)
    sgid = str((11 << 32) | 0x02000000)
    assert a.exit_gameids("com.x.y", 7, env={"SteamGameId": sgid}) == sorted(
        [GID, str(a.steam_gameid(9)), sgid])
    assert a.exit_gameids("com.x.y", "x", env={"SteamGameId": "0"}) == [str(a.steam_gameid(9))]


class FakeDevtools:
    def __init__(self, script):
        self.script, self.sent, self.closed = script, [], False

    def call(self, method, params=None, timeout=None):
        self.sent.append((method, params))
        return {}

    def evaluate(self, expr, timeout=None):
        self.sent.append(("eval", expr))
        step = self.script.pop(0)
        if isinstance(step, Exception):
            raise step
        return step

    def event(self, method, timeout):
        return self.script.pop(0) if self.script and isinstance(self.script[0], dict) else None

    def close(self):
        self.closed = True


def test_worker_pauses_then_lets_steam_end_the_game(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    monkeypatch.setattr(a, "exit_gameids", lambda pkg, appid: [GID])
    binding = f"fpExitPause{os.getpid()}"
    first = FakeDevtools(["installed",
                          {"name": "someone-else", "payload": "{}"},
                          {"name": binding, "payload": json.dumps({"gameid": GID})},
                          True, True,  # ack, done
                          a.AgentError("Steam's devtools closed the connection")])
    second = FakeDevtools(["installed", 1])  # Steam restarted: new connection, hook again; unregister at the end
    conns, paused, slept = [first, second], [], []
    rounds = a.exit_hook_worker("com.x.y", 7, os.getpid(), devtools=lambda: conns.pop(0),
                                pause=lambda: paused.append(1) or "state=STOPPED after 2 polls",
                                sleep=slept.append, refresh=0.01, max_rounds=3)
    assert rounds == 3 and paused == [1] and slept == [3] and first.closed and second.closed
    assert first.sent[0] == ("Runtime.addBinding", {"name": binding})
    evals = [s[1] for s in first.sent if s[0] == "eval"]
    assert f'["{GID}"], "{binding}", {a.EXIT_HOOK_CAP_MS}, {a.EXIT_HOOK_ACK_MS}' in evals[0]
    assert "p.ack()" in evals[1] and "p.finish(why)" in evals[2] and '"paused"' in evals[2]
    assert second.sent[0][0] == "Runtime.addBinding"
    assert "delete H.games[gid]" in [s[1] for s in second.sent if s[0] == "eval"][-1]


def test_worker_ends_with_its_launcher(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    monkeypatch.setattr(a, "exit_gameids", lambda pkg, appid: [GID])
    gone = subprocess.Popen(["true"])
    gone.wait()
    assert a.exit_hook_worker("com.x.y", 7, gone.pid, devtools=lambda: pytest.fail("connected")) == 0
    with pytest.raises(a.AgentError):
        a.exit_hook_worker("x'; reboot", 7, os.getpid())


@needs_bash
def test_exit_hook_line_follows_the_template_once(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    _, text = _lepton_launcher(a, tmp_path, "/opt/lepton")
    text = a.set_exit_hook_line(text, "com.x.y")
    assert text.count(" _exit_hook ") == 1
    hook = text.index(' _exit_hook com.x.y "$SteamAppId" $$')
    assert text.index("\nchild=$!\n") < hook < text.index('\nwait "$child"')
    assert a.set_exit_hook_line(text, "com.x.y") == text
    assert subprocess.run(["bash", "-n"], input=text, text=True).returncode == 0
    proton = "#!/bin/bash\n# Proton\nexec /x/proton run game.exe\n"
    assert a.set_exit_hook_line(proton, "rift.game") == proton


@needs_bash
def test_upgrade_launchers_adds_the_exit_hook_to_lepton_launchers(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    monkeypatch.setattr(a, "deployment", lambda pkg: {})
    _, text = _lepton_launcher(a, tmp_path, "/opt/lepton", pkg="com.Ubisoft.ACNexusVR")
    old = text.replace("#!/usr/bin/env bash\n", "#!/usr/bin/env bash\nexport FOVE_LEVEL=lo\n", 1)
    anchors = tmp_path / "Applications/quest-frame"
    (anchors / "com.Ubisoft.ACNexusVR").mkdir(parents=True)
    path = anchors / "com.Ubisoft.ACNexusVR/launch.sh"
    path.write_text(old)
    assert "com.Ubisoft.ACNexusVR" in a.upgrade_launchers()
    new = path.read_text()
    assert new.startswith("#!/usr/bin/env bash\nexport FOVE_LEVEL=lo\n")
    assert new.count("_exit_hook com.Ubisoft.ACNexusVR") == 1
    assert a.upgrade_launchers() == [] and path.read_text() == new
