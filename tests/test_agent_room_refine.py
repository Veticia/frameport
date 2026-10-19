"""Scan clean-up on a temporary fake Frame: refine.json, the options every build passes, room_refine (a fake
fp_roomscan script writes the scanner interface's files) and the player's own furniture kept over a new clean-up."""
import json
import os
import sys
from pathlib import Path

import pytest
from test_agent import load_agent

from frameport.install.rooms import REFINE_DEFAULTS
from frameport.mr.room import Mesh, Volume, box_room, dumps, parse, write_mesh

# A stand-in for fp_roomscan (scanner interface of docs/mr): --offline <dump> --out <dir> builds a room; --refine-only
# <dir> cleans one up again from room-raw.txt (an old room without raw files: room.txt becomes the raw one). It writes
# room.txt + room-mesh.bin (refined), room-raw.txt + room-mesh-raw.bin, scan.json, and logs its arguments.
FAKE_SCANNER = r'''#!PYTHON
import json, os, shutil, struct, sys, uuid
argv = sys.argv[1:]
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "scanner-calls.jsonl"), "a") as log:
    log.write(json.dumps(sys.argv) + "\n")
def opt(name, default=None):
    return argv[argv.index(name) + 1] if name in argv else default
if os.environ.get("FAKE_SCANNER_FAIL"):
    sys.stderr.write("boom")
    sys.exit(3)
mesh = (struct.pack("<4sIII", b"FPRM", 1, 3, 3) + struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 0, 1)
        + struct.pack("<3I", 0, 1, 2))
if "--offline" in argv:
    out = opt("--out")
    universe = opt("--universe")
    walls = ("wall {} -2 -1.5 2 -1.5 0 2.5\nwall {} 2 -1.5 2 1.5 0 2.5\n"
             "wall {} 2 1.5 -2 1.5 0 2.5\nwall {} -2 1.5 -2 -1.5 0 2.5\n")
    raw = ("frameport-room 1\nuniverse " + universe + "\nsource scan\ntime 100\nroom " + uuid.uuid4().hex +
           "\nfloor " + uuid.uuid4().hex + " 0\nceiling " + uuid.uuid4().hex + " 2.5\n" +
           walls.format(*(uuid.uuid4().hex for _ in range(4))) +
           "volume " + uuid.uuid4().hex + " COUCH 0 1 0.75 0 1.5 0.6 0.75\n"
           "mesh " + uuid.uuid4().hex + " room-mesh.bin 3 3\n")
    open(os.path.join(out, "room-raw.txt"), "w").write(raw.replace("room-mesh.bin", "room-mesh-raw.bin"))
    open(os.path.join(out, "room-mesh-raw.bin"), "wb").write(mesh)
    src = raw
    folder = out
else:
    folder = opt("--refine-only")
    if not os.path.exists(os.path.join(folder, "room-raw.txt")):
        text = open(os.path.join(folder, "room.txt")).read()
        open(os.path.join(folder, "room-raw.txt"), "w").write(text.replace("room-mesh.bin", "room-mesh-raw.bin"))
        shutil.copy(os.path.join(folder, "room-mesh.bin"), os.path.join(folder, "room-mesh-raw.bin"))
    src = open(os.path.join(folder, "room-raw.txt")).read().replace("room-mesh-raw.bin", "room-mesh.bin")
# the label rules: the couch is really a table (--label-rules 0 keeps the scanner's label)
rules = opt("--label-rules", "1") == "1"
refined = src.replace(" COUCH ", " TABLE ") if rules else src
open(os.path.join(folder, "room-mesh.bin"), "wb").write(mesh)
open(os.path.join(folder, "room.txt"), "w").write(refined)
settings = {"walls": opt("--walls", "panels"), "smooth": int(opt("--smooth", "8"))}
json.dump({"version": 1, "heads": [[0, 1.6, 0], [0.5, 1.6, 0]], "refine": settings,
           "report": {"triangles": 1, "kept": 1, "walked": 0},
           "labels": [{"from": "COUCH", "to": "TABLE" if rules else "COUCH", "reason": "top 0.75 m: a table",
                       "box": {"cx": 0, "cz": 1, "top_y": 0.75, "yaw_deg": 0, "w": 1.5, "d": 0.6, "h": 0.75}}],
           "suggestions": [{"kind": "door", "box": {"cx": 2, "cz": 0, "top_y": 2, "yaw_deg": 90, "w": 0.9, "d": 0.1,
                                                    "h": 2}, "wall": 1, "along": 0.5}]},
          open(os.path.join(folder, "scan.json"), "w"))
print(json.dumps({"triangles": 1, "refine": {"kept": 1}}))
'''


