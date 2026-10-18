// Host runtime/GL doubles exercise the actual adapter implementation, without a headset.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "line %d: %s\n", __LINE__, #c);                                                            \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
#include <stdarg.h>
static void log_ignore(const char *fmt, ...) { (void)fmt; }
#define LOG(...) log_ignore(__VA_ARGS__)
static uintptr_t fake = 100;
#define FAKE_HANDLE(t) ((t)(++fake))
static XrInstance active_instance;
static int swapchain_pool = 1;
static int creates, destroys, acquires, waits, releases, blits, deleted, fail_create, fail_enum, fail_wait,
    fail_release;
static int bad_gl, fail_gl, scissor = 1, read_fb = 31, draw_fb = 32, texture = 33;
static GLuint next_texture = 1000;
typedef struct {
    XrStructureType type;
    void *next;
    uint32_t image;
} emul_gles_image;
static XrResult create(XrSession s, const XrSwapchainCreateInfo *ci, XrSwapchain *h) {
    (void)s;
    (void)ci;
    ++creates;
    if (fail_create) return XR_ERROR_LIMIT_REACHED;
    *h = FAKE_HANDLE(XrSwapchain);
    return XR_SUCCESS;
}
static XrResult destroy(XrSwapchain h) {
    (void)h;
    ++destroys;
    return XR_SUCCESS;
}
static XrResult enumerate(XrSwapchain h, uint32_t cap, uint32_t *n, XrSwapchainImageBaseHeader *images) {
    (void)h;
    (void)cap;
    if (fail_enum) return XR_ERROR_RUNTIME_FAILURE;
    *n = 3;
    for (int j = 0; j < 3; ++j) ((emul_gles_image *)images)[j].image = 2000 + j;
    return XR_SUCCESS;
}
static XrResult acquire(XrSwapchain h, const XrSwapchainImageAcquireInfo *ai, uint32_t *index) {
    (void)h;
    (void)ai;
    *index = acquires++ % 3;
    return XR_SUCCESS;
}
static XrResult wait_image(XrSwapchain h, const XrSwapchainImageWaitInfo *wi) {
    (void)h;
    (void)wi;
    ++waits;
    return fail_wait ? XR_TIMEOUT_EXPIRED : XR_SUCCESS;
}
static XrResult release(XrSwapchain h, const XrSwapchainImageReleaseInfo *ri) {
    (void)h;
    (void)ri;
    ++releases;
    return fail_release ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}
static PFN_xrVoidFunction lookup(XrInstance instance, const char *name) {
    (void)instance;
#define PROC(n, f)                                                                                                     \
    if (!strcmp(name, n)) return (PFN_xrVoidFunction)f
    PROC("xrCreateSwapchain", create);
    PROC("xrDestroySwapchain", destroy);
    PROC("xrEnumerateSwapchainImages", enumerate);
    PROC("xrAcquireSwapchainImage", acquire);
    PROC("xrWaitSwapchainImage", wait_image);
    PROC("xrReleaseSwapchainImage", release);
    return NULL;
}
static void note_swapchain_size(XrSwapchain h, uint32_t w, uint32_t he) {
    (void)h;
    (void)w;
    (void)he;
}
static void remember_swapchain(XrSwapchain h) { (void)h; }
static void forget_swapchain(XrSwapchain h) { (void)h; }
static void emul_on_create_swapchain(XrSwapchain h, const XrSwapchainCreateInfo *ci) {
    (void)h;
    (void)ci;
}
static void emul_on_destroy_swapchain(XrSwapchain h) { (void)h; }
#include "pool_clamp.h"

