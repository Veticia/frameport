// Host runtime/GL doubles exercise the actual adapter implementation, without a headset.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>
#define CHECK(c)                                                                                                     \
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
static int swapchain_pool = 1, layer_debug;
static int creates, destroys, acquires, waits, releases, blits, deleted, fail_create, fail_enum, fail_wait,
    fail_release;
static int bad_gl, fail_gl, scissor = 1, read_fb = 31, draw_fb = 32, texture = 33;
static GLuint next_texture = 1000;
typedef struct {
    XrStructureType type;
    void *next;
    uint32_t image;
} emul_gles_image;
static int64_t atlas_format;
static XrResult create(XrSession s, const XrSwapchainCreateInfo *ci, XrSwapchain *h) {
    (void)s;
    atlas_format = ci->format;
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
    if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)
        for (int j = 0; j < 3; ++j) ((XrSwapchainImageVulkanKHR *)images)[j].image = (VkImage)(uintptr_t)(3000 + j);
    else
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
// ---- Vulkan doubles: flip_vk.c's session binding + a device that executes commands as they are recorded and
// checks every barrier's old layout and every copy's layouts and region.
static struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family, index;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
} vk;
static pthread_mutex_t flip_lock = PTHREAD_MUTEX_INITIALIZER;
static int vk_fail_ready, vk_fail_image, vk_fail_submit, vk_bad_layouts, vk_submits, vk_fence_waits, vk_copies,
    vk_images_alive, vk_memory_alive, vk_fences_alive, vk_pools_alive, vk_recording, vk_flip_tracked;
