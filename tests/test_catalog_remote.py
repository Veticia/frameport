"""Confirmed game configs from GitHub main (recommend/catalog.refresh_remote): fetched without a release, skipped when
they need a newer FramePort."""
import json
from types import SimpleNamespace

import pytest

from frameport.recommend import catalog

GOOD = ("package: com.x.good\ntitle: Good\nstatus: works\nframe:\n- frame.unity_text_input\n"
        "verified:\n  date: '2099-01-01'\n")
FUTURE_PATCH = "package: com.x.future\ntitle: Future\nstatus: works\nframe:\n- frame.not_yet_released\n"
FUTURE_FIELD = "package: com.x.field\ntitle: Field\nstatus: works\nlinux_runtime: x\n"
FUTURE_APP = "package: com.x.app\ntitle: App\nstatus: works\nmin_app: 99.0.0\n"


@pytest.fixture
def github(monkeypatch):
    files = {"catalog/games/com.x.good.yaml": ("s1", GOOD), "catalog/games/com.x.future.yaml": ("s2", FUTURE_PATCH),
             "catalog/games/com.x.field.yaml": ("s3", FUTURE_FIELD), "catalog/games/com.x.app.yaml": ("s4", FUTURE_APP),
             "catalog/triage.yaml": ("s5", "x: 1\n")}
    calls = []

    def http_get(url, timeout=20, **kw):
        calls.append(url)
        if "git/trees" in url:
            return SimpleNamespace(json=lambda: {"tree": [{"path": p, "type": "blob", "sha": s}
                                                          for p, (s, _t) in files.items()]})
        path = url.split("/main/", 1)[1]
        return SimpleNamespace(text=files[path][1])
    monkeypatch.setattr(catalog.cache, "http_get", http_get)
    monkeypatch.delenv("FRAMEPORT_CATALOG_URL", raising=False)
    monkeypatch.delenv("FRAMEPORT_NO_CATALOG_UPDATE", raising=False)
    return files, calls


def test_remote_configs_are_used_and_newer_app_configs_skipped(github):
    files, calls = github
    assert catalog.refresh_remote(force=True) == 0  # the first download: nothing "updated" (no toast)
    entries = catalog.load(refresh=True)
    assert entries["com.x.good"].origin == "remote"
    st = catalog.remote_status()
    assert set(st["skipped"]) == {"com.x.future", "com.x.field", "com.x.app"}
    assert "frame.not_yet_released" in st["skipped"]["com.x.future"]
    assert "com.x.future" not in entries and "com.x.app" not in entries
    # a second check downloads nothing new (only the file list), and only after 6 h unless forced
    calls.clear()
    assert catalog.refresh_remote() == 0 and calls == []
    assert catalog.refresh_remote(force=True) == 0 and len(calls) == 1


def test_changed_and_removed_files(github):
    files, _ = github
    catalog.refresh_remote(force=True)
    files["catalog/games/com.x.good.yaml"] = ("s9", GOOD.replace("status: works", "status: issues"))
    del files["catalog/games/com.x.app.yaml"]
    assert catalog.refresh_remote(force=True) == 1
    assert catalog.load(refresh=True)["com.x.good"].status == "issues"
    assert not list(catalog.cache.cache_dir().glob("catalog-gh-s4.yaml"))  # removed from main: removed here


def test_offline_keeps_the_cache(github, monkeypatch):
    catalog.refresh_remote(force=True)
    monkeypatch.setattr(catalog.cache, "http_get", lambda *a, **k: (_ for _ in ()).throw(OSError("offline")))
    assert catalog.refresh_remote(force=True) == 0
    assert catalog.load(refresh=True)["com.x.good"].origin == "remote"


def test_turned_off(github, monkeypatch):
    monkeypatch.setenv("FRAMEPORT_NO_CATALOG_UPDATE", "1")
    assert catalog.refresh_remote(force=True) == 0
    assert "com.x.good" not in catalog.load(refresh=True)
    assert json.dumps(catalog.remote_status())  # still reports (never checked)


def test_library_load_with_a_cold_catalog_does_not_recurse(monkeypatch):
    """GitHub #58: library.load() refreshes recipes from the catalog; the catalog's first load read a setting through
    library.load(), which refreshed recipes again before the catalog was cached (~140 nested loads, blank window or a
    crash at start-up)."""
    from frameport.core import library

    monkeypatch.delenv("FRAMEPORT_NO_CATALOG_UPDATE", raising=False)  # remote_enabled() must read the setting
    monkeypatch.setattr(library, "REFRESH_ON_UPDATE", True)
    library.save({"games": {"com.x.mine": {"recipe": {"source": "user", "patches": {}}, "analysis": {"package": "x"}}},
                  "settings": {"catalog.auto_update": False}})
    catalog._cache = None
    loads = []
    real = library.load

    def counting():
        loads.append(1)
        assert len(loads) < 10, "library.load() recursed"
        return real()
    monkeypatch.setattr(library, "load", counting)
    library.load()
    assert catalog.remote_enabled() is False and len(loads) <= 2


def test_swapchain_pool_catalog_is_gated_for_older_apps(monkeypatch):
    from pathlib import Path

    import yaml

    from frameport.patches import base

    path = Path(__file__).resolve().parents[1] / "catalog/games/cn.vr4p.oculus4xvrplayerov.yaml"
    entry = yaml.safe_load(path.read_text())
    assert entry["adapter"]["swapchain_pool"] == 1
    assert catalog.unusable_reason(entry) is None
    # Older installed apps see this catalog from main, but cannot register the new adapter key.
    monkeypatch.delitem(base.REGISTRY, "adapter.swapchain_pool")
    assert "adapter.swapchain_pool" in catalog.unusable_reason(entry)


def test_a_newer_entry_this_app_cant_use_keeps_the_last_usable_one(github):
    """An entry naming a patch, setting or field this app doesn't have must not change its games' recipes: the last
    remote entry it could use stays (not the bundled one, which can be older and broken, e.g. AC Nexus on Quest 3)."""
    files, _ = github
    catalog.refresh_remote(force=True)
    assert catalog.load(refresh=True)["com.x.good"].status == "works"
    newer = GOOD.replace("- frame.unity_text_input", "- frame.unity_text_input\n- frame.not_yet_released") \
        .replace("status: works", "status: issues")
    files["catalog/games/com.x.good.yaml"] = ("s7", newer)
    catalog.refresh_remote(force=True)
    entries = catalog.load(refresh=True)
    assert entries["com.x.good"].status == "works" and entries["com.x.good"].origin == "remote"
    assert "frame.not_yet_released" in catalog.remote_status()["skipped"]["com.x.good"]
    # usable again (the app was updated, or the entry fixed): that one is used and becomes the last usable one
    files["catalog/games/com.x.good.yaml"] = ("s8", GOOD.replace("status: works", "status: issues"))
    catalog.refresh_remote(force=True)
    assert catalog.load(refresh=True)["com.x.good"].status == "issues"
    # never usable here: nothing to keep, as before
    assert "com.x.future" not in catalog.load(refresh=True)


def test_fields_that_stand_for_a_patch_need_it(monkeypatch):
    from frameport.patches import base

    entry = {"package": "com.x.y", "title": "Y", "status": "works", "shared_folder": {"game": "com.a.b", "folder": "F"}}
    assert catalog.unusable_reason(entry) is None
    monkeypatch.setitem(catalog.FIELD_PATCHES, "shared_folder", "device.not_in_this_app")
    base.load_all()
    assert "device.not_in_this_app" in catalog.unusable_reason(entry)
