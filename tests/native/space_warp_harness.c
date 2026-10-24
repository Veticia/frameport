// Host doubles (runtime, GL, Vulkan) for FrameBridge's XR_FB_space_warp emulation (native/adapter/space_warp.c).
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static int logs;
#define LOG(...) ((void)++logs)
static uintptr_t fake = 0x7f000000;
#define FAKE_HANDLE(t) ((t)(++fake))
static XrInstance active_instance = (XrInstance)(uintptr_t)1;
static int space_warp_emul = 1, gles_session = 1;
static uint32_t runtime_eye_width, runtime_eye_height;
typedef struct { XrStructureType type; void *next; uint32_t image; } emul_gles_image;

// ---- runtime doubles
static uint32_t runtime_mv;            // what the runtime answers in XrSystemSpaceWarpPropertiesFB
static int runtime_refuses_sw;          // xrGetSystemProperties fails while the struct is chained
static XrResult runtime_properties(XrInstance i, XrSystemId s, XrSystemProperties *p) {
    (void)i; (void)s;
    for (XrBaseOutStructure *n = p->next; n; n = n->next)
        if (n->type == XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB) {
            if (runtime_refuses_sw) return XR_ERROR_VALIDATION_FAILURE;
            ((XrSystemSpaceWarpPropertiesFB *)n)->recommendedMotionVectorImageRectWidth = runtime_mv;
            ((XrSystemSpaceWarpPropertiesFB *)n)->recommendedMotionVectorImageRectHeight = runtime_mv;
        }
    return XR_SUCCESS;
}
static XrResult runtime_views(XrInstance i, XrSystemId s, XrViewConfigurationType t, uint32_t cap, uint32_t *n,
                              XrViewConfigurationView *v) {
    (void)i; (void)s; (void)t;
    *n = 2;
    for (uint32_t k = 0; k < cap && k < 2; ++k) v[k].recommendedImageRectWidth = v[k].recommendedImageRectHeight = 1728;
    return XR_SUCCESS;
}
static int64_t runtime_formats_list[] = {43, 50, 129};  // Vulkan: sRGB colour, D24S8 already there
static XrResult runtime_formats(XrSession s, uint32_t cap, uint32_t *n, int64_t *f) {
    (void)s;
    *n = 3;
    if (cap) { if (cap < 3) return XR_ERROR_SIZE_INSUFFICIENT; memcpy(f, runtime_formats_list, sizeof runtime_formats_list); }
    return XR_SUCCESS;
}
static PFN_xrVoidFunction lookup(XrInstance instance, const char *name) {
    (void)instance;
    if (!strcmp(name, "xrEnumerateViewConfigurationViews")) return (PFN_xrVoidFunction)runtime_views;
    if (!strcmp(name, "xrEnumerateSwapchainFormats")) return (PFN_xrVoidFunction)runtime_formats;
    return NULL;
}

// ---- GL doubles
static GLuint next_texture = 500;
static int fail_gl, textures_live, storage3d_calls, bound = 7;
static EGLContext current(void) { return (EGLContext)1; }
static EGLContext (*p_eglGetCurrentContext)(void) = current;
static void (*p_eglCreateContext)(void);
static int emul_load_gl(void) { return 1; }
static void p_glGetIntegerv(GLenum p, GLint *v) { (void)p; *v = bound; }
static void p_glGenTextures(GLsizei n, GLuint *v) { while (n--) { *v++ = next_texture++; ++textures_live; } }
static void p_glBindTexture(GLenum target, GLuint t) { (void)target; bound = (GLint)t; }
static void p_glTexStorage2D(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h) { (void)t; (void)l; (void)f; (void)w; (void)h; }
static void stub_storage3d(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLsizei d) {
    (void)t; (void)l; (void)f; (void)w; (void)h; (void)d; ++storage3d_calls;
}
static GLenum p_glGetError(void) { return fail_gl ? GL_INVALID_ENUM : GL_NO_ERROR; }
static void p_glDeleteTextures(GLsizei n, const GLuint *v) { (void)v; textures_live -= n; }