def scanner(a, monkeypatch):
    path = Path(a.ROOM_SCANNER)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(FAKE_SCANNER.replace("PYTHON", sys.executable))
    path.chmod(0o755)
    monkeypatch.setattr(a, "room_current_universe", lambda: 7)
    return path.parent / "scanner-calls.jsonl"


def calls(log):
    return [json.loads(line) for line in log.read_text().splitlines()]


def test_defaults_match_the_pc_and_settings_validate(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    assert a.ROOM_REFINE_DEFAULTS == {k: m["default"] for k, m in REFINE_DEFAULTS.items()}
    assert set(a.ROOM_REFINE_LIMITS) | {"walls"} == set(REFINE_DEFAULTS)
    for key, meta in REFINE_DEFAULTS.items():  # every slider value the PC offers is accepted
        if meta["kind"] == "number":
            lo, hi = a.ROOM_REFINE_LIMITS[key]
            assert lo <= meta["min"] and meta["max"] <= hi, key
    assert a.room_refine_settings({"band": 0.35, "walls": "snap", "smooth": 4.0, "protect": True}) == \
        {"walls": "snap", "smooth": 4, "protect": 1}
    for bad in ({"nope": 1}, {"walls": "maybe"}, {"band": float("nan")}, {"smooth": 2.5}, {"band": 9},
                {"island": "big"}):
        with pytest.raises(a.AgentError):
            a.room_refine_settings(bad)
    assert a.room_refine_args({"walls": "snap", "facing_deg": 45.0, "clip_open": 0.2}) == \
        ["--walls", "snap", "--facing-deg", "45", "--clip-open", "0.2"]
    assert a.room_refine_args({}, [[0, 1, 2.5, 0.25], {"x": 1, "y": 0, "z": 0, "r": 0.1}])[-2:] == \
        ["--erase", "0,1,2.5,0.25;1,0,0,0.1"]
    for bad in ([[0, 1, 2]], [[0, 1, 2, 0]], [["a", 1, 2, 3]]):
        with pytest.raises(a.AgentError, match="erase"):
            a.room_refine_args({}, bad)
    assert a.room_refine_load() == {}  # no file / a broken one = the defaults
    Path(a.ROOMS_DIR).mkdir(parents=True)
    Path(a.ROOM_REFINE_FILE).write_text("{broken")
    assert a.room_refine_load() == {}


def test_build_passes_settings_and_keeps_raw_files(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    log = scanner(a, monkeypatch)
    Path(a.ROOMS_DIR).mkdir(parents=True)
    dump = Path(a.ROOMS_DIR, "scan.fprs")
    dump.write_bytes(b"FPRS")
    status = a.cmd_room_build({"dump": str(dump)})  # nothing saved: no clean-up options (an old scanner works)
    argv = calls(log)[-1]
    assert argv[:3] == [a.ROOM_SCANNER, "--offline", str(dump)] and argv[argv.index("--universe") + 1] == "7"
    assert "--walls" not in argv and argv[-1] == "--json"
    folder = Path(a.ROOMS_DIR, "7")
    assert {p.name for p in folder.iterdir()} >= {"room.txt", "room-mesh.bin", "room-raw.txt", "room-mesh-raw.bin",
                                                   "scan.json"}
    assert status["report"] == {"triangles": 1, "refine": {"kept": 1}}
    assert status["labels"][0]["to"] == "TABLE" and status["suggestions"][0]["kind"] == "door"
    assert status["refine"] == {} and status["raw"] and not status["edited"]
    assert status["room"]["summary"]["volumes"] == {"TABLE": 1}
    a._write_json(a.ROOM_REFINE_FILE, {"walls": "snap", "smooth": 3})
    a.cmd_room_build({"dump": str(dump)})
    argv = calls(log)[-1]
    assert argv[argv.index("--walls") + 1] == "snap" and argv[argv.index("--smooth") + 1] == "3"


def test_refine_saves_settings_republishes_and_keeps_edits(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    log = scanner(a, monkeypatch)
    with pytest.raises(a.AgentError, match="No room"):
        a.cmd_room_refine({})
    # an old room (no raw files): the scanner takes room.txt as the raw one
    r = box_room(4, 3)
    r.universe = 7
    r.source = "scan"
    r.volumes = [Volume("a" * 32, "COUCH", 0, 1, 0.75, 0, 1.5, 0.6, 0.75)]
    m = Mesh(vertices=[(0, 0, 0), (1, 0, 0), (0, 0, 1)], indices=[0, 1, 2])
    r.mesh = Mesh(vertex_count=3, index_count=3)
    a.room_store(dumps(r), write_mesh(m), "old scan log")
    status = a.cmd_room_refine({"settings": {"walls": "snap", "band": 0.35, "label_rules": 0}})
    assert json.loads(Path(a.ROOM_REFINE_FILE).read_text()) == {"walls": "snap", "label_rules": 0}
    argv = calls(log)[-1]
    assert argv[:2] == [a.ROOM_SCANNER, "--refine-only"] and argv[-1] == "--json"
    assert argv[argv.index("--walls") + 1] == "snap" and "--band" not in argv
    assert status["summary"] == {"triangles": 1, "refine": {"kept": 1}}
    assert status["refine"] == {"walls": "snap", "label_rules": 0} and status["raw"]
    assert status["room"]["summary"]["volumes"] == {"COUCH": 1}  # label rules off
    folder = Path(a.ROOMS_DIR, "7")
    assert (folder / "scan.log").read_text() == "old scan log" and (folder / "room-raw.txt").is_file()
    assert not [n for n in os.listdir(a.ROOMS_DIR) if n.startswith(".refine-")]  # no stage left behind
    # null settings keep the saved ones; erase spots go to the scanner
    a.cmd_room_refine({"settings": None, "erase": [[0, 1, 0, 0.3]]})
    argv = calls(log)[-1]
    assert argv[argv.index("--walls") + 1] == "snap" and argv[argv.index("--erase") + 1] == "0,1,0,0.3"
    # the Room tab's box editor: an edited room keeps the raw files and its edits survive a new clean-up
    edited = parse(status["room_text"])
    edited.volumes = [Volume("b" * 32, "BED", 1, 0, 0.5, 0, 2, 1.4, 0.5)]
    incoming = Path(a.ROOMS_DIR, ".incoming-x")
    incoming.mkdir()
    (incoming / "room.txt").write_text(dumps(edited))
    (incoming / "room-mesh.bin").write_bytes(write_mesh(m))
    a.cmd_room_put({"path": str(incoming / "room.txt"), "edited": True})
    assert (folder / "edits.txt").read_text().startswith("volume " + "b" * 32 + " BED")
    assert (folder / "room-raw.txt").is_file() and (folder / "scan.json").is_file()
    status = a.cmd_room_refine({"settings": {}})
    assert status["edited"] and status["room"]["summary"]["volumes"] == {"BED": 1}
    assert status["refine"] == {} and json.loads(Path(a.ROOM_REFINE_FILE).read_text()) == {}
    status = a.cmd_room_refine({"keep_edits": False})  # back to the scan's boxes
    assert not status["edited"] and status["room"]["summary"]["volumes"] == {"TABLE": 1}
    # a plain import (another room) doesn't keep this one's raw files
    a.cmd_room_put({"path": str(incoming / "room.txt")})
    assert not (folder / "room-raw.txt").exists() and not a.cmd_room_status({})["raw"]


def test_refine_failures_leave_the_room(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    scanner(a, monkeypatch)
    r = box_room(4, 3)
    r.universe = 7
    text = dumps(r)
    a.room_store(text)
    with pytest.raises(a.AgentError, match="unknown"):
        a.cmd_room_refine({"settings": {"bogus": 1}})
    assert not Path(a.ROOM_REFINE_FILE).exists()
    monkeypatch.setenv("FAKE_SCANNER_FAIL", "1")
    with pytest.raises(a.AgentError, match="clean-up failed: boom"):
        a.cmd_room_refine({})
    assert Path(a.ROOMS_DIR, "7", "room.txt").read_text() == text
    monkeypatch.delenv("FAKE_SCANNER_FAIL")
    import fcntl
    with open(os.path.join(a.ROOMS_DIR, ".scan.lock"), "a") as lock:  # a scan is building a room
        fcntl.flock(lock, fcntl.LOCK_EX)
        with pytest.raises(a.AgentError, match="scan is running"):
            a.cmd_room_refine({})
    with pytest.raises(a.AgentError, match="unexpected"):
        a.room_store(text, extras={"../evil": b""})
    Path(a.ROOM_SCANNER).unlink()
    with pytest.raises(a.AgentError, match="not installed"):
        a.cmd_room_refine({})


def test_status_reads_scan_json_defensively(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    monkeypatch.setattr(a, "room_current_universe", lambda: 7)
    r = box_room(4, 3)
    r.universe = 7
    info = b'{"report": [1], "labels": {"x": 1}, "suggestions": [{"kind": "door"}]}'
    a.room_store(dumps(r), extras={"scan.json": info})
    st = a.cmd_room_status({})
    assert st["report"] is None and st["labels"] == [] and st["suggestions"] == [{"kind": "door"}]
    a.room_store(dumps(r), extras={"scan.json": b"[]"})
    assert a.cmd_room_status({})["labels"] == []
