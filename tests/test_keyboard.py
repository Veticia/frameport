"""Type on Frame: key names -> Linux key codes, and the line protocol of a keyboard session."""
import io
import json
from types import SimpleNamespace

import pytest

from frameport.frame import keyboard as K


@pytest.mark.parametrize("name,code", [
    ("A", 30), ("a", 30), ("Key A", 30), ("1", 2), ("Digit 1", 2), ("!", 2), ("Enter", 28), ("Backspace", 14),
    ("Arrow Left", 105), ("ArrowLeft", 105), ("Shift Left", 42), ("Control Left", 29), ("Alt Right", 100),
    ("Meta Left", 125), (" ", 57), ("Space", 57), ("F5", 63), ("F12", 88), ("Numpad 7", 71), ("Page Down", 109),
    ("Escape", 1), ("/", 53), ("?", 53), ("`", 41), ("\\", 43), ("Tab", 15), ("Delete", 111),
])
def test_linux_key(name, code):
    assert K.linux_key(name) == code


def test_unknown_key_is_skipped():
    assert K.linux_key("Launch Mail Application") is None and K.linux_key("") is None


class _Chan:
    def __init__(self):
        self.shut = False

    def settimeout(self, t):
        pass

    def shutdown_write(self):
        self.shut = True


class _In(io.StringIO):
    def __init__(self):
        super().__init__()
        self.channel = _Chan()

    def close(self):  # keep the value readable for the test
        pass


class _Out(io.StringIO):
    def __init__(self, text):
        super().__init__(text)
        self.channel = _Chan()


def _frame(replies):
    stdin, stdout = _In(), _Out(replies)
    client = SimpleNamespace(exec_command=lambda cmd, timeout=None: (stdin, stdout, None))
    return SimpleNamespace(client=client, home="/home/steamos", ensure_agent=lambda: None), stdin


def test_session_streams_keys_and_text_then_closes():
    frame, stdin = _frame('{"ready": true}\n{"typed": true, "skipped": "☃"}\n')
    s = K.KeyboardSession(frame)
    assert s.key("Shift Left") and s.key("A") and s.key("A", "up") and s.key("Arrow Down", "repeat")
    assert not s.key("Launch Mail Application")
    assert s.text("Hi ☃") == "☃"
    s.close()
    sent = [json.loads(x) for x in stdin.getvalue().splitlines()]
    assert sent == [{"k": 42, "v": 1}, {"k": 30, "v": 1}, {"k": 30, "v": 0}, {"k": 108, "v": 2},
                    {"text": "Hi ☃"}]
    assert stdin.channel.shut and s.closed
    s.key("A")  # after close: ignored, no error
    assert len(stdin.getvalue().splitlines()) == 5


def test_session_refused():
    frame, _ = _frame('{"ready": false, "error": "can\'t create a virtual keyboard: denied"}\n')
    with pytest.raises(K.KeyboardRefused, match="denied"):
        K.KeyboardSession(frame)


def test_paste_buttons_move_below_a_narrow_field():
    """The paste field keeps a usable width: its buttons go below it rather than squeezing it (or clipping)."""
    from frameport.ui import theme as T
    from frameport.ui.views.keyboard import PASTE_MIN_WIDTH, paste_stacked

    buttons = [110.0, 170.0]
    need = sum(buttons) + 2 * T.S2 + T.px(PASTE_MIN_WIDTH)
    assert paste_stacked(need + 1, buttons) is False
    assert paste_stacked(need - 1, buttons) is True
    assert paste_stacked(0, buttons) is None  # not measured yet: the layout stays
    assert paste_stacked(900, [110.0, None]) is None
