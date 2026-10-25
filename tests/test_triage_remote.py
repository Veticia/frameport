"""Triage signatures from GitHub main (catalog/triage.yaml fetched by recommend/catalog.refresh_remote): used per
signature where this app can, the bundled ones otherwise."""
from types import SimpleNamespace

import pytest
import yaml

from frameport.core.paths import catalog_dir
from frameport.recommend import catalog
from frameport.validate import triage

BUNDLED = yaml.safe_load((catalog_dir() / "triage.yaml").read_text(encoding="utf-8"))
FIRST = BUNDLED["signatures"][0]["id"]
SECOND = BUNDLED["signatures"][1]["id"]


def sig(sid, **kw):
    return {"id": sid, "pattern": "REMOTE_MARKER_" + sid, "severity": "error", "diagnosis": "remote " + sid,
            "suggest": [], **kw}


def remote_text(sigs, **top):
    return yaml.safe_dump({**top, "signatures": sigs, "milestones": BUNDLED["milestones"]}, sort_keys=False)


@pytest.fixture
def github(monkeypatch):
    files = {"catalog/games/com.x.good.yaml": ("g1", "package: com.x.good\ntitle: Good\nstatus: works\n"),
             "catalog/triage.yaml": ("t1", remote_text([sig(FIRST), sig("new-remote-sig")]))}
    calls = []

    def http_get(url, timeout=20, **kw):
        calls.append(url)
        if "git/trees" in url:
            return SimpleNamespace(json=lambda: {"tree": [{"path": p, "type": "blob", "sha": s}
                                                          for p, (s, _t) in files.items()]})
        return SimpleNamespace(text=files[url.split("/main/", 1)[1]][1])
    monkeypatch.setattr(catalog.cache, "http_get", http_get)
    monkeypatch.delenv("FRAMEPORT_CATALOG_URL", raising=False)
    monkeypatch.delenv("FRAMEPORT_NO_CATALOG_UPDATE", raising=False)
    triage.reset()
    yield files, calls
    triage.reset()


def ids():
    return [s["id"] for s in triage.database()["signatures"]]


def by_id(sid):
    return next(s for s in triage.database()["signatures"] if s["id"] == sid)


def test_remote_copy_is_used_and_bundled_only_signatures_stay(github):
    files, calls = github
    assert by_id(FIRST)["diagnosis"] != "remote " + FIRST
    catalog.refresh_remote(force=True)
    assert any(u.endswith("/main/catalog/triage.yaml") for u in calls)  # no extra API call: the same tree listing
    assert sum("git/trees" in u for u in calls) == 1
    assert by_id(FIRST)["diagnosis"] == "remote " + FIRST  # the refresh reset the cached database
    assert "new-remote-sig" in ids() and SECOND in ids()  # bundled-only ids are kept
    assert len(ids()) == len(BUNDLED["signatures"]) + 1
    res = triage.triage("REMOTE_MARKER_new-remote-sig\n")
    assert [f.id for f in res.findings] == ["new-remote-sig"]
    # a changed file on main replaces the cached one (and the old copy goes)
    files["catalog/triage.yaml"] = ("t2", remote_text([sig(FIRST, diagnosis="changed")]))
    catalog.refresh_remote(force=True)
    assert by_id(FIRST)["diagnosis"] == "changed" and "new-remote-sig" not in ids()
    assert [f.name for f in catalog.cache.cache_dir().glob("triage-gh-*.yaml")] == ["triage-gh-t2.yaml"]


def test_bad_yaml_falls_back_to_bundled(github):
    files, _ = github
    files["catalog/triage.yaml"] = ("t3", "signatures: [ {id: x, pattern: 'unclosed\n")
    catalog.refresh_remote(force=True)
    assert triage.database()["signatures"] == BUNDLED["signatures"]
    assert triage.skipped() == {"*": "not valid YAML"}


def test_unknown_patch_drops_only_that_signature(github):
    files, _ = github
    files["catalog/triage.yaml"] = ("t4", remote_text([
        sig(FIRST, suggest=["frame.not_yet_released"]), sig("other", suggest=["adapter.focus_hold=2500"]),
        sig("bad-adapter", suggest=["adapter.not_a_setting"]), sig("future-field", pattern2="x"),
        sig("bad-severity", severity="catastrophic"), sig("bad-regex", pattern="(")]))
    catalog.refresh_remote(force=True)
    sk = triage.skipped()
    assert "frame.not_yet_released" in sk[FIRST] and "adapter.not_a_setting" in sk["bad-adapter"]
    assert set(sk) == {FIRST, "bad-adapter", "future-field", "bad-severity", "bad-regex"}
    assert by_id(FIRST) == BUNDLED["signatures"][0]  # the bundled version of a dropped remote signature stays
    assert by_id("other")["diagnosis"] == "remote other"
    assert not {"bad-adapter", "future-field", "bad-severity", "bad-regex"} & set(ids())


def test_min_app_too_new_is_dropped(github):
    files, _ = github
    files["catalog/triage.yaml"] = ("t5", remote_text([sig(FIRST, min_app="99.0.0"), sig("ok", min_app="0.1.0")]))
    catalog.refresh_remote(force=True)
    assert "99.0.0" in triage.skipped()[FIRST] and by_id(FIRST) == BUNDLED["signatures"][0]
    assert "ok" in ids()
    # top-level min_app: the whole remote copy waits for the app update
    files["catalog/triage.yaml"] = ("t6", remote_text([sig("ok")], min_app="99.0.0"))
    catalog.refresh_remote(force=True)
    assert "ok" not in ids() and "99.0.0" in triage.skipped()["*"]


def test_a_later_dated_bundled_signature_wins():
    bundled = {"signatures": [sig("a", updated="2026-10-20", diagnosis="bundled a"), sig("b", diagnosis="bundled b")]}
    merged = triage.merge_remote(bundled, remote_text([sig("a", updated="2026-10-01"), sig("b")]))
    assert [s["diagnosis"] for s in merged["signatures"]] == ["bundled a", "remote b"]


def test_turned_off(github, monkeypatch):
    catalog.refresh_remote(force=True)
    triage.reset()
    monkeypatch.setenv("FRAMEPORT_NO_CATALOG_UPDATE", "1")
    assert triage.database()["signatures"] == BUNDLED["signatures"]
    triage.reset()
    monkeypatch.delenv("FRAMEPORT_NO_CATALOG_UPDATE")
    from frameport.core import library

    library.save({"games": {}, "settings": {"catalog.auto_update": False}})
    assert triage.database()["signatures"] == BUNDLED["signatures"]


def test_catalog_url_override(github, monkeypatch):
    files, calls = github
    catalog.refresh_remote(force=True)  # a GitHub copy in the cache must not be used with the override
    triage.reset()
    monkeypatch.setenv("FRAMEPORT_CATALOG_URL", "https://example.invalid/cat")
    monkeypatch.setattr(catalog.cache, "http_get", lambda url, **k: SimpleNamespace(
        text=remote_text([sig("from-override")])) if url == "https://example.invalid/cat/triage.yaml"
        else (_ for _ in ()).throw(OSError("404")))
    assert "from-override" in ids() and "new-remote-sig" not in ids()
