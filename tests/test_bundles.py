"""App bundles (.xapk/.apks/.apkm/.apkx, split APK folders): sources/bundles.py and where games are added."""
import json
import zipfile
from pathlib import Path

import pytest
from conftest import build_axml

from frameport.apk import axml
from frameport.sources import bundles, quest_dump

PKG = "com.example.bundled"


def base_manifest(pkg=PKG, vc=7, split_required=True):
    app = [("isSplitRequired", "bool", True)] if split_required else []
    return build_axml([
        ("start", "manifest", [("package", "str", pkg), ("versionCode", "int", vc)]),
        ("start", "application", [("hasCode", "bool", True), *app]),
        ("start", "meta-data", [("name", "str", "com.android.vending.splits.required"), ("value", "bool", True)]),
        ("end", "meta-data"),
        ("start", "activity", [("name", "str", "com.example.Main")]),
        ("start", "intent-filter", []),
        ("start", "action", [("name", "str", "android.intent.action.MAIN")]),
        ("end", "action"),
        ("start", "category", [("name", "str", "android.intent.category.LAUNCHER")]),
        ("end", "category"),
        ("end", "intent-filter"),
        ("end", "activity"),
        ("end", "application"),
        ("end", "manifest"),
    ])


def split_manifest(split, pkg=PKG, vc=7):
    return build_axml([
        ("start", "manifest", [("package", "str", pkg), ("versionCode", "int", vc), ("split", "str", split)]),
        ("start", "application", [("hasCode", "bool", False)]),
        ("end", "application"),
        ("end", "manifest"),
    ])


def apk_bytes(manifest, files=(), stored_libs=True):
    import io

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("AndroidManifest.xml", manifest)
        z.writestr("META-INF/CERT.RSA", b"old signature")
        for name, data in files:
            ct = zipfile.ZIP_STORED if stored_libs and name.endswith(".so") else zipfile.ZIP_DEFLATED
            z.writestr(zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0)), data, compress_type=ct)
    return buf.getvalue()


def base_apk(**kw):
    return apk_bytes(base_manifest(**kw), [("classes.dex", b"dex\n035\0"), ("assets/data.bin", b"base asset"),
                                           ("resources.arsc", b"arsc")])


def abi_split(abi="arm64_v8a", **kw):
    real = bundles.ABIS[abi]
    return apk_bytes(split_manifest(f"config.{abi}", **kw), [(f"lib/{real}/libgame.so", b"\x7fELF" + real.encode())])


def make_zip(path: Path, members: dict) -> Path:
    with zipfile.ZipFile(path, "w") as z:
        for name, data in members.items():
            z.writestr(name, data, compress_type=zipfile.ZIP_STORED)
    return path


def xapk(tmp_path, name="Cool Game.xapk", obb=b"obb data", extra=None):
    manifest = {"xapk_version": 2, "package_name": PKG, "name": "Cool Game", "version_code": "7",
                "split_apks": [{"file": f"{PKG}.apk", "id": "base"},
                               {"file": "config.arm64_v8a.apk", "id": "config.arm64_v8a"},
                               {"file": "config.armeabi_v7a.apk", "id": "config.armeabi_v7a"},
                               {"file": "config.x86_64.apk", "id": "config.x86_64"}],
                "expansions": [{"file": f"Android/obb/{PKG}/main.7.{PKG}.obb", "install_location": "EXTERNAL_STORAGE",
                                "install_path": f"Android/obb/{PKG}/main.7.{PKG}.obb"}]}
    members = {"manifest.json": json.dumps(manifest), "icon.png": b"png", f"{PKG}.apk": base_apk(),
               "config.arm64_v8a.apk": abi_split(), "config.armeabi_v7a.apk": abi_split("armeabi_v7a"),
               "config.x86_64.apk": abi_split("x86_64"), f"Android/obb/{PKG}/main.7.{PKG}.obb": obb,
               **(extra or {})}
    return make_zip(tmp_path / name, members)


