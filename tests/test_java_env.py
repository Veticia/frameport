"""GitHub #487: FramePort on the Frame (C/POSIX locale) started Java without UTF-8, so `LEGO® Bricktales/x.apk`
reached OVRPort as `LEGO?? Bricktales/x.apk`."""
import sys

import pytest

from frameport.tools import toolchain


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="Linux only")
def test_java_gets_a_utf8_locale_on_linux():
    assert toolchain.java_env({"PATH": "/bin"})["LC_ALL"] == "C.UTF-8"
    assert toolchain.java_env({"LC_ALL": "C", "LANG": "en_US.UTF-8"})["LC_ALL"] == "C.UTF-8"
    assert toolchain.java_env({"LANG": "en_US.UTF-8"}) is None  # already UTF-8: inherited unchanged
    assert toolchain.java_env({"LC_CTYPE": "de_DE.utf8"}) is None


def test_other_systems_inherit_the_environment(monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    assert toolchain.java_env({"LC_ALL": "C"}) is None