static EGLContext current(void) { return (EGLContext)1; }
static EGLContext (*p_eglGetCurrentContext)(void) = current;
static void (*p_eglCreateContext)(void);
static int emul_load_gl(void) { return 1; }
static void p_glGetIntegerv(GLenum p, GLint *v) {
    *v = p == GL_TEXTURE_BINDING_2D ? texture : p == GL_READ_FRAMEBUFFER_BINDING ? read_fb : draw_fb;
}
static void p_glGenTextures(GLsizei n, GLuint *v) {
    while (n--) *v++ = next_texture++;
}
static void p_glBindTexture(GLenum target, GLuint t) {
    (void)target;
    texture = t;
}
static void p_glTexStorage2D(GLenum t, GLsizei levels, GLenum f, GLsizei w, GLsizei h) {
    (void)t;
    (void)levels;
    (void)f;
    (void)w;
    (void)h;
}
static GLenum p_glGetError(void) { return fail_gl ? GL_INVALID_OPERATION : GL_NO_ERROR; }
static void p_glDeleteTextures(GLsizei n, const GLuint *v) {
    (void)v;
    deleted += n;
}
static GLsync p_glFenceSync(GLenum c, GLbitfield f) {
    (void)c;
    (void)f;
    return (GLsync)1;
}
static void p_glDeleteSync(GLsync s) { (void)s; }
static void p_glWaitSync(GLsync s, GLbitfield f, GLuint64 t) {
    (void)s;
    (void)f;
    (void)t;
}
static void p_glFlush(void) {}
static GLboolean p_glIsEnabled(GLenum c) {
    (void)c;
    return scissor;
}
static void p_glEnable(GLenum c) {
    (void)c;
    scissor = 1;
}
static void p_glDisable(GLenum c) {
    (void)c;
    scissor = 0;
}
static void p_glGenFramebuffers(GLsizei n, GLuint *f) {
    while (n--) *f++ = next_texture++;
}
static void p_glDeleteFramebuffers(GLsizei n, const GLuint *f) {
    (void)n;
    (void)f;
}
static void p_glBindFramebuffer(GLenum t, GLuint f) {
    if (t == GL_READ_FRAMEBUFFER)
        read_fb = f;
    else
        draw_fb = f;
}
static void p_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tex, GLuint id, GLint level) {
    (void)t;
    (void)a;
    (void)tex;
    (void)id;
    (void)level;
}
static GLenum p_glCheckFramebufferStatus(GLenum target) {
    (void)target;
    return bad_gl ? 0 : GL_FRAMEBUFFER_COMPLETE;
}
static void p_glBlitFramebuffer(GLint x, GLint y, GLint w, GLint h, GLint dx, GLint dy, GLint dw, GLint dh,
                                GLbitfield mask, GLenum filter) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)dx;
    (void)dy;
    (void)dw;
    (void)dh;
    (void)mask;
    (void)filter;
    ++blits;
}
#include "swapchain_pool.c"
static int released(XrSwapchain h) {
    int i = pool_find(h);
    uint32_t index;
    CHECK(pool_cycle(i, 0, &index) == XR_SUCCESS && index == 0);
    CHECK(pool_cycle(i, 1, NULL) == XR_SUCCESS);
    CHECK(pool_cycle(i, 2, NULL) == XR_SUCCESS);
    return 0;
}
int main(void) {
    pool_session = (XrSession)1;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.width = 1024;
    ci.height = 128;
    ci.format = GL_SRGB8_ALPHA8;
    ci.faceCount = ci.arraySize = ci.sampleCount = ci.mipCount = 1;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    XrSwapchain first, second, h;
    CHECK(!pool_should_create(pool_session));
    for (int k = 0; k < 8; ++k) pool_note_runtime(pool_session, (XrSwapchain)(uintptr_t)(k + 500));
    CHECK(pool_should_create(pool_session));
    pool_forget_runtime((XrSwapchain)500);
    CHECK(!pool_should_create(pool_session));
    CHECK(!pool_create((XrSession)2, &ci, &h, 0)); // Vulkan/other session
    swapchain_pool = 0;
    CHECK(!pool_create(pool_session, &ci, &h, 0));
    swapchain_pool = 1;
    ci.arraySize = 2;
    CHECK(!pool_create(pool_session, &ci, &h, 0));
    ci.arraySize = 1;
    ci.sampleCount = 4;
    CHECK(!pool_create(pool_session, &ci, &h, 0));
    ci.sampleCount = 1;
    ci.format = GL_DEPTH_COMPONENT24;
    CHECK(!pool_create(pool_session, &ci, &h, 0));
    ci.format = GL_SRGB8_ALPHA8;
    CHECK(pool_create(pool_session, &ci, &first, 0) && texture == 33 && creates == 1);
    CHECK(pool_create(pool_session, &ci, &second, 1) && creates == 1);
    int i = pool_find(first), j = pool_find(second);
    uint32_t n, index;
    CHECK(pool_enumerate(i, 0, &n, NULL) == XR_SUCCESS && n == 1);
    emul_gles_image image = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
    CHECK(pool_enumerate(i, 1, &n, (void *)&image) == XR_SUCCESS && image.image == pool_images[i].tex);
    CHECK(pool_cycle(i, 1, NULL) == XR_ERROR_CALL_ORDER_INVALID);
    CHECK(pool_cycle(i, 0, NULL) == XR_ERROR_VALIDATION_FAILURE);
    CHECK(pool_cycle(i, 0, &index) == XR_SUCCESS);
    CHECK(pool_cycle(i, 0, &index) == XR_ERROR_CALL_ORDER_INVALID);
    CHECK(pool_cycle(i, 2, NULL) == XR_ERROR_CALL_ORDER_INVALID);
    CHECK(pool_cycle(i, 1, NULL) == XR_SUCCESS && pool_cycle(i, 2, NULL) == XR_SUCCESS);
    CHECK(released(second) == 0);
    XrSwapchainSubImage src = {second, {{10, 5}, {100, 50}}, 0}, sub = src;
    pool_begin_frame();
    CHECK(pool_subimage(&sub));
    CHECK(sub.swapchain == pool_atlas[0].handle && sub.imageRect.offset.x == pool_images[j].rect.x + 10);
    CHECK(sub.imageRect.offset.y == 5 && sub.imageRect.extent.width == 100);
    CHECK(read_fb == 31 && draw_fb == 32 && scissor == 1 && texture == 33);
    sub = src;
    CHECK(pool_subimage(&sub) && blits == 1); // two eyes/strips copy source once
    CHECK(acquires == 1 && waits == 1 && releases == 0 && pool_finish() && releases == 1);
    pool_begin_frame();
    sub = src;
    CHECK(pool_subimage(&sub) && blits == 2 && acquires == 2);
    CHECK(pool_finish());
    // Failed wait keeps ownership, retries the same acquired image, never releases before wait.
    pool_begin_frame();
    fail_wait = 1;
    sub = src;
    CHECK(!pool_subimage(&sub));
    CHECK(pool_finish());
    CHECK(acquires == 3 && releases == 2);
    fail_wait = 0;
    CHECK(pool_subimage(&sub) && acquires == 3);
    CHECK(pool_finish());
    pool_begin_frame();
    bad_gl = 1;
    sub = src;
    CHECK(!pool_subimage(&sub));
    CHECK(pool_finish());
    bad_gl = 0;
    CHECK(read_fb == 31 && draw_fb == 32 && scissor == 1);
    pool_begin_frame();
    sub = src;
    CHECK(pool_subimage(&sub));
    fail_release = 1;
    CHECK(!pool_finish());
    fail_release = 0;
    CHECK(pool_finish());
    // Final layer submission preserves order, const inputs, strip crops, stereo and cylinder rects.
    XrCompositionLayerQuad q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.subImage = src;
    XrCompositionLayerCylinderKHR cylinder = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    cylinder.subImage = src;
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    views[0].subImage = views[1].subImage = src;
    XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount = 2;
    projection.views = views;
    const XrCompositionLayerBaseHeader *layers[] = {(void *)&projection, (void *)&q, (void *)&cylinder, (void *)&q};
    PoolFrame frame;
    int before_blits = blits, before_acquires = acquires, before_releases = releases;
    CHECK(pool_rewrite_layers(layers, 4, &frame) == 4);
    CHECK(layers[0]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION &&
          layers[2]->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR);
    CHECK(frame.views[0][1].subImage.swapchain == pool_atlas[0].handle &&
          frame.cylinders[2].subImage.imageRect.offset.x == pool_images[j].rect.x + 10);
    CHECK(q.subImage.swapchain == second && views[1].subImage.swapchain == second);
    CHECK(blits == before_blits + 1 && acquires == before_acquires + 1 && releases == before_releases + 1);
    sub = src;
    sub.imageRect = (XrRect2Di){{0, 0}, {0, 0}};
    pool_begin_frame();
    CHECK(pool_subimage(&sub) && sub.imageRect.extent.width == 1024);
    CHECK(pool_finish());
    sub = src;
    sub.imageRect.offset.x = 2000;
    pool_begin_frame();
    CHECK(!pool_subimage(&sub));
    CHECK(pool_finish());
    sub = src;
    sub.imageRect.offset.x = -10;
    pool_begin_frame();
    CHECK(pool_subimage(&sub) && sub.imageRect.offset.x == pool_images[j].rect.x && sub.imageRect.extent.width == 90);
    CHECK(pool_finish());
    PoolRect old = pool_images[i].rect;
    pool_destroy(i);
    CHECK(pool_create(pool_session, &ci, &h, 0) && pool_images[pool_find(h)].rect.x == old.x);
    // Reporter-sized submenu images keep sharing one runtime handle even when further creates are refused.
    int before_creates = creates;
    fail_create = 1;
    int dimensions[][2] = {{1024, 128}, {1120, 440}, {2048, 256}, {600, 440}, {1000, 440}, {512, 256}, {400, 60}};
    for (int k = 0; k < 7; ++k) {
        ci.width = dimensions[k][0];
        ci.height = dimensions[k][1];
        CHECK(pool_create(pool_session, &ci, &h, 1));
    }
    CHECK(creates == before_creates);
    fail_create = 0;
    // Full atlas spills to a second one, then refuses instead of overlapping.
    pool_cleanup(pool_session);
    pool_session = (XrSession)1;
    ci.width = ci.height = 4096;
    CHECK(pool_create(pool_session, &ci, &first, 0));
    CHECK(pool_create(pool_session, &ci, &second, 0));
    CHECK(pool_images[pool_find(first)].atlas != pool_images[pool_find(second)].atlas);
    CHECK(!pool_create(pool_session, &ci, &h, 0));
    pool_cleanup(pool_session);
    CHECK(!pool_session && pool_find(first) < 0);
    pool_session = (XrSession)1;
    fail_create = 1;
    CHECK(!pool_create(pool_session, &ci, &h, 1));
    fail_create = 0;
    fail_enum = 1;
    CHECK(!pool_create(pool_session, &ci, &h, 0));
    CHECK(!pool_atlas[0].handle);
    fail_enum = 0;
    fail_gl = 1;
    CHECK(!pool_create(pool_session, &ci, &h, 0));
    fail_gl = 0;
    pool_cleanup(pool_session);
    pool_error();
    CHECK(pool_errors == 1 && deleted > 0 && destroys > 0);
    PoolRect occupied[4] = {{0, 0, 2, 2}, {2, 0, 2, 2}, {0, 2, 2, 2}, {2, 2, 2, 2}}, out;
    CHECK(!pool_place(occupied, 4, 4, 1, 1, &out));
    CHECK(!pool_place(occupied, 0, 4, 5, 1, &out));
    CHECK(!pool_place(occupied, 0, 4, 0, 1, &out));
    occupied[1] = occupied[3];
    CHECK(pool_place(occupied, 3, 4, 2, 2, &out) && out.x == 2 && out.y == 0);
    puts("ok");
    return 0;
}