def test_manifest_helpers_remove_split_attributes():
    m = base_manifest()
    assert axml.manifest_attrs(m) == {"package": PKG, "versionCode": 7}
    assert axml.Axml(m).get_bool("application", "isSplitRequired") is True
    fixed = bundles.fix_manifest(m)
    assert not axml.has_attr(fixed, "application", "isSplitRequired")
    assert axml.has_attr(fixed, "application", "hasCode")  # the other attributes stay
    assert axml.meta_data(fixed)["com.android.vending.splits.required"] is False
    assert [e.name for e in axml.Axml(fixed).elements()] == [e.name for e in axml.Axml(m).elements()]
    assert axml.remove_attrs(fixed, "application", {"isSplitRequired"}) is None
    assert axml.manifest_attrs(split_manifest("config.arm64_v8a"))["split"] == "config.arm64_v8a"


def test_xapk_merges_the_arm64_split_and_unpacks_obbs(tmp_path):
    src = xapk(tmp_path)
    plan = bundles.inspect(src)
    assert plan.package == PKG and plan.version_code == 7 and plan.method == "libs" and plan.title == "Cool Game"
    assert [p.name for p in plan.splits] == ["config.arm64_v8a.apk"]
    assert {n for n, _ in plan.skipped} == {"config.armeabi_v7a.apk", "config.x86_64.apk"}
    game = quest_dump.from_path(src)
    assert game.bundle == src and game.origin == tmp_path and game.name == "Cool Game"
    assert bundles.owned(game.apk) and game.apk.name == f"{PKG}.apk"
    assert (game.data_dir / f"main.7.{PKG}.obb").read_bytes() == b"obb data"
    with zipfile.ZipFile(game.apk) as z:
        names = z.namelist()
        manifest = z.read("AndroidManifest.xml")
        assert z.getinfo("lib/arm64-v8a/libgame.so").compress_type == zipfile.ZIP_STORED
    assert "lib/arm64-v8a/libgame.so" in names and "assets/data.bin" in names and "classes.dex" in names
    assert not any(n.startswith(("lib/armeabi-v7a", "lib/x86")) for n in names)
    assert "META-INF/CERT.RSA" not in names and not bundles.is_signed(game.apk)
    assert not axml.has_attr(manifest, "application", "isSplitRequired")
    from frameport.analysis.detect import _split_base

    assert _split_base(base_manifest()) and not _split_base(manifest)
    # unchanged bundle: the unpacked copy is reused; a moved bundle finds the same copy
    stamp = game.apk.stat().st_mtime_ns
    moved = tmp_path / "sub"
    moved.mkdir()
    src.rename(moved / src.name)
    again = quest_dump.from_path(moved / src.name)
    assert again.apk == game.apk and again.apk.stat().st_mtime_ns == stamp and again.origin == moved


def test_single_apk_xapk_keeps_the_apk_unchanged(tmp_path):
    apk = apk_bytes(base_manifest(split_required=False), [("lib/arm64-v8a/libgame.so", b"\x7fELF")])
    src = make_zip(tmp_path / "game.xapk", {"manifest.json": json.dumps({"package_name": PKG}), f"{PKG}.apk": apk,
                                            f"Android/obb/{PKG}/main.7.{PKG}.obb": b"o"})
    game = quest_dump.from_path(src)
    assert game.apk.read_bytes() == apk  # the original signature stays valid
    assert bundles.inspect(src).method == "base"


