"""Expansion files with a changed name go to the Frame under the name the game looks for (GitHub #450/#481)."""
import contextlib
from types import SimpleNamespace

from frameport import pipeline
from frameport.core.events import Reporter
from frameport.core.models import Recipe
from frameport.game_update import plan_update
from frameport.install import installer
from frameport.sources import quest_dump

VR4 = "com.Armature.VR4"


def names(pkg, rels):
    return {o.name: (o.wanted, o.rename, o.reason) for o in quest_dump.obb_names(pkg, rels)}


def test_copy_suffixes_are_renamed():
    """#450: a download's -003/-001 suffixes; also Windows' " (1)" and " - Copy"."""
    assert names(VR4, ["main.203.com.Armature.VR4-003.obb", "patch.203.com.Armature.VR4-001.obb"]) == {
        "main.203.com.Armature.VR4-003.obb": ("main.203.com.Armature.VR4.obb", True, "copy"),
        "patch.203.com.Armature.VR4-001.obb": ("patch.203.com.Armature.VR4.obb", True, "copy")}
    assert names("com.x.y", ["main.5.com.x.y (1).obb"])["main.5.com.x.y (1).obb"][:2] == ("main.5.com.x.y.obb", True)
    assert names("com.x.y", ["main.5.com.x.y - Copy.obb"])["main.5.com.x.y - Copy.obb"][:2] == (
        "main.5.com.x.y.obb", True)


def test_case_differences_are_renamed_and_the_version_is_kept():
    assert names(VR4, ["Main.203.com.armature.vr4.obb"]) == {
        "Main.203.com.armature.vr4.obb": ("main.203.com.Armature.VR4.obb", True, "case")}
    # an older versionCode is normal (expansion files keep theirs): never changed
    assert names(VR4, ["main.200.com.Armature.VR4-1.obb"])["main.200.com.Armature.VR4-1.obb"][0] == \
        "main.200.com.Armature.VR4.obb"


def test_the_games_own_names_are_left_alone():
    """Exact names, Unreal chunk files (#471), split parts (Phantom), other data and subfolders."""
    assert names("com.Sanzaru.Wrath2", ["main.5513.com.Sanzaru.Wrath2.obb", "patch.0.com.Sanzaru.Wrath2.obb",
                                        "patch.pakchunk27_s1optional-Android_ASTC.com.Sanzaru.Wrath2.obb"]) == {}
    assert names("com.nDreams.PhantomQuest", [f"main.com.nDreams.PhantomQuest.part{i}.obb" for i in range(6)]) == {}
    assert names("com.crytek.climb2", ["main.com.crytek.climb2.obb", "Audio_en-US.obb", "AAP.obb",
                                       "chunk.1.com.crytek.climb2.obb", "sub/main.1.com.crytek.CLIMB2.obb"]) == {}


def test_unclear_cases_stay_warnings():
    # #129: a renamed build's OBB is another app's file: never renamed
    assert names("com.beatgames.beatsaber", ["main.1716.com.beatgames.beatsaber.lj369vr.obb"]) == {
        "main.1716.com.beatgames.beatsaber.lj369vr.obb": ("main.1716.com.beatgames.beatsaber.obb", False,
                                                          "another-app")}
    # the right name is there too: the copy is left as it is
    assert names(VR4, ["main.203.com.Armature.VR4.obb", "main.203.com.Armature.VR4-003.obb"]) == {
        "main.203.com.Armature.VR4-003.obb": ("main.203.com.Armature.VR4.obb", False, "duplicate")}
    # two candidates for one name
    got = names(VR4, ["main.203.com.Armature.VR4-1.obb", "main.203.com.Armature.VR4 (2).obb"])
    assert {v[1:] for v in got.values()} == {(False, "ambiguous")} and len(got) == 2
    # no versionCode: the number at the end may be the versionCode
    assert names(VR4, ["main.com.Armature.VR4-203.obb"])["main.com.Armature.VR4-203.obb"][1:] == (
        False, "unclear-version")


def test_game_page_lists_renamed_files_as_info_and_the_rest_as_warnings():
    entry = {"package": VR4, "data_manifest": {"main.203.com.Armature.VR4-003.obb": 1,
                                               "patch.203.com.Armature.VR4-001.obb": 1}}
    assert [(o.name, o.wanted) for o in pipeline.renamed_obbs(entry)] == [
        ("main.203.com.Armature.VR4-003.obb", "main.203.com.Armature.VR4.obb"),
        ("patch.203.com.Armature.VR4-001.obb", "patch.203.com.Armature.VR4.obb")]
    assert pipeline.misnamed_obbs(entry) == []
    chunks = {"package": "com.Sanzaru.Wrath2", "data_manifest": {
        "patch.pakchunk27-Android_ASTC.com.Sanzaru.Wrath2.obb": 1, "main.5513.com.Sanzaru.Wrath2.obb": 1}}
    assert pipeline.misnamed_obbs(chunks) == [] and pipeline.renamed_obbs(chunks) == []


