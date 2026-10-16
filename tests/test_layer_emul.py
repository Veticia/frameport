"""Math behind FrameBridge's per-game fixes (native/adapter/layer_math.h): cube faces for 360° layers
(equirect_emul) and the pose algebra used by stable_local / aim correction. Compiled for this host with a small C
harness. Opt-in (FRAMEPORT_NATIVE_TESTS=1, -m native): needs the OpenXR headers in native/.cache (native/build.py)."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
INC = next(iter((ROOT / "native/.cache").glob("openxr-*")), None)
CC = shutil.which("cc") or shutil.which("clang")
pytestmark = [pytest.mark.native, pytest.mark.skipif(
    not os.environ.get("FRAMEPORT_NATIVE_TESTS") or not INC or not CC,
    reason="set FRAMEPORT_NATIVE_TESTS=1 (needs the OpenXR headers from native/build.py and a host C compiler)")]

HARNESS = r"""
#include <stdio.h>
#include "layer_math.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #c); return 1; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)
#define VNEAR(v, X, Y, Z) (NEAR((v).x, X) && NEAR((v).y, Y) && NEAR((v).z, Z))
int main(void) {
    // face centre directions: front -Z, right +X, back +Z, left -X, up +Y, down -Y
    CHECK(VNEAR(lm_face_dir(0, 0, 0), 0, 0, -1));
    CHECK(VNEAR(lm_face_dir(1, 0, 0), 1, 0, 0));
    CHECK(VNEAR(lm_face_dir(2, 0, 0), 0, 0, 1));
    CHECK(VNEAR(lm_face_dir(3, 0, 0), -1, 0, 0));
    CHECK(VNEAR(lm_face_dir(4, 0, 0), 0, 1, 0));
    CHECK(VNEAR(lm_face_dir(5, 0, 0), 0, -1, 0));
    // faces tile seamlessly: the right edge of the front face is the left edge of the right face, etc.
    CHECK(VNEAR(lm_face_dir(0, 1, 0), 1, 0, -1) && VNEAR(lm_face_dir(1, -1, 0), 1, 0, -1));
    CHECK(VNEAR(lm_face_dir(1, 1, 0), 1, 0, 1) && VNEAR(lm_face_dir(2, -1, 0), 1, 0, 1));
    CHECK(VNEAR(lm_face_dir(0, 0, 1), 0, 1, -1) && VNEAR(lm_face_dir(4, 0, -1), 0, 1, -1));
    CHECK(VNEAR(lm_face_dir(0, 0, -1), 0, -1, -1) && VNEAR(lm_face_dir(5, 0, 1), 0, -1, -1));
    // every face's quad sits on its axis and faces the centre (a quad's normal is its local +Z)
    XrPosef origin = {{0, 0, 0, 1}, {0, 1.5f, 0}};
    for (int f = 0; f < 6; ++f) {
        XrPosef p = lm_face_pose(origin, f, 10);
        XrVector3f axis = lm_face_dir(f, 0, 0);
        CHECK(VNEAR(p.position, 10 * axis.x, 1.5f + 10 * axis.y, 10 * axis.z));
        XrVector3f normal = lm_rotate(p.orientation, (XrVector3f){0, 0, 1});
        CHECK(VNEAR(normal, -axis.x, -axis.y, -axis.z));
        // the quad's right (+X) is the face's +u: its right edge is where lm_face_dir(f, 1, 0) points
        XrVector3f right = lm_rotate(p.orientation, (XrVector3f){1, 0, 0});
        XrVector3f edge = lm_face_dir(f, 1, 0);
        CHECK(VNEAR(edge, axis.x + right.x, axis.y + right.y, axis.z + right.z));
    }
    // the shader's cube_dir writes every texel where GL's cube lookup will read it back
    for (int f = 0; f < 6; ++f)
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j) {
                float u = -0.9f + 0.45f * i, v = -0.9f + 0.45f * j, s, t;
                CHECK(lm_glcube_lookup(lm_glcube_dir(f, u, v), &s, &t) == f);
                CHECK(NEAR(s, (u + 1) / 2) && NEAR(t, (v + 1) / 2));
            }
    // equirect2: full sphere, image centre (u 0.5) straight ahead (-Z), +X to the right, v 1 at the top
    float u, v;
    CHECK(lm_equirect2_uv((XrVector3f){0, 0, -1}, 2 * LM_PI, LM_PI / 2, -LM_PI / 2, &u, &v)
          && NEAR(u, 0.5f) && NEAR(v, 0.5f));
    CHECK(lm_equirect2_uv((XrVector3f){1, 0, 0}, 2 * LM_PI, LM_PI / 2, -LM_PI / 2, &u, &v)
          && NEAR(u, 0.75f) && NEAR(v, 0.5f));
    CHECK(lm_equirect2_uv((XrVector3f){-1, 0, 0}, 2 * LM_PI, LM_PI / 2, -LM_PI / 2, &u, &v) && NEAR(u, 0.25f));
    CHECK(lm_equirect2_uv((XrVector3f){0, 1, 0}, 2 * LM_PI, LM_PI / 2, -LM_PI / 2, &u, &v) && NEAR(v, 1.0f));
    CHECK(lm_equirect2_uv((XrVector3f){0, -1, -0.0001f}, 2 * LM_PI, LM_PI / 2, -LM_PI / 2, &u, &v) && v < 0.001f);
    // a 180° layer covers the front hemisphere only
    CHECK(lm_equirect2_uv((XrVector3f){0.9f, 0, -0.1f}, LM_PI, LM_PI / 2, -LM_PI / 2, &u, &v) && u > 0.9f);
    CHECK(!lm_equirect2_uv((XrVector3f){0, 0, 1}, LM_PI, LM_PI / 2, -LM_PI / 2, &u, &v));
    // face choice: looking ahead with a ~60° half-diagonal FOV + margin, the back face is never needed
    float cos_limit = cosf(1.05f + 0.26f);
    XrVector3f ahead = {0, 0, -1};
    CHECK(lm_face_visible(0, ahead, cos_limit) && !lm_face_visible(2, ahead, cos_limit));
    CHECK(lm_face_visible(1, ahead, cos_limit) && lm_face_visible(4, ahead, cos_limit));
    CHECK(lm_face_score(0, ahead) > lm_face_score(1, ahead));
    // pose algebra (stable_local): (A∘B)∘B⁻¹ = A
    XrPosef a = {lm_axis_angle(0, 1, 0, 0.7f), {0.3f, -0.2f, 1.1f}},
            b = {lm_axis_angle(1, 0, 0, -0.4f), {-0.5f, 0.1f, 0.2f}};
    XrPosef back = lm_pose_mul(lm_pose_mul(a, b), lm_pose_inv(b));
    CHECK(VNEAR(back.position, a.position.x, a.position.y, a.position.z));
    CHECK(NEAR(fabsf(back.orientation.w), fabsf(a.orientation.w))
          && NEAR(lm_angle(lm_qmul(back.orientation, lm_qconj(a.orientation))), 0));
    CHECK(NEAR(lm_angle(lm_axis_angle(0, 1, 0, 0.5f)), 0.5f));
    // quad into a projection image (projection_only=2): a 2x2 m quad 1 m ahead of an eye with a symmetric 90° FOV
    // fills the view exactly (NDC = clip / w = ±1); bottom-left first, top-right last
    float clip[4][4];
    XrFovf fov90 = {-LM_PI / 4, LM_PI / 4, LM_PI / 4, -LM_PI / 4};
    XrPosef ident = {{0, 0, 0, 1}, {0, 0, 0}}, ahead1 = {{0, 0, 0, 1}, {0, 0, -1}};
    lm_quad_clip(ahead1, (XrExtent2Df){2, 2}, ident, fov90, clip);
    CHECK(NEAR(clip[0][3], 1) && NEAR(clip[0][0] / clip[0][3], -1) && NEAR(clip[0][1] / clip[0][3], -1));
    CHECK(NEAR(clip[3][0] / clip[3][3], 1) && NEAR(clip[3][1] / clip[3][3], 1) && NEAR(clip[1][0], 1));
    CHECK(NEAR(clip[2][1], 1) && NEAR(clip[2][0], -1) && clip[0][2] == 0);
    // the eye moved 1 m to the right: the quad's centre is left of the view's centre, its right edge at the centre
    XrPosef right_eye = {{0, 0, 0, 1}, {1, 0, 0}};
    lm_quad_clip(ahead1, (XrExtent2Df){2, 2}, right_eye, fov90, clip);
    CHECK(NEAR(clip[1][0] / clip[1][3], 0) && NEAR(clip[0][0] / clip[0][3], -2));
    // an asymmetric FOV (more to the left) moves the view's centre: the quad's centre is right of the image centre
    XrFovf asym = {-0.9f, 0.5f, 0.6f, -0.6f};
    lm_quad_clip(ahead1, (XrExtent2Df){0.01f, 0.01f}, ident, asym, clip);
    CHECK(clip[0][0] / clip[0][3] > 0.1f && NEAR(clip[0][1] / clip[0][3] + clip[3][1] / clip[3][3], 0));
    // behind the eye: w < 0 (GL clips it away)
    XrPosef behind = {{0, 0, 0, 1}, {0, 0, 1}};
    lm_quad_clip(behind, (XrExtent2Df){1, 1}, ident, fov90, clip);
    CHECK(clip[0][3] < 0 && clip[3][3] < 0);
    // a quad turned 90° about Y is seen edge-on: left and right corners at the same x
    XrPosef turned = {lm_axis_angle(0, 1, 0, LM_PI / 2), {0, 0, -2}};
    lm_quad_clip(turned, (XrExtent2Df){1, 1}, ident, fov90, clip);
    CHECK(fabsf(clip[0][0] / clip[0][3]) < 0.3f && NEAR(clip[0][3] + clip[1][3], 4));
    puts("ok");
    return 0;
}
"""


def test_layer_math(tmp_path):
    src = tmp_path / "harness.c"
    src.write_text(HARNESS)
    exe = tmp_path / "harness"
    subprocess.run([CC, "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(INC), "-I", str(ROOT / "native/adapter"),
                    str(src), "-lm", "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == "ok"