def test_bundletool_apks_and_universal(tmp_path):
    src = make_zip(tmp_path / "game.apks", {"toc.pb": b"\x0a\x00", "splits/base-master.apk": base_apk(),
                                            "splits/base-arm64_v8a.apk": abi_split(),
                                            "splits/base-x86.apk": abi_split("x86"),
                                            "instant/instant-base-master.apk": base_apk()})
    plan = bundles.inspect(src)
    assert plan.base.name == "splits/base-master.apk" and [p.name for p in plan.splits] == ["splits/base-arm64_v8a.apk"]
    universal = apk_bytes(base_manifest(split_required=False), [("lib/arm64-v8a/libgame.so", b"x")])
    src2 = make_zip(tmp_path / "universal.apks", {"toc.pb": b"", "universal.apk": universal})
    assert bundles.inspect(src2).method == "base"
    # bundletool standalones (pre-Lollipop): the arm64 one is complete, the splits aren't needed
    src3 = make_zip(tmp_path / "standalones.apks", {
        "splits/base-master.apk": base_apk(), "splits/base-arm64_v8a.apk": abi_split(),
        "standalones/standalone-armeabi_v7a_xxhdpi.apk": base_apk(split_required=False),
        "standalones/standalone-arm64_v8a_xxhdpi.apk": base_apk(split_required=False)})
    p3 = bundles.inspect(src3)
    assert p3.base.name == "standalones/standalone-arm64_v8a_xxhdpi.apk" and not p3.splits


def test_32bit_only_bundle_takes_the_armv7_split(tmp_path):
    src = make_zip(tmp_path / "old.apkm", {"info.json": json.dumps({"app_name": "Old"}), "base.apk": base_apk(),
                                           "split_config.armeabi_v7a.apk": abi_split("armeabi_v7a"),
                                           "split_config.x86.apk": abi_split("x86")})
    plan = bundles.inspect(src)
    assert [p.name for p in plan.splits] == ["split_config.armeabi_v7a.apk"] and plan.title == "Old"


def test_resource_splits_go_through_apkeditor(tmp_path, monkeypatch):
    from frameport.tools import apkeditor

    lang = apk_bytes(split_manifest("config.de"), [("resources.arsc", b"de")])
    src = make_zip(tmp_path / "game.apkm", {"base.apk": base_apk(), "split_config.arm64_v8a.apk": abi_split(),
                                            "split_config.de.apk": lang})
    assert bundles.inspect(src).method == "apkeditor"
    seen = []

    def fake_merge(apks, out, work, timeout=0):
        seen.append(len(apks))
        bundles.merge_simple(apks[0], apks[1:], out)  # (the manifest still gets fixed afterwards)
        return out
    monkeypatch.setattr(apkeditor, "merge", fake_merge)
    game = quest_dump.from_path(src)
    assert seen == [3]
    with zipfile.ZipFile(game.apk) as z:
        assert not axml.has_attr(z.read("AndroidManifest.xml"), "application", "isSplitRequired")
    assert json.loads((game.apk.parent / bundles.INFO).read_text())["method"] == "apkeditor"


def test_apkeditor_missing_falls_back_for_language_splits(tmp_path, monkeypatch):
    from frameport.tools import apkeditor

    def broken(*a, **k):
        raise RuntimeError("Java is not installed")
    monkeypatch.setattr(apkeditor, "merge", broken)
    lang = apk_bytes(split_manifest("config.de"), [("resources.arsc", b"de")])
    src = make_zip(tmp_path / "game.apkm", {"base.apk": base_apk(), "split_config.arm64_v8a.apk": abi_split(),
                                            "split_config.de.apk": lang})
    game = quest_dump.from_path(src)
    with zipfile.ZipFile(game.apk) as z:
        assert "lib/arm64-v8a/libgame.so" in z.namelist() and z.read("resources.arsc") == b"arsc"
    assert json.loads((game.apk.parent / bundles.INFO).read_text())["method"] == "libs-only"
    # a feature module with code can't be left out
    feature = apk_bytes(split_manifest("feature_vr"), [("classes.dex", b"dex")])
    src2 = make_zip(tmp_path / "feature.apks", {"base.apk": base_apk(), "split_feature_vr.apk": feature})
    with pytest.raises(bundles.BundleError, match="couldn't be merged"):
        quest_dump.from_path(src2)


