"""The Room tab's browser view server (mr/room_viewer.py): token-only routes on 127.0.0.1, the mesh served unchanged."""
import json
import urllib.error
import urllib.request

import pytest

from frameport.mr.room import Mesh, Volume, box_room, dumps, write_mesh
from frameport.mr.room_viewer import RoomViewerServer, room_json
from frameport.mr.viewer_page import VIEWER_HTML


def get(url):
    with urllib.request.urlopen(url, timeout=5) as r:
        return r.status, r.headers, r.read()


@pytest.fixture
def server():
    srv = RoomViewerServer().start()
    yield srv
    srv.stop()


def test_routes_need_the_token(server):
    assert server.url.startswith("http://127.0.0.1:")
    status, headers, body = get(server.url)
    assert status == 200 and b"FPRM" in body and "default-src 'self'" in headers["Content-Security-Policy"]
    base = server.url.rsplit("/", 2)[0]
    for path in ("/", "/room.json", "/mesh.bin", "/x/room.json", f"/{server.token}x/room.json"):
        with pytest.raises(urllib.error.HTTPError) as exc:
            get(base + path)
        assert exc.value.code == 404
    with pytest.raises(urllib.error.HTTPError):  # no room yet
        get(server.url + "room.json")


def test_room_and_mesh_served_unchanged(server):
    room = box_room(4, 3)
    room.volumes.append(Volume("1" * 32, "TABLE", 0.5, -0.5, 0.75, 90.0, 1.0, 0.5, 0.75))
    mesh = write_mesh(Mesh(vertices=[(0, 0, 0), (1, 0, 0), (0, 0, 1)], indices=[0, 2, 1]))
    server.set_room(dumps(room), mesh)
    _, headers, body = get(server.url + "mesh.bin")
    assert body == mesh and headers["Content-Type"] == "application/octet-stream"
    data = json.loads(get(server.url + "room.json")[2])
    assert data["has_mesh"] and data["summary"]["walls"] == 4 and data["version"] == 1
    corners = data["volumes"][0]["corners"]
    assert corners[0] == pytest.approx([0.25, -0.0])  # yaw 90: local -X/-Z corner (see view3d.box_corners)
    v1 = get(server.url + "version")[2]
    server.set_room(dumps(room), mesh)  # same room: same version (the page doesn't reload)
    assert get(server.url + "version")[2] == v1
    server.set_room(dumps(room), None)
    assert get(server.url + "version")[2] == b"2"
    with pytest.raises(urllib.error.HTTPError):
        get(server.url + "mesh.bin")


def test_rejects_unreadable_room_and_stops(server):
    with pytest.raises(ValueError):
        server.set_room("not a room")
    url = server.url
    server.stop()
    server.stop()  # twice is fine
    with pytest.raises(OSError):
        get(url)


def test_page_is_self_contained():
    assert "http" not in VIEWER_HTML.replace("http-equiv", "")  # nothing from the network
    assert "<script src" not in VIEWER_HTML and "getContext(\"webgl\"" in VIEWER_HTML
    assert room_json(box_room(2, 2), False)["has_mesh"] is False
