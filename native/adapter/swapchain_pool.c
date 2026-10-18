// Included after layer_emul_gl.c: reuses its dynamically loaded GLES entry points.
// Proactive at eight app swapchains: an atlas cannot be allocated after the runtime cap.
#include "pool_rect.h"
#define POOL_MAX 128
#define POOL_SIDE 4096
static XrSession pool_session;
static struct {
    XrSwapchain handle;
    GLuint images[16];
    uint32_t count, index;
    int acquired, waited;
} pool_atlas[2];
static struct {
    XrSwapchain handle;
    GLuint tex;
    PoolRect rect;
    int atlas, state, released, copied;
    GLsync fence, read_fence;
} pool_images[POOL_MAX];
static unsigned pool_errors;
static XrSwapchain pool_runtime[256];
static unsigned pool_runtime_count;
static void pool_note_runtime_locked(XrSession session, XrSwapchain handle) {
    if (!swapchain_pool || session != pool_session) return;
    for (int i = 0; i < 256; ++i)
        if (!pool_runtime[i]) {
            pool_runtime[i] = handle;
            ++pool_runtime_count;
            return;
        }
}
static void pool_forget_runtime_locked(XrSwapchain handle) {
    for (int i = 0; i < 256; ++i)
        if (pool_runtime[i] == handle && handle) {
            pool_runtime[i] = XR_NULL_HANDLE;
            --pool_runtime_count;
            return;
        }
}
static int pool_should_create_locked(XrSession session) {
    return swapchain_pool && session == pool_session && pool_runtime_count >= 8;
}
static int pool_find_locked(XrSwapchain h) {
    for (int i = 0; h && i < POOL_MAX; ++i)
        if (pool_images[i].handle == h) return i;
    return -1;
}
static void pool_error(void) {
    if (pool_errors++ % 240 == 0) LOG("swapchain_pool: atlas update failed; omitting affected layers");
}
static int pool_create_locked(XrSession session, const XrSwapchainCreateInfo *ci, XrSwapchain *out, int limited) {
    if (!swapchain_pool || session != pool_session || !out || ci->next || ci->faceCount != 1 || ci->arraySize != 1 ||
        ci->sampleCount != 1 || ci->mipCount != 1 || !(ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) ||
        (ci->createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ||
        (ci->format != GL_RGBA8 && ci->format != GL_SRGB8_ALPHA8) || !ci->width || !ci->height ||
        ci->width > POOL_SIDE || ci->height > POOL_SIDE)
        return 0;
    if (!p_eglCreateContext && !emul_load_gl()) return 0;
    if (p_eglGetCurrentContext() == EGL_NO_CONTEXT) return 0;
    // Like emul_virtual_create: stale application GL errors must not look like allocation failures.
    for (int i = 0; i < 8 && p_glGetError() != GL_NO_ERROR; ++i) {
    }
    int slot = -1;
    for (int i = 0; i < POOL_MAX; ++i)
        if (!pool_images[i].handle) {
            slot = i;
            break;
        }
    if (slot < 0) return 0;
    for (int a = 0; a < 2; ++a) {
        PoolRect used[POOL_MAX], rect;
        int n = 0;
        for (int i = 0; i < POOL_MAX; ++i)
            if (pool_images[i].handle && pool_images[i].atlas == a) used[n++] = pool_images[i].rect;
        if (!pool_place(used, n, POOL_SIDE, ci->width, ci->height, &rect)) continue;
        if (!pool_atlas[a].handle) {
            XrSwapchainCreateInfo ai = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ai.width = ai.height = POOL_SIDE;
            ai.arraySize = ai.faceCount = ai.sampleCount = ai.mipCount = 1;
            ai.format = GL_SRGB8_ALPHA8;
            ai.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            PFN_xrCreateSwapchain create = (PFN_xrCreateSwapchain)lookup(active_instance, "xrCreateSwapchain");
            PFN_xrEnumerateSwapchainImages enumerate =
                (PFN_xrEnumerateSwapchainImages)lookup(active_instance, "xrEnumerateSwapchainImages");
            if (XR_FAILED(create(session, &ai, &pool_atlas[a].handle))) return 0;
            emul_gles_image images[16];
            for (int j = 0; j < 16; ++j) images[j] = (emul_gles_image){XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
            if (XR_FAILED(enumerate(pool_atlas[a].handle, 16, &pool_atlas[a].count, (void *)images)) ||
                !pool_atlas[a].count || pool_atlas[a].count > 16) {
                ((PFN_xrDestroySwapchain)lookup(active_instance, "xrDestroySwapchain"))(pool_atlas[a].handle);
                memset(&pool_atlas[a], 0, sizeof pool_atlas[a]);
                return 0;
            }
            for (uint32_t j = 0; j < pool_atlas[a].count; ++j) pool_atlas[a].images[j] = images[j].image;
        }
        GLint old;
        GLuint tex = 0;
        p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
        p_glGenTextures(1, &tex);
        p_glBindTexture(GL_TEXTURE_2D, tex);
        p_glTexStorage2D(GL_TEXTURE_2D, 1, ci->format, ci->width, ci->height);
        GLenum err = p_glGetError();
        p_glBindTexture(GL_TEXTURE_2D, old);
        if (err != GL_NO_ERROR) {
            p_glDeleteTextures(1, &tex);
            return 0;
        }
        pool_images[slot].handle = FAKE_HANDLE(XrSwapchain);
        pool_images[slot].tex = tex;
        pool_images[slot].rect = rect;
        pool_images[slot].atlas = a;
        *out = pool_images[slot].handle;
        note_swapchain_size(*out, ci->width, ci->height);
        remember_swapchain(*out);
        emul_on_create_swapchain(*out, ci);
        LOG("swapchain_pool: virtual %ux%u (%s)", ci->width, ci->height,
            limited ? "runtime limit reached" : "runtime limit proactively avoided");
        return 1;
    }
    return 0;
}
static void pool_destroy_locked(int i) {
    emul_on_destroy_swapchain(pool_images[i].handle);
    if (pool_images[i].fence && p_eglGetCurrentContext && p_eglGetCurrentContext() != EGL_NO_CONTEXT)
        p_glDeleteSync(pool_images[i].fence);
    if (p_eglGetCurrentContext && p_eglGetCurrentContext() != EGL_NO_CONTEXT)
        p_glDeleteTextures(1, &pool_images[i].tex);
    if (pool_images[i].read_fence && p_eglGetCurrentContext && p_eglGetCurrentContext() != EGL_NO_CONTEXT)
        p_glDeleteSync(pool_images[i].read_fence);
    forget_swapchain(pool_images[i].handle);
    memset(&pool_images[i], 0, sizeof pool_images[i]);
}
static XrResult pool_enumerate_locked(int i, uint32_t cap, uint32_t *count, XrSwapchainImageBaseHeader *images) {
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = 1;
    if (!cap) return XR_SUCCESS;
    if (!images || images->type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) return XR_ERROR_VALIDATION_FAILURE;
    ((emul_gles_image *)images)->image = pool_images[i].tex;
    return XR_SUCCESS;
}
static XrResult pool_cycle_locked(int i, int op, uint32_t *index) {
    int *state = &pool_images[i].state;
    if (op == 0) {
        if (!index) return XR_ERROR_VALIDATION_FAILURE;
        if (*state) return XR_ERROR_CALL_ORDER_INVALID;
        if (pool_images[i].read_fence) {
            if (p_eglGetCurrentContext() == EGL_NO_CONTEXT) return XR_ERROR_RUNTIME_FAILURE;
            p_glWaitSync(pool_images[i].read_fence, 0, GL_TIMEOUT_IGNORED);
        }
        *index = 0;
        *state = 1;
    }
    if (op == 1) {
        if (*state != 1) return XR_ERROR_CALL_ORDER_INVALID;
        *state = 2;
    }
    if (op == 2) {
        if (*state != 2) return XR_ERROR_CALL_ORDER_INVALID;
        if (p_eglGetCurrentContext() == EGL_NO_CONTEXT) return XR_ERROR_RUNTIME_FAILURE;
        if (pool_images[i].fence) p_glDeleteSync(pool_images[i].fence);
        pool_images[i].fence = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!pool_images[i].fence) return XR_ERROR_RUNTIME_FAILURE;
        p_glFlush();
        *state = 0;
        pool_images[i].released = 1;
    }
    return XR_SUCCESS;
}
static void pool_cleanup_locked(XrSession session) {
    if (session != pool_session) return;
    for (int i = 0; i < POOL_MAX; ++i)
        if (pool_images[i].handle) pool_destroy_locked(i);
    for (int a = 0; a < 2; ++a)
        if (pool_atlas[a].handle) {
            ((PFN_xrDestroySwapchain)lookup(active_instance, "xrDestroySwapchain"))(pool_atlas[a].handle);
            memset(&pool_atlas[a], 0, sizeof pool_atlas[a]);
        }
    memset(pool_runtime, 0, sizeof pool_runtime);
    pool_runtime_count = 0;
    pool_session = XR_NULL_HANDLE;
}
// Called on final layers, after strips, flips and rect_clamp. Copy every referenced image
// each frame: runtime atlas images rotate, and untouched areas are not preserved.
static void pool_begin_frame_locked(void) {
    for (int i = 0; i < POOL_MAX; ++i) pool_images[i].copied = 0;
}
static int pool_subimage_locked(XrSwapchainSubImage *sub) {
    int i = pool_find_locked(sub->swapchain);
    if (i < 0) return 1;
    int a = pool_images[i].atlas;
    if (!pool_images[i].released || pool_images[i].state || sub->imageArrayIndex) return 0;
    PFN_xrAcquireSwapchainImage acquire =
        (PFN_xrAcquireSwapchainImage)lookup(active_instance, "xrAcquireSwapchainImage");
    PFN_xrWaitSwapchainImage wait = (PFN_xrWaitSwapchainImage)lookup(active_instance, "xrWaitSwapchainImage");
    if (!pool_atlas[a].acquired) {
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(acquire(pool_atlas[a].handle, &ai, &pool_atlas[a].index))) return 0;
        pool_atlas[a].acquired = 1;
    }
    if (!pool_atlas[a].waited) {
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION};
        if (wait(pool_atlas[a].handle, &wi) != XR_SUCCESS) return 0;
        pool_atlas[a].waited = 1;
    }
    if (p_eglGetCurrentContext() == EGL_NO_CONTEXT) return 0;
    PoolRect r = pool_images[i].rect;
    if (!pool_images[i].copied) {
        for (int e = 0; e < 8 && p_glGetError() != GL_NO_ERROR; ++e) {
        }
        if (pool_images[i].fence) p_glWaitSync(pool_images[i].fence, 0, GL_TIMEOUT_IGNORED);
        GLint read, draw;
        GLuint fb[2];
        GLboolean scissor = p_glIsEnabled(GL_SCISSOR_TEST);
        p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        p_glGenFramebuffers(2, fb);
        p_glDisable(GL_SCISSOR_TEST);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
        p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pool_images[i].tex, 0);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
        p_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                 pool_atlas[a].images[pool_atlas[a].index], 0);
        int ok = p_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                 p_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok) p_glBlitFramebuffer(0, 0, r.w, r.h, r.x, r.y, r.x + r.w, r.y + r.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        ok &= p_glGetError() == GL_NO_ERROR;
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        if (scissor) p_glEnable(GL_SCISSOR_TEST);
        p_glDeleteFramebuffers(2, fb);
        if (!ok) return 0;
        if (pool_images[i].read_fence) p_glDeleteSync(pool_images[i].read_fence);
        pool_images[i].read_fence = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!pool_images[i].read_fence) return 0;
        pool_images[i].copied = 1;
    }
    clamp_rect(&sub->imageRect, r.w, r.h);
    if (sub->imageRect.offset.x < 0 || sub->imageRect.offset.y < 0 ||
        (int64_t)sub->imageRect.offset.x + sub->imageRect.extent.width > r.w ||
        (int64_t)sub->imageRect.offset.y + sub->imageRect.extent.height > r.h)
        return 0;
    sub->swapchain = pool_atlas[a].handle;
    sub->imageRect.offset.x += r.x;
    sub->imageRect.offset.y += r.y;
    return 1;
}
static int pool_finish_locked(void) {
    int ok = 1;
    for (int a = 0; a < 2; ++a)
        if (pool_atlas[a].acquired && pool_atlas[a].waited) {
            p_glFlush();
            XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XrResult r = ((PFN_xrReleaseSwapchainImage)lookup(active_instance, "xrReleaseSwapchainImage"))(
                pool_atlas[a].handle, &ri);
            if (XR_FAILED(r))
                ok = 0;
            else
                pool_atlas[a].acquired = pool_atlas[a].waited = 0;
        }
    return ok;
}