def test_loose_copy_named_obbs_next_to_the_apk_are_found(tmp_path, monkeypatch):
    """A folder with the APK and a download's -003 OBBs: found by name, the right-named one wins a tie."""
    (tmp_path / "game.apk").write_bytes(b"")
    for n in ("main.203.com.Armature.VR4-003.obb", "patch.203.com.Armature.VR4-001.obb", "notes.txt"):
        (tmp_path / n).write_bytes(b"x")
    data, files = quest_dump.find_data(tmp_path, VR4, 203)
    assert data == tmp_path and files == ["main.203.com.Armature.VR4-003.obb", "patch.203.com.Armature.VR4-001.obb"]
    (tmp_path / "main.203.com.Armature.VR4.obb").write_bytes(b"x")
    assert quest_dump.find_data(tmp_path, VR4, 203)[1] == [
        "main.203.com.Armature.VR4.obb", "patch.203.com.Armature.VR4-001.obb"]


def test_plan_update_compares_the_frames_names():
    """No endless "Update on Frame": the deployment records the renamed file under its Frame name."""
    local = {"main.203.com.Armature.VR4-003.obb": 7}
    game = {"package": VR4, "data_manifest": local}
    assert not plan_update(game, {"data_manifest": {"main.203.com.Armature.VR4.obb": 7}}).data
    assert plan_update(game, {"data_manifest": {"main.203.com.Armature.VR4-003.obb": 7}}).data  # installed before
    import hashlib
    import json
    fp = hashlib.sha256(json.dumps({"main.203.com.Armature.VR4.obb": 7}, sort_keys=True).encode()).hexdigest()[:32]
    assert not plan_update(game, {"data_fp": fp}).data  # list_installed without details: the fingerprint


class FakeFrame:
    def __init__(self, existing):
        self.existing, self.calls, self.renamed = existing, {}, []
        self.target = SimpleNamespace(host="10.0.0.5")
        self.sftp = SimpleNamespace(rename=lambda a, b: self.renamed.append((a, b)))

    def agent(self, command, **args):
        self.calls[command] = args
        if command == "prepare":
            return {"base": "/g", "incoming": "/g/incoming", "existing_obb": self.existing, "same_apk": True,
                    "free_bytes": 1 << 40, "lepton": "/l", "installed": True}
        return {"base": "/g", "appid": 1}


def _install(tmp_path, monkeypatch, existing):
    data = tmp_path / "Resident Evil 4" / VR4
    data.mkdir(parents=True)
    (data / "main.203.com.Armature.VR4-003.obb").write_bytes(b"m" * 5)
    (data / "patch.203.com.Armature.VR4-001.obb").write_bytes(b"p" * 3)
    apk = tmp_path / "game.apk"
    apk.write_bytes(b"apk")
    sent = []
    monkeypatch.setattr(installer, "upload_files", lambda xfer, items, *a, **k: sent.extend(items))
    monkeypatch.setattr(installer, "transfer_link", lambda frame, reporter, need: contextlib.nullcontext(frame))
    monkeypatch.setattr(installer, "_tags", lambda pkg: [])
    frame = FakeFrame(existing)
    installer.install(frame, installer.InstallPlan(VR4, "VR4", apk, data, Recipe(VR4)), Reporter())
    return frame, sent, data


def test_install_sends_renamed_files_under_the_games_names(tmp_path, monkeypatch):
    frame, sent, data = _install(tmp_path, monkeypatch, {})
    assert sorted((p.name, rel) for p, rel, _ in sent) == [
        ("main.203.com.Armature.VR4-003.obb", "obb/main.203.com.Armature.VR4.obb"),
        ("patch.203.com.Armature.VR4-001.obb", "obb/patch.203.com.Armature.VR4.obb")]
    assert frame.calls["finalize"]["obb_manifest"] == {"main.203.com.Armature.VR4.obb": 5,
                                                       "patch.203.com.Armature.VR4.obb": 3}
    assert sorted(p.name for p in data.iterdir()) == ["main.203.com.Armature.VR4-003.obb",
                                                       "patch.203.com.Armature.VR4-001.obb"]  # PC files untouched


def test_install_renames_an_earlier_copy_on_the_frame_instead_of_sending_it(tmp_path, monkeypatch):
    """Installed before this fix under the old name: moved into place on the Frame, nothing uploaded twice."""
    frame, sent, _ = _install(tmp_path, monkeypatch, {"main.203.com.Armature.VR4-003.obb": 5,
                                                      "patch.203.com.Armature.VR4.obb": 3})
    assert sent == [] and frame.renamed == [("/g/lepton-app/obb/main.203.com.Armature.VR4-003.obb",
                                             "/g/incoming/obb/main.203.com.Armature.VR4.obb")]


def test_relink_on_the_frame_uses_the_pcs_name(tmp_path, monkeypatch):
    """#433: an earlier copy of a renamed file becomes a link to the original under the Frame's name."""
    import os

    src = tmp_path / "src"
    src.mkdir()
    (src / "main.1.com.x-003.obb").write_bytes(b"x" * 4)
    root = tmp_path / "obb"
    root.mkdir()
    (root / "main.1.com.x.obb").write_bytes(b"x" * 4)
    monkeypatch.setattr("frameport.frame.local.on_frame", lambda: True)
    frame = SimpleNamespace(target=SimpleNamespace(host="127.0.0.1"))
    assert installer._relink_installed(frame, src, {"main.1.com.x.obb": 4}, str(root), Reporter(),
                                       {"main.1.com.x.obb": "main.1.com.x-003.obb"}) == 4
    assert os.path.samefile(root / "main.1.com.x.obb", src / "main.1.com.x-003.obb")
