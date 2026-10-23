"""device.shared_folder: a folder of /sdcard shared with another game (GitHub #440)."""
import pytest

from frameport.patches import base
from frameport.patches.base import InstallContext
from frameport.recommend.catalog import CatalogEntry


def _ctx(params):
    ctx = InstallContext.__new__(InstallContext)
    ctx.params, ctx.env = params, {}
    return ctx


def test_env_and_validation():
    p = base.get("device.shared_folder")
    ctx = _ctx({"game": "com.drbeef.jkxr", "folder": "JKXR"})
    p.install(ctx)
    assert ctx.env == {"FRAMEPORT_SHARE": "com.drbeef.jkxr:JKXR"}
    for bad in ({"game": "x", "folder": "JKXR"}, {"game": "com.a.b", "folder": "../etc"},
                {"game": "com.a.b", "folder": "a/b"}):
        with pytest.raises(ValueError):
            p.install(_ctx(bad))
    empty = _ctx({})
    p.install(empty)
    assert empty.env == {}


def test_catalog_field():
    e = CatalogEntry(package="com.BaggyG.JKXR_Companion_App", title="JKXR Companion",
                     shared_folder={"game": "com.drbeef.jkxr", "folder": "JKXR"})
    assert e.to_dict()["shared_folder"] == {"game": "com.drbeef.jkxr", "folder": "JKXR"}