def test_encrypted_and_broken_bundles_say_why(tmp_path):
    enc = tmp_path / "old.apkm"
    enc.write_bytes(b"\x00\x02" + bytes(80))
    with pytest.raises(bundles.BundleError, match="protected APKMirror"):
        bundles.inspect(enc)
    junk = tmp_path / "x.xapk"
    junk.write_bytes(b"hello world")
    with pytest.raises(bundles.BundleError, match="isn't an app bundle"):
        bundles.inspect(junk)
    empty = make_zip(tmp_path / "e.apkx", {"readme.txt": b"hi"})
    with pytest.raises(bundles.BundleError, match="no APK"):
        bundles.inspect(empty)
    only_splits = make_zip(tmp_path / "s.xapk", {"config.arm64_v8a.apk": abi_split()})
    with pytest.raises(bundles.BundleError, match="no base APK"):
        bundles.inspect(only_splits)
    evil = make_zip(tmp_path / "evil.xapk", {
        "manifest.json": json.dumps({"expansions": [{"file": "x.obb", "install_path": f"Android/obb/{PKG}/../../x"}]}),
        "base.apk": base_apk(), "x.obb": b"x"})
    with pytest.raises(bundles.BundleError, match="outside"):
        quest_dump.from_path(evil)


def test_split_folder_and_scan(tmp_path):
    lib = tmp_path / "library"
    folder = lib / "Pulled Game"
    folder.mkdir(parents=True)
    (folder / "base.apk").write_bytes(base_apk())
    (folder / "split_config.arm64_v8a.apk").write_bytes(abi_split())
    obb = folder / PKG
    obb.mkdir()
    (obb / f"main.7.{PKG}.obb").write_bytes(b"o")
    assert bundles.split_folder(folder)
    xapk(lib, "Other.xapk")  # (same package: a second copy of the game)
    bad = lib / "broken.apkm"
    bad.write_bytes(b"\x00\x01" + bytes(80))
    errors = []
    games = quest_dump.scan(lib, on_error=lambda p, e: errors.append(p.name))
    assert errors == ["broken.apkm"]
    by_bundle = {g.bundle.name: g for g in games}
    assert set(by_bundle) == {"Pulled Game", "Other.xapk"}
    pulled = by_bundle["Pulled Game"]
    assert pulled.data_dir == obb and pulled.origin == folder and bundles.owned(pulled.apk)
    # a folder with an APK and its alternate build isn't a split set
    plain = tmp_path / "plain"
    plain.mkdir()
    (plain / "a.apk").write_bytes(base_apk(split_required=False))
    (plain / "a.alt-noforcequit.apk").write_bytes(base_apk(split_required=False))
    assert bundles.split_folder(plain) is None


def test_add_path_library_entry_and_cleanup(tmp_path):
    from frameport import pipeline
    from frameport.core import library

    src = xapk(tmp_path)
    [entry] = pipeline.add_path(src)
    assert entry["package"] == PKG and entry["bundle"] == str(src) and bundles.owned(entry["apk"])
    files = pipeline.local_game_files(PKG)
    assert src in files and Path(entry["apk"]).parent in files
    # the unpacked copy was removed (another PC, cleaned data folder): unpacked again when needed
    import shutil

    shutil.rmtree(Path(entry["apk"]).parent)
    assert pipeline.check_source(library.game(PKG)).apk.is_file()
    library.remove_game(PKG)
    assert not Path(entry["apk"]).exists() and src.exists()


def test_missing_split_triage_and_note():
    from frameport.validate.triage import triage

    r = triage("Failure [INSTALL_FAILED_MISSING_SPLIT: Missing split for com.example.bundled]", "EXITED")
    assert "missing-split" in {f.id for f in r.findings}


def test_routes(tmp_path):
    from frameport import deeplink
    from frameport.ui import dropped

    f = xapk(tmp_path)
    assert dropped.route([str(f)]) == [("apk", str(f))]
    assert deeplink.classify("game.APKM") == deeplink.APK and deeplink.classify("x.apks") == deeplink.APK
    m = deeplink.Manifest("Game", [deeplink.ManifestFile("https://example.com/game.xapk")])
    assert m.main.filename == "game.xapk"
    assert set(deeplink.BUNDLE_EXTS) == set(bundles.EXTS)