// ---- Vulkan doubles (the session binding flip_vk.c keeps)
static int images_live, memory_live, barriers, submits, vk_usage_seen, vk_flags_seen;
static VkResult create_image(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void)d; (void)a;
    vk_usage_seen = (int)ci->usage;
    vk_flags_seen = (int)ci->flags;
    *out = (VkImage)(uintptr_t)(9000 + images_live++);
    return VK_SUCCESS;
}
static void destroy_image(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d; (void)i; (void)a; --images_live; }
static void requirements(VkDevice d, VkImage i, VkMemoryRequirements *r) { (void)d; (void)i; r->size = 4096; r->memoryTypeBits = 3; }
static VkResult allocate(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *m) {
    (void)d; (void)a;
    if (ai->memoryTypeIndex != 1) return VK_ERROR_OUT_OF_DEVICE_MEMORY;  // type 1 is the device-local one
    *m = (VkDeviceMemory)(uintptr_t)(7000 + memory_live++);
    return VK_SUCCESS;
}
static void free_memory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)m; (void)a; --memory_live; }
static VkResult bind(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)i; (void)m; (void)o; return VK_SUCCESS; }
static VkResult create_pool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *p) {
    (void)d; (void)ci; (void)a; *p = (VkCommandPool)(uintptr_t)55; return VK_SUCCESS;
}
static void destroy_pool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)p; (void)a; }
static VkResult alloc_cmd(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *c) {
    (void)d; (void)ai; *c = (VkCommandBuffer)(uintptr_t)66; return VK_SUCCESS;
}
static VkResult begin_cmd(VkCommandBuffer c, const VkCommandBufferBeginInfo *b) { (void)c; (void)b; return VK_SUCCESS; }
static VkResult end_cmd(VkCommandBuffer c) { (void)c; return VK_SUCCESS; }
static void pipeline_barrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f,
                             uint32_t mc, const VkMemoryBarrier *m, uint32_t bc, const VkBufferMemoryBarrier *b,
                             uint32_t ic, const VkImageMemoryBarrier *i) {
    (void)c; (void)s; (void)d; (void)f; (void)mc; (void)m; (void)bc; (void)b;
    for (uint32_t k = 0; k < ic; ++k)
        if (i[k].newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
            i[k].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ++barriers;
}
static VkResult submit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) { (void)q; (void)n; (void)s; (void)f; ++submits; return VK_SUCCESS; }
static VkResult idle(VkQueue q) { (void)q; return VK_SUCCESS; }
static PFN_vkVoidFunction device_proc(VkDevice d, const char *name) {
    (void)d;
    if (!strcmp(name, "vkDestroyCommandPool")) return (PFN_vkVoidFunction)destroy_pool;
    if (!strcmp(name, "vkQueueWaitIdle")) return (PFN_vkVoidFunction)idle;
    return NULL;
}
static struct {
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkQueueSubmit QueueSubmit;
    VkPhysicalDeviceMemoryProperties memory;
} vk;
static pthread_mutex_t flip_lock = PTHREAD_MUTEX_INITIALIZER;
static int vk_ready(void) { return vk.device != VK_NULL_HANDLE; }

#include "space_warp.c"

static XrCompositionLayerDepthInfoKHR depth_info(XrSwapchain s, const void *next) {
    XrCompositionLayerDepthInfoKHR d = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, next};
    d.subImage.swapchain = s;
    return d;
}

