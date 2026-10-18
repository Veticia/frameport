import pytest

from frameport.validate.triage import triage


@pytest.mark.parametrize("line", ["scene: capture requested -> guardian box",
    "scene: capture requested -> using guardian room", "scene: room from guardian",
    "scene: room 1.50 x 1.50 x 2.50 m (from guardian)",
    "scene: room 1.50 x 1.50 x 2.50 m (configured/default size)"])
def test_fallback_room_is_an_informational_hint(line):
    result = triage("FrameBridge: " + line, "RUNNING", "com.example.mr")
    finding = next(f for f in result.findings if f.id == "mr-no-room")
    assert finding.severity == "info" and "Scan your room" in finding.diagnosis
    assert finding.suggest == []
    assert result.verdict == "pass"


@pytest.mark.parametrize("line", ["overportOXR: Unknown proc addr: xrDiscoverSpacesMETA",
                                   "OVRPlugin: Unknown proc addr: xrDiscoverSpacesMETA", "MRUK: NoRoomsFound"])
def test_discovery_suggests_scene_emulation(line):
    result = triage(line, "RUNNING", "com.example.mr")
    finding = next(f for f in result.findings if f.id == "mr-discovery-unsupported")
    assert finding.suggest == ["adapter.scene_emul"]


def test_served_room_raises_no_scene_warnings():
    # OVRPort lists XR_FB_scene and names xrDiscoverSpacesMETA before the shim takes over; FrameBridge served the room
    log = ("overportOXR: - XR_FB_scene 1\n"
           "overportOXR: Unknown proc addr: xrDiscoverSpacesMETA\n"
           "FrameBridge: extension shim: xrQuerySpacesFB -> FrameBridge\n"
           "FrameBridge: scene: emulation active (anchored to STAGE)\n"
           "FrameBridge: scene: discovery request=398895087617 results=1\n")
    ids = {f.id for f in triage(log, "RUNNING", "com.example.mr").findings}
    assert "scene-missing" not in ids and "mr-discovery-unsupported" not in ids


def test_loaded_scan_is_not_a_missing_room():
    result = triage("FrameBridge: scene: loaded room.txt (source scan)", "RUNNING")
    assert not any(f.id.startswith("mr-") for f in result.findings)
