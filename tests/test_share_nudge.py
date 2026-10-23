"""When the game page invites the user to share a recipe (GitHub #310/#329)."""
from frameport.ui.views.game import should_ask_to_share


def test_share_invitation():
    own = {"recipe": {"status": "unknown", "source": "suggested"}}
    assert should_ask_to_share(own, installed=True)
    assert not should_ask_to_share({**own, "share_dismissed": True}, installed=True)
    issues = {"recipe": {"status": "issues", "source": "catalog (remote)"}}
    assert not should_ask_to_share(issues, installed=True)  # installed only: not known how it runs
    assert should_ask_to_share({**issues, "last_played": 1}, installed=True)
    assert should_ask_to_share({**issues, "last_test": {"verdict": "pass"}}, installed=False)
    works = {"recipe": {"status": "works", "source": "catalog (bundled)"}, "last_played": 1}
    assert not should_ask_to_share(works, installed=True)
    assert not should_ask_to_share({**issues, "last_played": 1, "kind": "rift"}, installed=True)