// Each OpenXR call may run on a different thread, including creates of different panels.
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;

static int pool_find(XrSwapchain h) {
    pthread_mutex_lock(&pool_lock);
    int result = pool_find_locked(h);
    pthread_mutex_unlock(&pool_lock);
    return result;
}

static int pool_create(XrSession session, const XrSwapchainCreateInfo *ci, XrSwapchain *out, int limited) {
    pthread_mutex_lock(&pool_lock);
    int result = pool_create_locked(session, ci, out, limited);
    pthread_mutex_unlock(&pool_lock);
    return result;
}

static void pool_destroy(int i) {
    pthread_mutex_lock(&pool_lock);
    pool_destroy_locked(i);
    pthread_mutex_unlock(&pool_lock);
}

static void pool_cleanup(XrSession session) {
    pthread_mutex_lock(&pool_lock);
    pool_cleanup_locked(session);
    pthread_mutex_unlock(&pool_lock);
}

static XrResult pool_cycle(int i, int op, uint32_t *index) {
    pthread_mutex_lock(&pool_lock);
    XrResult result = pool_cycle_locked(i, op, index);
    pthread_mutex_unlock(&pool_lock);
    return result;
}

static XrResult pool_enumerate(int i, uint32_t cap, uint32_t *count, XrSwapchainImageBaseHeader *images) {
    pthread_mutex_lock(&pool_lock);
    XrResult result = pool_enumerate_locked(i, cap, count, images);
    pthread_mutex_unlock(&pool_lock);
    return result;
}

