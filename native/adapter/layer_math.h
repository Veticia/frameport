// SPDX-License-Identifier: GPL-3.0-only
// Pose / direction math shared by the adapter's per-game fixes (session_fixes.c, layer_emul_gl.c). Header-only and
// free of Android/GL dependencies so tests/test_layer_emul.py can compile it for the host.
#ifndef FRAMEBRIDGE_LAYER_MATH_H
#define FRAMEBRIDGE_LAYER_MATH_H
#include <math.h>
#include <openxr/openxr.h>

#define LM_PI 3.14159265358979323846f

static inline XrVector3f lm_rotate(XrQuaternionf q, XrVector3f v) {  // v' = v + 2w(u x v) + 2 u x (u x v)
    XrVector3f u = {q.x, q.y, q.z}, t = {2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return (XrVector3f){v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                        v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

static inline XrQuaternionf lm_qmul(XrQuaternionf a, XrQuaternionf b) {
    return (XrQuaternionf){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                           a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static inline XrQuaternionf lm_qconj(XrQuaternionf q) { return (XrQuaternionf){-q.x, -q.y, -q.z, q.w}; }

static inline XrQuaternionf lm_axis_angle(float x, float y, float z, float radians) {
    float s = sinf(radians / 2);
    return (XrQuaternionf){x * s, y * s, z * s, cosf(radians / 2)};
}

// a∘b: the pose b (expressed in a's frame) expressed in a's parent frame.
static inline XrPosef lm_pose_mul(XrPosef a, XrPosef b) {
    XrVector3f p = lm_rotate(a.orientation, b.position);
    return (XrPosef){lm_qmul(a.orientation, b.orientation),
                     {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z}};
}

static inline XrPosef lm_pose_inv(XrPosef a) {
    XrQuaternionf q = lm_qconj(a.orientation);
    XrVector3f p = lm_rotate(q, a.position);
    return (XrPosef){q, {-p.x, -p.y, -p.z}};
}

// Rotation angle (radians) of a pose: how far it is from identity.
static inline float lm_angle(XrQuaternionf q) {
    float w = fabsf(q.w) > 1 ? 1 : fabsf(q.w);
    return 2 * acosf(w);
}

static inline float lm_length(XrVector3f v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

static inline float lm_dot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// ---- cube faces used to show 360° (equirect) layers as world-locked quads.
// Face f looks along lm_face_rotation(f) * (0,0,-1): 0 = -Z (front), 1 = +X (right), 2 = +Z (back), 3 = -X (left),
// 4 = +Y (up), 5 = -Y (down). The same rotation orients the face's quad (a quad faces +Z, so it faces the centre).
static inline XrQuaternionf lm_face_rotation(int face) {
    switch (face) {
    case 1: return lm_axis_angle(0, 1, 0, -LM_PI / 2);
    case 2: return lm_axis_angle(0, 1, 0, LM_PI);
    case 3: return lm_axis_angle(0, 1, 0, LM_PI / 2);
    case 4: return lm_axis_angle(1, 0, 0, LM_PI / 2);
    case 5: return lm_axis_angle(1, 0, 0, -LM_PI / 2);
    default: return (XrQuaternionf){0, 0, 0, 1};
    }
}

// Direction (layer space) of the point (u, v) in [-1, 1]² of face f (u = right, v = up on the face's quad).
static inline XrVector3f lm_face_dir(int face, float u, float v) {
    return lm_rotate(lm_face_rotation(face), (XrVector3f){u, v, -1});
}

// Pose of face f's quad: `distance` metres from the layer's origin, facing it.
static inline XrPosef lm_face_pose(XrPosef layer, int face, float distance) {
    XrPosef local = {lm_face_rotation(face), lm_face_dir(face, 0, 0)};
    local.position.x *= distance; local.position.y *= distance; local.position.z *= distance;
    return lm_pose_mul(layer, local);
}

// GL cube map convention (the equirect_emul shader's cube_dir): direction of texel (s, t) = ((u + 1) / 2, (v + 1) / 2)
// on face GL_TEXTURE_CUBE_MAP_POSITIVE_X + face.
static inline XrVector3f lm_glcube_dir(int face, float u, float v) {
    switch (face) {
    case 0: return (XrVector3f){1, -v, -u};
    case 1: return (XrVector3f){-1, -v, u};
    case 2: return (XrVector3f){u, 1, v};
    case 3: return (XrVector3f){u, -1, -v};
    case 4: return (XrVector3f){u, -v, 1};
    default: return (XrVector3f){-u, -v, -1};
    }
}

// Cube image array order: +X,-X,+Y,-Y,+Z,-Z. Local +Z faces inward.
// Quad right/up match GL cube s/t; Vulkan copies reverse rows to compensate its top-left image origin.
static inline XrPosef lm_cube_quad_pose(XrQuaternionf orientation, int face, float distance) {
    static const int faces[6] = {1, 3, 4, 5, 2, 0};
    XrPosef p = lm_face_pose((XrPosef){orientation, {0, 0, 0}}, faces[face], distance);
    if (face != 2 && face != 3)
        p.orientation = lm_qmul(p.orientation, (XrQuaternionf){0, 0, 1, 0});
    return p;
}

// GL's cube map lookup (OpenGL ES 3.0 spec, table 3.21): direction -> face and (s, t).
static inline int lm_glcube_lookup(XrVector3f d, float *s, float *t) {
    float ax = fabsf(d.x), ay = fabsf(d.y), az = fabsf(d.z), sc, tc, ma;
    int face;
    if (ax >= ay && ax >= az) { face = d.x > 0 ? 0 : 1; ma = ax; sc = d.x > 0 ? -d.z : d.z; tc = -d.y; }
    else if (ay >= az) { face = d.y > 0 ? 2 : 3; ma = ay; sc = d.x; tc = d.y > 0 ? d.z : -d.z; }
    else { face = d.z > 0 ? 4 : 5; ma = az; sc = d.z > 0 ? d.x : -d.x; tc = -d.y; }
    *s = (sc / ma + 1) / 2;
    *t = (tc / ma + 1) / 2;
    return face;
}

// Equirect2 mapping (XR_KHR_composition_layer_equirect2): direction (layer space) -> normalized image coordinates
// (u right, v up; v = 0 at lowerVerticalAngle). Longitude 0 is the -Z axis, growing towards +X.
// Returns 0 when the direction is outside the layer's angles.
static inline int lm_equirect2_uv(XrVector3f d, float central, float upper, float lower, float *u, float *v) {
    float len = lm_length(d);
    if (len <= 0 || central <= 0 || upper <= lower) return 0;
    float lon = atan2f(d.x, -d.z), lat = asinf(fmaxf(-1.0f, fminf(1.0f, d.y / len)));
    *u = lon / central + 0.5f;
    *v = (lat - lower) / (upper - lower);
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

// Is any part of face f within `cos_limit` (cosine of the angle) of `forward` (unit, layer space)? Samples a grid on
// the face, so it is conservative up to the grid spacing (the caller adds a margin to cos_limit for that).
static inline int lm_face_visible(int face, XrVector3f forward, float cos_limit) {
    for (int i = 0; i <= 4; ++i)
        for (int j = 0; j <= 4; ++j) {
            XrVector3f d = lm_face_dir(face, -1 + 0.5f * i, -1 + 0.5f * j);
            if (lm_dot(d, forward) / lm_length(d) >= cos_limit) return 1;
        }
    return 0;
}

// Cosine of the angle between the face's centre direction and `forward` (unit): sort key, larger = closer to view.
static inline float lm_face_score(int face, XrVector3f forward) { return lm_dot(lm_face_dir(face, 0, 0), forward); }

#endif