int main(void) {
    sw_glTexStorage3D = stub_storage3d;  // the host has no libGLESv3.so to dlopen
    // ---- properties: the runtime's zero answer becomes a quarter of the eye; its own size is kept
    XrSystemSpaceWarpPropertiesFB swp = {XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
    XrSystemProperties props = {XR_TYPE_SYSTEM_PROPERTIES, &swp};
    CHECK(runtime_properties(0, 1, &props) == XR_SUCCESS);
    sw_properties(active_instance, 1, &props);
    CHECK(swp.recommendedMotionVectorImageRectWidth == 432 && swp.recommendedMotionVectorImageRectHeight == 432);
    CHECK(sw_mv_width == 432 && sw_mv_height == 432);
    CHECK(runtime_eye_width == 1728);  // asked the runtime for the eye size
    runtime_mv = 376;  // already small: kept
    runtime_properties(0, 1, &props);
    sw_properties(active_instance, 1, &props);
    CHECK(swp.recommendedMotionVectorImageRectWidth == 376 && sw_mv_width == 376);
    runtime_mv = 1728;  // the Frame's runtime: the whole eye, replaced by a quarter of it
    runtime_properties(0, 1, &props);
    sw_properties(active_instance, 1, &props);
    CHECK(swp.recommendedMotionVectorImageRectWidth == 432 && sw_mv_width == 432 && sw_mv_height == 432);
    runtime_mv = 0;
    runtime_refuses_sw = 1;  // a runtime that rejects the struct: answered without it, then filled
    XrResult r = runtime_properties(0, 1, &props);
    CHECK(XR_FAILED(r));
    r = sw_properties_retry(runtime_properties, active_instance, 1, &props, r);
    CHECK(r == XR_SUCCESS && props.next == (void *)&swp && swp.next == NULL);
    sw_properties(active_instance, 1, &props);
    CHECK(swp.recommendedMotionVectorImageRectWidth == 432 && sw_mv_width == 432);
    runtime_refuses_sw = 0;
    space_warp_emul = 0;  // setting off: the runtime's answer stays, nothing is served
    swp.recommendedMotionVectorImageRectWidth = 0;
    runtime_properties(0, 1, &props);
    sw_properties(active_instance, 1, &props);
    CHECK(swp.recommendedMotionVectorImageRectWidth == 0);
    space_warp_emul = 1;

    // ---- which swapchains are served: motion vectors and their depth at the reported size only
    XrSwapchainCreateInfo mv = {XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL, 0,
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, 0x881A, 1, 432, 432, 1, 2, 1};
    XrSwapchainCreateInfo depth = mv;
    depth.format = 0x88F0;
    depth.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchainCreateInfo eye = mv;
    eye.width = eye.height = 1728;
    eye.format = 0x8C43;
    XrSwapchainCreateInfo eye_depth = depth;
    eye_depth.width = eye_depth.height = 1728;
    XrSwapchainCreateInfo srgb_small = eye;
    srgb_small.width = srgb_small.height = 432;  // a colour panel that happens to have the size: not served
    XrSession s1 = (XrSession)1;
    CHECK(sw_wanted(s1, &mv) && sw_wanted(s1, &depth));
    CHECK(!sw_wanted(s1, &eye) && !sw_wanted(s1, &eye_depth) && !sw_wanted(s1, &srgb_small));
    // OVRPlugin/Unreal size their motion vectors themselves (376x376 on the Frame): small motion-vector formats are
    // served, their depth once a served motion-vector swapchain of that size exists in the session
    XrSwapchainCreateInfo mv376 = mv, depth376 = depth, big_mv = mv;
    mv376.width = mv376.height = depth376.width = depth376.height = 376;
    big_mv.width = big_mv.height = 1000;  // more than half the eye: an eye buffer in a float format, not served
    CHECK(sw_wanted(s1, &mv376) && !sw_wanted(s1, &depth376) && !sw_wanted(s1, &big_mv));
    XrSwapchain h376 = 0, d376 = 0;
    CHECK(sw_create(s1, &mv376, &h376) && h376);
    CHECK(sw_wanted(s1, &depth376) && !sw_wanted((XrSession)9, &depth376));
    CHECK(sw_create(s1, &depth376, &d376) && d376);
    sw_destroy(h376);
    sw_destroy(d376);
    CHECK(textures_live == 0);
    XrSwapchain h_mv = 0, h_depth = 0, h_none = 0;
    CHECK(!sw_create((XrSession)1, &eye, &h_none) && !h_none);

    // ---- GLES: textures on the app's context, three images, round robin, the app's binding restored
    CHECK(sw_create((XrSession)1, &mv, &h_mv) && h_mv);
    CHECK(sw_create((XrSession)1, &depth, &h_depth) && h_depth && h_depth != h_mv);
    CHECK(storage3d_calls == 12 && textures_live == 6 && bound == 7);
    CHECK(sw_is_fake(h_mv) && sw_is_fake(h_depth) && !sw_is_fake((XrSwapchain)(uintptr_t)42));
    uint32_t n = 0;
    CHECK(sw_enumerate(h_mv, 0, &n, NULL) == XR_SUCCESS && n == 3);
    emul_gles_image gl[3] = {{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR}, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR},
                             {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR}};
    CHECK(sw_enumerate(h_mv, 2, &n, (XrSwapchainImageBaseHeader *)gl) == XR_ERROR_SIZE_INSUFFICIENT);
    CHECK(sw_enumerate(h_mv, 3, &n, (XrSwapchainImageBaseHeader *)gl) == XR_SUCCESS);
    CHECK(gl[0].image && gl[1].image == gl[0].image + 1 && gl[2].image == gl[0].image + 2);
    XrSwapchainImageVulkanKHR wrong[3] = {{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR}};
    CHECK(sw_enumerate(h_mv, 3, &n, (XrSwapchainImageBaseHeader *)wrong) == XR_ERROR_VALIDATION_FAILURE);
    uint32_t index = 99;
    for (uint32_t k = 0; k < 7; ++k) {
        CHECK(sw_acquire(h_mv, &index) == XR_SUCCESS && index == k % 3);
        CHECK(sw_wait(h_mv, NULL) == XR_SUCCESS && sw_release(h_mv, NULL) == XR_SUCCESS);
    }
    CHECK(sw_acquire(h_mv, NULL) == XR_ERROR_VALIDATION_FAILURE);
    CHECK(sw_acquires == 7 && sw_waits == 7 && sw_releases == 7);
    CHECK(sw_wait((XrSwapchain)(uintptr_t)42, NULL) == XR_ERROR_FUNCTION_UNSUPPORTED);  // not ours: next hook

    // ---- xrEndFrame chains: space warp info and depth info on a served image go, eye depth info stays
    XrCompositionLayerSpaceWarpInfoFB warp = {XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB};
    warp.motionVectorSubImage.swapchain = h_mv;
    warp.depthSubImage.swapchain = h_depth;
    XrCompositionLayerDepthInfoKHR eye_d = depth_info((XrSwapchain)(uintptr_t)42, NULL);
    XrCompositionLayerDepthInfoKHR copies[4];
    int changed = 0;
    CHECK(sw_strip_chain(&eye_d, copies, &changed) == &eye_d && !changed);  // nothing to remove
    warp.next = &eye_d;  // [warp, eye depth]: skips the head
    const XrCompositionLayerDepthInfoKHR *out = sw_strip_chain(&warp, copies, &changed);
    CHECK(changed && out == &copies[0] && out->subImage.swapchain == (XrSwapchain)(uintptr_t)42 && !out->next);
    warp.next = NULL;
    XrCompositionLayerDepthInfoKHR eye_first = depth_info((XrSwapchain)(uintptr_t)42, &warp);  // [eye depth, warp]
    changed = 0;
    out = sw_strip_chain(&eye_first, copies, &changed);
    CHECK(changed && out == &copies[0] && out->subImage.swapchain == (XrSwapchain)(uintptr_t)42 && !out->next);
    CHECK(eye_first.next == &warp);  // the app's own structs are never modified
    XrBaseInStructure unknown = {(XrStructureType)1000999000, NULL};
    warp.next = &unknown;  // [warp, unknown]: the unknown tail is kept as it is
    changed = 0;
    CHECK(sw_strip_chain(&warp, copies, &changed) == (const void *)&unknown && changed);
    unknown.next = (const XrBaseInStructure *)&warp;  // [unknown, warp]: can't skip inside it, the chain goes
    warp.next = NULL;
    changed = 0;
    CHECK(sw_strip_chain(&unknown, copies, &changed) == NULL && changed);
    XrCompositionLayerDepthInfoKHR fake_d = depth_info(h_depth, NULL);  // depth info on a served image goes too
    changed = 0;
    CHECK(sw_strip_chain(&fake_d, copies, &changed) == NULL && changed);

    // ---- formats: what space warp needs is appended after the runtime's own list (none of it twice)
    gles_session = 0;
    CHECK(sw_enumerate_formats((XrSession)1, 0, &n, NULL) == XR_SUCCESS && n == 7);
    int64_t formats[8] = {0};
    CHECK(sw_enumerate_formats((XrSession)1, 6, &n, formats) == XR_ERROR_SIZE_INSUFFICIENT);
    CHECK(sw_enumerate_formats((XrSession)1, 8, &n, formats) == XR_SUCCESS && n == 7);
    CHECK(formats[0] == 43 && formats[1] == 50 && formats[2] == 129 && formats[3] == 97 && formats[6] == 124);
    gles_session = 1;
    CHECK(sw_enumerate_formats((XrSession)1, 8, &n, formats) == XR_SUCCESS && n == 8 && formats[3] == 0x881A);

    // ---- GL failure: the swapchain goes to the runtime, nothing leaks
    XrSwapchainCreateInfo mv2 = mv;
    mv2.arraySize = 1;
    fail_gl = 1;
    int before = textures_live;
    CHECK(!sw_create((XrSession)1, &mv2, &h_none) && textures_live == before);
    fail_gl = 0;

    // ---- destroy / session end
    sw_destroy(h_mv);
    CHECK(!sw_is_fake(h_mv) && textures_live == 3);
    sw_session_destroy((XrSession)1);
    CHECK(!sw_is_fake(h_depth) && textures_live == 0);

    // ---- Vulkan: device-local images in attachment layout, META extra usage, freed on destroy
    gles_session = 0;
    vk.device = (VkDevice)(uintptr_t)1;
    vk.GetDeviceProcAddr = device_proc;
    vk.CreateImage = create_image;
    vk.DestroyImage = destroy_image;
    vk.GetImageMemoryRequirements = requirements;
    vk.AllocateMemory = allocate;
    vk.FreeMemory = free_memory;
    vk.BindImageMemory = bind;
    vk.CreateCommandPool = create_pool;
    vk.AllocateCommandBuffers = alloc_cmd;
    vk.BeginCommandBuffer = begin_cmd;
    vk.EndCommandBuffer = end_cmd;
    vk.CmdPipelineBarrier = pipeline_barrier;
    vk.QueueSubmit = submit;
    vk.memory.memoryTypeCount = 2;
    vk.memory.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    XrVulkanSwapchainCreateInfoMETA meta = {XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META, NULL, 0,
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    XrSwapchainCreateInfo vmv = mv;
    vmv.format = 97;
    vmv.next = &meta;
    XrSwapchainCreateInfo vdepth = depth;
    vdepth.format = 129;
    XrSwapchain v_mv = 0, v_depth = 0;
    CHECK(sw_create((XrSession)2, &vmv, &v_mv) && v_mv);
    CHECK(vk_usage_seen == (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    CHECK(sw_create((XrSession)2, &vdepth, &v_depth) && v_depth);
    CHECK(vk_usage_seen == (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
    CHECK(images_live == 6 && memory_live == 6 && barriers == 6 && submits == 2);
    XrSwapchainImageVulkanKHR vimg[3] = {{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR}, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR},
                                         {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR}};
    CHECK(sw_enumerate(v_mv, 3, &n, (XrSwapchainImageBaseHeader *)vimg) == XR_SUCCESS && vimg[0].image && vimg[2].image);
    sw_session_destroy((XrSession)2);
    CHECK(images_live == 0 && memory_live == 0 && !sw_is_fake(v_mv));
    vk.device = VK_NULL_HANDLE;  // no Vulkan binding: not served
    CHECK(!sw_create((XrSession)2, &vmv, &v_mv) || 0);

    space_warp_emul = 0;  // off: nothing is ever served
    gles_session = 1;
    CHECK(!sw_wanted((XrSession)1, &mv) && !sw_create((XrSession)1, &mv, &h_none));
    puts("ok");
    return 0;
}