static int vk_ready(void) { return !vk_fail_ready && vk.device; }
static struct {
    VkImage image;
    VkImageLayout layout;
} vk_layouts[64];
static VkImageCreateInfo vk_last_image;
static VkImageCopy vk_last_copy;
static VkImageBlit vk_last_blit;
static VkImage vk_last_dst, vk_last_blit_src;
static int vk_blits;
static uint32_t vk_last_memory_type;
static VkImageLayout *vk_layout(VkImage image) {
    for (int k = 0; k < 64; ++k)
        if (vk_layouts[k].image == image) return &vk_layouts[k].layout;
    for (int k = 0; k < 64; ++k)
        if (!vk_layouts[k].image) {  // runtime atlas images come out of xrWaitSwapchainImage in this layout
            vk_layouts[k].image = image;
            vk_layouts[k].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            return &vk_layouts[k].layout;
        }
    return NULL;
}
static uintptr_t vk_next = 9000;
static VkResult fk_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void)d;
    (void)a;
    if (vk_fail_image) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    vk_last_image = *ci;
    *out = (VkImage)++vk_next;
    *vk_layout(*out) = ci->initialLayout;
    ++vk_images_alive;
    return VK_SUCCESS;
}
static void fk_DestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks *a) {
    (void)d;
    (void)a;
    for (int k = 0; k < 64; ++k)
        if (vk_layouts[k].image == image) vk_layouts[k].image = VK_NULL_HANDLE;
    --vk_images_alive;
}
static void fk_GetImageMemoryRequirements(VkDevice d, VkImage image, VkMemoryRequirements *req) {
    (void)d;
    (void)image;
    *req = (VkMemoryRequirements){1 << 20, 4096, 3};
}
static VkResult fk_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a,
                                  VkDeviceMemory *out) {
    (void)d;
    (void)a;
    vk_last_memory_type = ai->memoryTypeIndex;
    *out = (VkDeviceMemory)++vk_next;
    ++vk_memory_alive;
    return VK_SUCCESS;
}
static void fk_FreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void)d;
    (void)a;
    if (m) --vk_memory_alive;
}
static VkResult fk_BindImageMemory(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize o) {
    (void)d;
    (void)i;
    (void)m;
    (void)o;
    return VK_SUCCESS;
}
static VkResult fk_CreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a,
                                     VkCommandPool *out) {
    (void)d;
    (void)a;
    if (ci->queueFamilyIndex != vk.family || !(ci->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT))
        return VK_ERROR_INITIALIZATION_FAILED;
    *out = (VkCommandPool)++vk_next;
    ++vk_pools_alive;
    return VK_SUCCESS;
}
static void fk_DestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) {
    (void)d;
    (void)p;
    (void)a;
    --vk_pools_alive;
}
static VkResult fk_AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
    (void)d;
    for (uint32_t k = 0; k < ai->commandBufferCount; ++k) out[k] = (VkCommandBuffer)++vk_next;
    return VK_SUCCESS;
}
static VkResult fk_BeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *bi) {
    (void)c;
    (void)bi;
    if (vk_recording) return VK_ERROR_VALIDATION_FAILED_EXT;
    vk_recording = 1;
    return VK_SUCCESS;
}
static VkResult fk_EndCommandBuffer(VkCommandBuffer c) {
    (void)c;
    if (!vk_recording) return VK_ERROR_VALIDATION_FAILED_EXT;
    vk_recording = 0;
    return VK_SUCCESS;
}
static VkResult fk_ResetCommandBuffer(VkCommandBuffer c, VkCommandBufferResetFlags f) {
    (void)c;
    (void)f;
    return VK_SUCCESS;
}
static void fk_CmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags d,
                                  VkDependencyFlags f, uint32_t mc, const VkMemoryBarrier *m, uint32_t bc,
                                  const VkBufferMemoryBarrier *b, uint32_t ic, const VkImageMemoryBarrier *ib) {
    (void)c;
    (void)s;
    (void)d;
    (void)f;
    (void)mc;
    (void)m;
    (void)bc;
    (void)b;
    if (!vk_recording) ++vk_bad_layouts;
    for (uint32_t k = 0; k < ic; ++k) {
        VkImageLayout *l = vk_layout(ib[k].image);
        if (!l || (ib[k].oldLayout != VK_IMAGE_LAYOUT_UNDEFINED && ib[k].oldLayout != *l)) ++vk_bad_layouts;
        if (l) *l = ib[k].newLayout;
    }
}
static void fk_CmdCopyImage(VkCommandBuffer c, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                            uint32_t n, const VkImageCopy *r) {
    (void)c;
    if (!vk_recording || n != 1 || sl != *vk_layout(src) || dl != *vk_layout(dst) ||
        sl != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || dl != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        ++vk_bad_layouts;
    vk_last_copy = r[0];
    vk_last_dst = dst;
    ++vk_copies;
}
static void fk_CmdBlitImage(VkCommandBuffer c, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                            uint32_t n, const VkImageBlit *r, VkFilter filter) {
    (void)c;
    if (!vk_recording || n != 1 || sl != *vk_layout(src) || dl != *vk_layout(dst) ||
        sl != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || dl != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        filter != VK_FILTER_NEAREST)
        ++vk_bad_layouts;
    vk_last_blit = r[0];
    vk_last_blit_src = src;
    vk_last_dst = dst;
    ++vk_blits;
}
static VkResult fk_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    (void)s;
    if (q != vk.queue || n != 1 || !f || vk_recording) ++vk_bad_layouts;
    if (vk_fail_submit) return VK_ERROR_DEVICE_LOST;
    ++vk_submits;
    return VK_SUCCESS;
}
static VkResult fk_CreateFence(VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) {
    (void)d;
    (void)ci;
    (void)a;
    *out = (VkFence)++vk_next;
    ++vk_fences_alive;
    return VK_SUCCESS;
}
static void fk_DestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a) {
    (void)d;
    (void)a;
    if (f) --vk_fences_alive;
}
static VkResult fk_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t) {
    (void)d;
    (void)n;
    (void)f;
    (void)all;
    (void)t;
    ++vk_fence_waits;
    return VK_SUCCESS;
}
static VkResult fk_ResetFences(VkDevice d, uint32_t n, const VkFence *f) {
    (void)d;
    (void)n;
    (void)f;
    return VK_SUCCESS;
}
static PFN_vkVoidFunction fk_GetDeviceProcAddr(VkDevice d, const char *name) {
    if (d != vk.device) return NULL;
#define FK(n)                                                                                                          \
    if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)fk_##n
    FK(CreateImage);
    FK(DestroyImage);
    FK(GetImageMemoryRequirements);
    FK(AllocateMemory);
    FK(FreeMemory);
    FK(BindImageMemory);
    FK(CreateCommandPool);
    FK(DestroyCommandPool);
    FK(AllocateCommandBuffers);
    FK(BeginCommandBuffer);
    FK(EndCommandBuffer);
    FK(ResetCommandBuffer);
    FK(CmdPipelineBarrier);
    FK(CmdCopyImage);
    FK(CmdBlitImage);
    FK(QueueSubmit);
    FK(CreateFence);
    FK(DestroyFence);
    FK(WaitForFences);
    FK(ResetFences);
    return NULL;
}
static XrSwapchainCreateInfo vk_flip_info;
static void flip_on_create_swapchain(XrSwapchain h, const XrSwapchainCreateInfo *ci) {
    (void)h;
    vk_flip_info = *ci;
    ++vk_flip_tracked;
}
static void flip_on_destroy(XrSwapchain h) {
    (void)h;
    --vk_flip_tracked;
}
// flip_vk.c's flip interface: one runtime quad swapchain (6000) whose shown image is 4000 (1024x256, sRGB)
static int flip_on = 1, flip_source_ok = 1, dump_records, dump_collects;
static int flip_emulated(void) { return flip_on; }
static int wants_vertical_flip(const XrCompositionLayerQuad *q) {
    for (const XrBaseInStructure *n = (const XrBaseInStructure *)q->next; n; n = n->next)
        if (n->type == (XrStructureType)1000040000) return (((const XrCompositionLayerImageLayoutFB *)n)->flags & 1) != 0;
    return 0;
}
static int flip_source(XrSwapchain h, uint32_t layer, VkImage *image, int64_t *format, uint32_t *w, uint32_t *he) {
    if (!flip_source_ok || h != (XrSwapchain)6000 || layer) return 0;
    *image = (VkImage)4000;
    *format = VK_FORMAT_R8G8B8A8_SRGB;
    *w = 1024;
    *he = 256;
    return 1;
}
static int flip_dump_due(XrSwapchain h) { (void)h; return 1; }
static void flip_dump_record(VkCommandBuffer cmd, int slot, XrSwapchain source, VkImage atlas, int32_t x, int32_t y,
                             uint32_t w, uint32_t h) {
    (void)cmd; (void)slot; (void)source; (void)x; (void)y; (void)w; (void)h;
    if (*vk_layout(atlas) != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ++vk_bad_layouts;
    ++dump_records;
}
static void flip_dump_collect(int slot, int ok) { (void)slot; (void)ok; ++dump_collects; }
static void flip_dump_free(void) {}
#include "swapchain_pool.c"
// AC Nexus (#424): its panels, then the Quest 3 eye set while the old one lives. Returns the live runtime count.
static int vk_session_counts(int *pooled) {
    static const int panels[][2] = {{1200, 1020}, {768, 153}, {1459, 1228}, {855, 1500}, {1875, 1500}, {2457, 1152},
                                    {512, 512},   {460, 102}, {691, 153},   {360, 360},  {1200, 1020}, {768, 153}};
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.faceCount = ci.sampleCount = ci.mipCount = 1;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    int live = 0;
    XrSwapchain h;
    ci.arraySize = 2;  // the first eye set: colour + 384x384 extras; never pooled
    int64_t eye_formats[] = {VK_FORMAT_R8G8B8A8_SRGB, 97, 129};
    for (int k = 0; k < 3; ++k) {
        ci.format = eye_formats[k];
        ci.width = ci.height = k ? 384 : 1824;
        if (pool_should_create(pool_session) && pool_create(pool_session, &ci, &h, 0)) return -1;
        pool_note_runtime(pool_session, (XrSwapchain)(uintptr_t)(700 + k));
        ++live;
    }
    ci.arraySize = 1;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    for (int k = 0; k < 12; ++k) {
        ci.width = panels[k][0];
        ci.height = panels[k][1];
        if (pool_should_create(pool_session) && pool_create(pool_session, &ci, &h, 0)) {
            ++*pooled;
            continue;
        }
        pool_note_runtime(pool_session, (XrSwapchain)(uintptr_t)(800 + k));
        ++live;
    }
    for (int a = 0; a < 2; ++a) live += pool_atlas[a].handle != XR_NULL_HANDLE;
    ci.arraySize = 2;  // the Quest 3 set: 2592x2256 colour
    ci.width = 2592;
    ci.height = 2256;
    if (pool_create(pool_session, &ci, &h, 1)) return -1;
    return live + 3;
}
// frame_adapter.c's xrCreateSwapchain order against a runtime that refuses the 16th live swapchain (-10).
static int vk_live;
static int vk_flow(const XrSwapchainCreateInfo *ci, XrSwapchain *h) {
    if (pool_should_create(pool_session) && pool_create(pool_session, ci, h, 0)) return 1;  // pooled
    int atlases = (pool_atlas[0].handle != XR_NULL_HANDLE) + (pool_atlas[1].handle != XR_NULL_HANDLE);
    if (vk_live + atlases >= 15) return pool_create(pool_session, ci, h, 1) ? 1 : -10;
    *h = (XrSwapchain)(uintptr_t)(5000 + vk_live++);
    pool_note_runtime(pool_session, *h);
    flip_on_create_swapchain(*h, ci);  // runtime quads are flip-tracked too (flip_wants_transfer adds 0x30)
    return 0;
}
// The device run of 3f5210fa (AC Nexus, Quest 3): nothing was pooled. Usage 0x39 panels, foveation/META chains and
// mipmapped panels must pool; the 2592x2256 eye set must then fit under the cap.
static int vk_device_sequence(void) {
    XrGraphicsBindingVulkanKHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, &binding};
    pool_on_create_session((XrSession)9, &sci);
    vk_live = 0;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.faceCount = ci.sampleCount = ci.mipCount = 1;
    XrSwapchain h;
    int64_t eye_formats[] = {VK_FORMAT_R8G8B8A8_SRGB, 97, 129};
    ci.arraySize = 2;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    for (int k = 0; k < 3; ++k) {
        ci.format = eye_formats[k];
        ci.width = ci.height = k ? 384 : 1728;
        CHECK(vk_flow(&ci, &h) == 0);
    }
    ci.arraySize = 1;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    ci.usageFlags = 0x39;  // COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST | SAMPLED
    ci.width = ci.height = 2;
    CHECK(vk_flow(&ci, &h) == 0);
    ci.width = ci.height = 1024;
    CHECK(vk_flow(&ci, &h) == 0);
    ci.arraySize = 2;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    for (int k = 0; k < 3; ++k) {
        ci.format = eye_formats[k];
        ci.width = k ? 384 : 2016;
        ci.height = k ? 384 : 1728;
        CHECK(vk_flow(&ci, &h) == 0);
    }
    CHECK(vk_live == 8);
    ci.arraySize = 1;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    ci.usageFlags = 0x39;
    XrSwapchainCreateInfoFoveationFB foveation = {XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
    XrVulkanSwapchainCreateInfoMETA meta = {XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META, &foveation,
                                            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_USAGE_STORAGE_BIT};
    static const int panels[][2] = {{512, 512},  {2457, 1152}, {1875, 1500}, {1875, 1500}, {855, 1500}, {855, 1500},
                                    {1075, 1382}, {1459, 1228}, {768, 153},   {1200, 1020}, {691, 153},  {360, 360}};
    int pooled = 0;
    for (int k = 0; k < 12; ++k) {
        ci.width = panels[k][0];
        ci.height = panels[k][1];
        ci.next = k == 1 ? (const void *)&meta : k == 2 ? (const void *)&foveation : NULL;
        ci.mipCount = k == 3 ? 4 : 1;
        int r = vk_flow(&ci, &h);
        CHECK(r == 1);
        ++pooled;
        if (k == 1)
            CHECK((vk_last_image.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) &&
                  (vk_last_image.usage & VK_IMAGE_USAGE_STORAGE_BIT));
        if (k == 3) CHECK(vk_last_image.mipLevels == 4);
    }
    ci.next = NULL;
    ci.mipCount = 1;
    CHECK(pooled == 12 && vk_live == 8);
    // The Quest 3 eye set fits: 8 app swapchains + atlases + 3.
    ci.arraySize = 2;
    for (int k = 0; k < 3; ++k) {
        ci.format = eye_formats[k];
        ci.width = k ? 384 : 2592;
        ci.height = k ? 384 : 2256;
        CHECK(vk_flow(&ci, &h) == 0 && pool_last_decline == POOL_SHAPE);  // offered to the pool, never pooled
    }
    int atlases = (pool_atlas[0].handle != XR_NULL_HANDLE) + (pool_atlas[1].handle != XR_NULL_HANDLE);
    CHECK(vk_live == 11 && atlases >= 1 && vk_live + atlases <= 13);
    // A mipmapped panel copies level 0 and keeps every level's layout (barriers cover all levels).
    int i = -1;
    for (int k = 0; k < POOL_MAX; ++k)  // the last 1875x1500 one has 4 mip levels
        if (pool_images[k].handle && pool_images[k].rect.w == 1875) i = k;
    uint32_t index;
    CHECK(i >= 0 && pool_cycle(i, 0, &index) == XR_SUCCESS && pool_cycle(i, 1, NULL) == XR_SUCCESS &&
          pool_cycle(i, 2, NULL) == XR_SUCCESS);
    XrSwapchainSubImage sub = {pool_images[i].handle, {{0, 0}, {1875, 1500}}, 0};
    pool_begin_frame_locked();
    CHECK(pool_subimage_locked(&sub, 0) && pool_finish_locked() && !vk_bad_layouts);
    CHECK(vk_last_copy.srcSubresource.mipLevel == 0 && vk_last_copy.dstSubresource.mipLevel == 0);
    // Every decline reason is reported: an unknown chained struct, no mip level.
    XrBaseInStructure unknown = {(XrStructureType)1000999000};
    ci.arraySize = 1;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    ci.width = ci.height = 64;
    ci.next = &unknown;
    CHECK(!pool_create(pool_session, &ci, &h, 1) && pool_last_decline == POOL_CHAIN);
    ci.next = NULL;
    ci.mipCount = 0;
    CHECK(!pool_create(pool_session, &ci, &h, 1) && pool_last_decline == POOL_MIPS);
    ci.mipCount = 1;
    ci.format = VK_FORMAT_B8G8R8A8_SRGB;
    CHECK(!pool_create(pool_session, &ci, &h, 1) && pool_last_decline == POOL_FORMAT);
    CHECK(!pool_create((XrSession)77, &ci, &h, 1) && pool_last_decline == POOL_NO_SESSION);
    pool_cleanup(pool_session);
    CHECK(vk_images_alive == 0 && vk_memory_alive == 0 && vk_fences_alive == 0);
    return 0;
}
static int vk_main(void) {
    // Session detection: only bindings with a pool path, only with the setting.
    XrGraphicsBindingVulkanKHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, &binding};
    swapchain_pool = 0;
    flip_on = 0;
    pool_on_create_session((XrSession)5, &sci);
    CHECK(!pool_session && !pool_vulkan);
    flip_on = 1;  // flip emulation alone: the Vulkan session gets atlases, but nothing is pooled
    pool_on_create_session((XrSession)5, &sci);
    CHECK(pool_session == (XrSession)5 && pool_vulkan && pool_flips && !pool_should_create(pool_session));
    pool_cleanup((XrSession)5);
    CHECK(!pool_session && !pool_flips);
    swapchain_pool = 1;
    XrBaseInStructure other = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    sci.next = &other;
    pool_on_create_session((XrSession)4, &sci);
    CHECK(pool_session == (XrSession)4 && !pool_vulkan);
    pool_cleanup((XrSession)4);
    sci.next = &binding;
    pool_on_create_session((XrSession)5, &sci);
    CHECK(pool_session == (XrSession)5 && pool_vulkan);
    vk.device = (VkDevice)7;
    vk.queue = (VkQueue)8;
    vk.family = 2;
    vk.GetDeviceProcAddr = fk_GetDeviceProcAddr;
    vk.memory.memoryTypeCount = 2;
    vk.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    vk.memory.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.width = 1200;
    ci.height = 1020;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    ci.faceCount = ci.arraySize = ci.sampleCount = ci.mipCount = 1;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchain panel, h;
    // Other formats keep runtime swapchains (no format conversion in vkCmdCopyImage); so do eye images.
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    CHECK(!pool_create(pool_session, &ci, &h, 1));
    ci.format = GL_SRGB8_ALPHA8;
    CHECK(!pool_create(pool_session, &ci, &h, 1));
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    ci.arraySize = 2;
    CHECK(!pool_create(pool_session, &ci, &h, 1));
    ci.arraySize = 1;
    vk_fail_ready = 1;  // no Vulkan device functions: runtime swapchain, and no retry in this session
    CHECK(!pool_create(pool_session, &ci, &h, 1) && pvk.state < 0);
    vk_fail_ready = 0;
    pvk.state = 0;
    vk_fail_image = 1;
    int before_creates = creates;
    CHECK(!pool_create(pool_session, &ci, &h, 1) && pvk.state > 0);
    vk_fail_image = 0;
    CHECK(pool_create(pool_session, &ci, &panel, 0) && creates == before_creates + 1);
    CHECK(atlas_format == VK_FORMAT_R8G8B8A8_SRGB && pool_atlas[0].vk_images[2] == (VkImage)(uintptr_t)3002);
    CHECK(vk_last_image.format == VK_FORMAT_R8G8B8A8_SRGB && vk_last_image.extent.width == 1200 &&
          vk_last_image.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED && vk_last_image.arrayLayers == 1);
    CHECK((vk_last_image.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
          (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    CHECK(vk_last_memory_type == 1 && vk_flip_tracked == 1 &&
          (vk_flip_info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT));
    int i = pool_find(panel);
    uint32_t n, index;
    XrSwapchainImageVulkanKHR image = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    CHECK(pool_enumerate(i, 1, &n, (void *)&image) == XR_SUCCESS && n == 1 && image.image == pool_images[i].vk_image);
    emul_gles_image wrong = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
    CHECK(pool_enumerate(i, 1, &n, (void *)&wrong) == XR_ERROR_VALIDATION_FAILURE);
    // First acquire moves the image into COLOR_ATTACHMENT_OPTIMAL, once.
    CHECK(*vk_layout(image.image) == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(pool_cycle(i, 0, &index) == XR_SUCCESS && index == 0 && vk_submits == 1);
    CHECK(*vk_layout(image.image) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(pool_cycle(i, 1, NULL) == XR_SUCCESS && pool_cycle(i, 2, NULL) == XR_SUCCESS);
    // Not released yet in this frame: the layer is omitted, nothing recorded.
    int before_acquires = acquires, before_releases = releases;
    CHECK(pool_cycle(i, 0, &index) == XR_SUCCESS && vk_submits == 1);
    XrSwapchainSubImage src = {panel, {{10, 5}, {100, 50}}, 0}, sub = src;
    pool_begin_frame_locked();
    CHECK(!pool_subimage_locked(&sub, 0));
    CHECK(pool_finish_locked() && vk_submits == 1);
    CHECK(pool_cycle(i, 1, NULL) == XR_SUCCESS && pool_cycle(i, 2, NULL) == XR_SUCCESS);
    // A frame: one atlas acquire/wait/release, one copy per image however often it is referenced, one submit.
    XrCompositionLayerQuad q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.subImage = src;
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    views[0].subImage.swapchain = views[1].subImage.swapchain = (XrSwapchain)(uintptr_t)700;  // runtime eye set
    views[1].subImage.imageArrayIndex = 1;
    XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount = 2;
    projection.views = views;
    const XrCompositionLayerBaseHeader *layers[] = {(void *)&projection, (void *)&q, (void *)&q};
    PoolFrame frame;
    before_acquires = acquires;
    before_releases = releases;
    int before_copies = vk_copies;
    CHECK(pool_rewrite_layers(layers, 3, &frame) == 3);
    CHECK(vk_copies == before_copies + 1 && vk_submits == 2 && !vk_bad_layouts && !vk_recording);
    CHECK(acquires == before_acquires + 1 && releases == before_releases + 1);
    PoolRect r = pool_images[i].rect;
    CHECK(vk_last_copy.dstOffset.x == r.x && vk_last_copy.dstOffset.y == r.y && vk_last_copy.extent.width == 1200 &&
          vk_last_copy.extent.height == 1020 && vk_last_copy.srcOffset.x == 0);
    CHECK(vk_last_dst == pool_atlas[0].vk_images[pool_atlas[0].index]);
    CHECK(*vk_layout(vk_last_dst) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
          *vk_layout(image.image) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(frame.quads[1].subImage.swapchain == pool_atlas[0].handle &&
          frame.quads[1].subImage.imageRect.offset.x == r.x + 10 && frame.quads[2].subImage.imageRect.offset.y == r.y + 5);
    CHECK(layers[0] == (void *)&frame.projections[0] && frame.views[0][1].subImage.swapchain == (XrSwapchain)700 &&
          frame.views[0][1].subImage.imageArrayIndex == 1);
    CHECK(q.subImage.swapchain == panel);  // caller's layers stay untouched
    // The ring reuses a command buffer only after its fence: no waits for the first frames, then one per frame.
    int before_waits = vk_fence_waits;
    for (int f = 0; f < 4; ++f) {
        const XrCompositionLayerBaseHeader *again[] = {(void *)&q};
        CHECK(pool_rewrite_layers(again, 1, &frame) == 1);
    }
    CHECK(vk_submits == 6 && vk_fence_waits > before_waits && vk_fence_waits <= before_waits + 4 && !vk_bad_layouts);
    // A failed submit drops the frame's layers (like a failed GL blit) but still releases the atlas image.
    vk_fail_submit = 1;
    before_releases = releases;
    {
        const XrCompositionLayerBaseHeader *again[] = {(void *)&q};
        CHECK(pool_rewrite_layers(again, 1, &frame) == 0 && releases == before_releases + 1 && !vk_recording);
    }
    vk_fail_submit = 0;
    // Vertical flips: a flagged pooled quad is blitted upside down (same atlas area, rect mirrored), a flagged runtime
    // quad gets a per-frame atlas area with its rectangle upside down, shared by both eyes; caller layers untouched.
    {
        XrCompositionLayerImageLayoutFB layout = {(XrStructureType)1000040000, NULL, 1};
        XrCompositionLayerQuad fq = q, rq = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        fq.next = &layout;
        rq.next = &layout;
        rq.subImage = (XrSwapchainSubImage){(XrSwapchain)6000, {{24, 16}, {1000, 200}}, 0};
        const XrCompositionLayerBaseHeader *fl[] = {(void *)&fq, (void *)&rq, (void *)&rq};
        int blits0 = vk_blits, copies0 = vk_copies;
        pool_flips = 1;
        CHECK(pool_rewrite_layers(fl, 3, &frame) == 3 && !vk_bad_layouts && !vk_recording);
        CHECK(vk_blits == blits0 + 2 && vk_copies == copies0 && dump_records == 2);
        // pooled: whole image reversed into its rect; quad rect 10,5 100x50 -> y = 1020 - 5 - 50
        CHECK(frame.quads[0].subImage.swapchain == pool_atlas[0].handle &&
              frame.quads[0].subImage.imageRect.offset.x == r.x + 10 &&
              frame.quads[0].subImage.imageRect.offset.y == r.y + 1020 - 55);
        // runtime: atlas area outside the pooled rect, rows reversed, source rect as submitted
        XrRect2Di m = frame.quads[1].subImage.imageRect;
        CHECK(frame.quads[1].subImage.swapchain == pool_atlas[0].handle && m.extent.width == 1000 &&
              m.extent.height == 200 && !memcmp(&frame.quads[2].subImage, &frame.quads[1].subImage, sizeof(XrSwapchainSubImage)));
        CHECK(!(m.offset.x < r.x + r.w && m.offset.x + 1000 > r.x && m.offset.y < r.y + r.h && m.offset.y + 200 > r.y));
        CHECK(vk_last_blit_src == (VkImage)4000 && vk_last_blit.srcOffsets[0].y == 16 &&
              vk_last_blit.srcOffsets[1].y == 216 && vk_last_blit.dstOffsets[0].y == m.offset.y + 200 &&
              vk_last_blit.dstOffsets[1].y == m.offset.y && *vk_layout((VkImage)4000) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK(rq.subImage.swapchain == (XrSwapchain)6000 && fq.subImage.swapchain == panel);
        // every frame again (late drawing shows up), never in place: the source is only read
        fl[0] = (void *)&fq;  // (the call rewrites the array it is given)
        fl[1] = fl[2] = (void *)&rq;
        CHECK(pool_rewrite_layers(fl, 3, &frame) == 3 && vk_blits == blits0 + 4 && !vk_bad_layouts);
        // an undrawn/unknown source stays as it is
        flip_source_ok = 0;
        fl[0] = (void *)&fq;
        fl[1] = fl[2] = (void *)&rq;
        CHECK(pool_rewrite_layers(fl, 3, &frame) == 3 && frame.quads[1].subImage.swapchain == (XrSwapchain)6000);
        flip_source_ok = 1;
        pool_flips = 0;
    }
    // Destroy waits for in-flight copies, frees the image and its memory and the flip tracking.
    int images = vk_images_alive, memory = vk_memory_alive;
    pool_destroy(i);
    CHECK(vk_images_alive == images - 1 && vk_memory_alive == memory - 1 && vk_flip_tracked == 0 && pool_find(panel) < 0);
    pool_cleanup(pool_session);
    CHECK(!pool_session && !pool_vulkan && !pvk.state && vk_fences_alive == 0 && vk_pools_alive == 0);
    CHECK(vk_images_alive == 0 && vk_memory_alive == 0);
    // AC Nexus: without the pool its 15 swapchains fill the cap and the Quest 3 eye set is refused (#424);
    // pooled panels need at most two atlas slots, so the new set fits.
    pool_on_create_session((XrSession)6, &sci);
    int pooled = 0, live = vk_session_counts(&pooled);
    CHECK(live > 0 && live <= 14 && pooled >= 5);
    pool_cleanup(pool_session);
    CHECK(vk_images_alive == 0 && vk_memory_alive == 0 && vk_fences_alive == 0);
    return vk_device_sequence();
}
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
    pool_begin_frame_locked();
    CHECK(pool_subimage_locked(&sub, 0));
    CHECK(sub.swapchain == pool_atlas[0].handle && sub.imageRect.offset.x == pool_images[j].rect.x + 10);
    CHECK(sub.imageRect.offset.y == 5 && sub.imageRect.extent.width == 100);
    CHECK(read_fb == 31 && draw_fb == 32 && scissor == 1 && texture == 33);
    sub = src;
    CHECK(pool_subimage_locked(&sub, 0) && blits == 1); // two eyes/strips copy source once
    CHECK(acquires == 1 && waits == 1 && releases == 0 && pool_finish_locked() && releases == 1);
    pool_begin_frame_locked();
    sub = src;
    CHECK(pool_subimage_locked(&sub, 0) && blits == 2 && acquires == 2);
    CHECK(pool_finish_locked());
    // Failed wait keeps ownership, retries the same acquired image, never releases before wait.
    pool_begin_frame_locked();
    fail_wait = 1;
    sub = src;
    CHECK(!pool_subimage_locked(&sub, 0));
    CHECK(pool_finish_locked());
    CHECK(acquires == 3 && releases == 2);
    fail_wait = 0;
    CHECK(pool_subimage_locked(&sub, 0) && acquires == 3);
    CHECK(pool_finish_locked());
    pool_begin_frame_locked();
    bad_gl = 1;
    sub = src;
    CHECK(!pool_subimage_locked(&sub, 0));
    CHECK(pool_finish_locked());
    bad_gl = 0;
    CHECK(read_fb == 31 && draw_fb == 32 && scissor == 1);
    pool_begin_frame_locked();
    sub = src;
    CHECK(pool_subimage_locked(&sub, 0));
    fail_release = 1;
    CHECK(!pool_finish_locked());
    fail_release = 0;
    CHECK(pool_finish_locked());
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
    pool_begin_frame_locked();
    CHECK(pool_subimage_locked(&sub, 0) && sub.imageRect.extent.width == 1024);
    CHECK(pool_finish_locked());
    sub = src;
    sub.imageRect.offset.x = 2000;
    pool_begin_frame_locked();
    CHECK(!pool_subimage_locked(&sub, 0));
    CHECK(pool_finish_locked());
    sub = src;
    sub.imageRect.offset.x = -10;
    pool_begin_frame_locked();
    CHECK(pool_subimage_locked(&sub, 0) && sub.imageRect.offset.x == pool_images[j].rect.x && sub.imageRect.extent.width == 90);
    CHECK(pool_finish_locked());
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
    if (vk_main()) return 1;
    puts("ok");
    return 0;
}