static void pool_begin_frame(void) {
    pthread_mutex_lock(&pool_lock);
    pool_begin_frame_locked();
    pthread_mutex_unlock(&pool_lock);
}

static int pool_subimage(XrSwapchainSubImage *sub) {
    pthread_mutex_lock(&pool_lock);
    int result = pool_subimage_locked(sub);
    pthread_mutex_unlock(&pool_lock);
    return result;
}

static int pool_finish(void) {
    pthread_mutex_lock(&pool_lock);
    int result = pool_finish_locked();
    pthread_mutex_unlock(&pool_lock);
    return result;
}

static void pool_note_runtime(XrSession session, XrSwapchain handle) {
    pthread_mutex_lock(&pool_lock);
    pool_note_runtime_locked(session, handle);
    pthread_mutex_unlock(&pool_lock);
}

static void pool_forget_runtime(XrSwapchain handle) {
    pthread_mutex_lock(&pool_lock);
    pool_forget_runtime_locked(handle);
    pthread_mutex_unlock(&pool_lock);
}

static int pool_should_create(XrSession session) {
    pthread_mutex_lock(&pool_lock);
    int result = pool_should_create_locked(session);
    pthread_mutex_unlock(&pool_lock);
    return result;
}

// Caller-owned storage stays live until the runtime's xrEndFrame returns. The input layer
// structs are const: never mutate them, including repeated references from strips or stereo.
typedef struct {
    XrCompositionLayerQuad quads[64];
    XrCompositionLayerCylinderKHR cylinders[64];
    XrCompositionLayerProjection projections[64];
    XrCompositionLayerProjectionView views[64][2];
} PoolFrame;
static uint32_t pool_rewrite_layers(const XrCompositionLayerBaseHeader **layers, uint32_t count, PoolFrame *frame) {
    pool_begin_frame();
    uint32_t n = 0;
    for (uint32_t j = 0; j < count; ++j) {
        const XrCompositionLayerBaseHeader *l = layers[j];
        int ok = 1;
        if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            frame->quads[j] = *(const XrCompositionLayerQuad *)l;
            ok = pool_subimage(&frame->quads[j].subImage);
            l = (const void *)&frame->quads[j];
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            frame->cylinders[j] = *(const XrCompositionLayerCylinderKHR *)l;
            ok = pool_subimage(&frame->cylinders[j].subImage);
            l = (const void *)&frame->cylinders[j];
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection *p = (const void *)l;
            if (p->viewCount <= 2) {
                frame->projections[j] = *p;
                for (uint32_t v = 0; v < p->viewCount; ++v) {
                    frame->views[j][v] = p->views[v];
                    ok &= pool_subimage(&frame->views[j][v].subImage);
                }
                frame->projections[j].views = frame->views[j];
                l = (const void *)&frame->projections[j];
            }
        }
        if (ok)
            layers[n++] = l;
        else
            pool_error();
    }
    if (!pool_finish()) {
        pool_error();
        return 0;
    }
    return n;
}
