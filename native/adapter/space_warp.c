// SPDX-License-Identifier: GPL-3.0-only
// Included by frame_adapter.c (after layer_emul_gl.c and cube_standin.c, whose GL entry points, Vulkan session and
// fake handles it shares). space_warp_emul (per game, off by default): XR_FB_space_warp for games that refuse to run
// without Application SpaceWarp (OutRun Quest VR, GitHub #336: "AppSW recommended buffers unavailable").
//
// The game sees a complete XR_FB_space_warp:
//  * the extension is offered and enabled (passed on when the runtime has it, else emulated);
//  * XrSystemSpaceWarpPropertiesFB carries a motion-vector size: a quarter of the runtime's recommended eye size (the
//    ratio Meta's runtime uses). The Frame's runtime answers with the full eye size (1728x1728), which is kept only
//    when it is already below half the eye; 0 (a runtime without the extension) is replaced too;
//  * its motion-vector swapchains (RGBA16F/RG16F at the reported size, or at most half the eye size: OVRPlugin/Unreal
//    pick their own 376x376) and their depth swapchains (depth usage at the reported size or at the size of a served
//    motion-vector swapchain) are served by FrameBridge as ordinary GL textures / Vulkan images (three per swapchain,
//    cycled like a runtime's): the Frame's runtime only takes sRGB colour formats from GLES apps, and space warp must
//    not reach its compositor anyway (UE5/Unity space warp flickered on the Frame: Into The Radius 2, Metro Awakening);
//  * xrEnumerateSwapchainFormats also lists RGBA16F and the usual depth formats (after the runtime's own), for apps
//    that check the list before creating these swapchains;
//  * xrEndFrame removes XrCompositionLayerSpaceWarpInfoFB (and any depth info that shows one of these images) from the
//    projection views, so the runtime composites a normal frame. The game keeps rendering every frame: only Meta's
//    runtime halves an app's frame rate for space warp; SteamVR's xrWaitFrame doesn't.
// The motion vectors are rendered and never used: a few percent of GPU time at a sixteenth of the eye's pixels.
#define SW_MAX 16
#define SW_IMAGES 3
typedef struct {
    XrSwapchain handle;
    XrSession session;
    XrSwapchainCreateInfo info;
    uint32_t next;             // next image to hand out (round robin, as runtimes do)
    GLuint tex[SW_IMAGES];
    VkDevice device;
    VkImage image[SW_IMAGES];
    VkDeviceMemory memory[SW_IMAGES];
} sw_swapchain;
static sw_swapchain sw_chains[SW_MAX];
static pthread_mutex_t sw_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t sw_mv_width, sw_mv_height;  // what the app was told (0: never asked)
static void (*sw_glTexStorage3D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);

static int sw_find(XrSwapchain handle) {
    if (!handle) return -1;
    int slot = -1;
    pthread_mutex_lock(&sw_lock);
    for (int i = 0; i < SW_MAX && slot < 0; ++i)
        if (sw_chains[i].handle == handle) slot = i;
    pthread_mutex_unlock(&sw_lock);
    return slot;
}

static int sw_is_fake(XrSwapchain handle) { return space_warp_emul && sw_find(handle) >= 0; }

// Wait/release of a served image: nothing to do (counted for the log).
static int sw_acquires, sw_waits, sw_releases;
static int sw_served_wait(XrSwapchain handle) {
    if (!sw_is_fake(handle)) return 0;
    __atomic_add_fetch(&sw_waits, 1, __ATOMIC_RELAXED);
    return 1;
}
static int sw_served_release(XrSwapchain handle) {
    if (!sw_is_fake(handle)) return 0;
    __atomic_add_fetch(&sw_releases, 1, __ATOMIC_RELAXED);
    return 1;
}

// GL_RGBA16F / GL_RG16F / VK_FORMAT_R16G16B16A16_SFLOAT / VK_FORMAT_R16G16_SFLOAT: what XR_FB_space_warp's motion
// vectors use (Meta recommends RGBA16F).
static int sw_motion_format(int64_t f) { return f == 0x881A || f == 0x822F || f == 97 || f == 83; }

static int sw_vk_depth_format(int64_t f) { return f >= 124 && f <= 130; }  // VK_FORMAT_D16_UNORM .. D32_SFLOAT_S8_UINT

// The runtime's recommended eye size (xrEnumerateViewConfigurationViews), 1728 until known.
static uint32_t sw_eye_width(void) { return runtime_eye_width ? runtime_eye_width : 1728; }
static uint32_t sw_eye_height(void) { return runtime_eye_height ? runtime_eye_height : 1728; }

// A space-warp swapchain: motion vectors (a motion-vector format at the reported size or at most half the eye size)
// or their depth (depth usage at the reported size or at the size of a motion-vector swapchain served here).
static int sw_wanted(XrSession session, const XrSwapchainCreateInfo *info) {
    if (!space_warp_emul || !info || info->faceCount != 1 || !info->width || !info->height) return 0;
    int reported = sw_mv_width && info->width == sw_mv_width && info->height == sw_mv_height;
    if (sw_motion_format(info->format) && !(info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        return reported || (info->width * 2 <= sw_eye_width() && info->height * 2 <= sw_eye_height());
    if (!(info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return 0;
    if (reported) return 1;
    int paired = 0;
    pthread_mutex_lock(&sw_lock);
    for (int i = 0; i < SW_MAX && !paired; ++i)
        paired = sw_chains[i].handle && sw_chains[i].session == session && sw_motion_format(sw_chains[i].info.format) &&
                 sw_chains[i].info.width == info->width && sw_chains[i].info.height == info->height;
    pthread_mutex_unlock(&sw_lock);
    return paired;
}

// Fills XrSystemSpaceWarpPropertiesFB (xrGetSystemProperties): logs the runtime's answer once; with the setting the
// app gets a quarter of the eye size unless the runtime's answer is already smaller than half the eye.
static void sw_properties(XrInstance instance, XrSystemId system, XrSystemProperties *properties) {
    if (!properties) return;
    for (XrBaseOutStructure *p = (XrBaseOutStructure *)properties->next; p; p = p->next) {
        if (p->type != XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB) continue;
        XrSystemSpaceWarpPropertiesFB *sw = (XrSystemSpaceWarpPropertiesFB *)p;
        static int logged;
        if (!logged++)
            LOG("space warp: runtime motion-vector size %ux%u%s", sw->recommendedMotionVectorImageRectWidth,
                sw->recommendedMotionVectorImageRectHeight, space_warp_emul ? "" : " (space_warp_emul off)");
        if (!space_warp_emul) continue;
        if (!runtime_eye_width) {  // the app may ask before it enumerates its views
            PFN_xrEnumerateViewConfigurationViews views =
                (PFN_xrEnumerateViewConfigurationViews)lookup(instance, "xrEnumerateViewConfigurationViews");
            XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
            uint32_t n = 0;
            if (views && XR_SUCCEEDED(views(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, v)) && n &&
                v[0].recommendedImageRectWidth >= 64 && v[0].recommendedImageRectHeight >= 64) {
                runtime_eye_width = v[0].recommendedImageRectWidth;
                runtime_eye_height = v[0].recommendedImageRectHeight;
            }
        }
        uint32_t w = sw->recommendedMotionVectorImageRectWidth, h = sw->recommendedMotionVectorImageRectHeight;
        if (!w || !h || w * 2 > sw_eye_width() || h * 2 > sw_eye_height()) {
            w = (sw_eye_width() + 3) / 4;
            h = (sw_eye_height() + 3) / 4;
        }
        sw->recommendedMotionVectorImageRectWidth = w;
        sw->recommendedMotionVectorImageRectHeight = h;
        if (sw_mv_width != w || sw_mv_height != h) LOG("space_warp_emul: motion-vector size %ux%u reported to the app", w, h);
        sw_mv_width = w;
        sw_mv_height = h;
    }
}

// The runtime refused the properties call (it may not know the struct when it lacks the extension): retried without
// XrSystemSpaceWarpPropertiesFB in the chain, which is then filled here.
static XrResult sw_properties_retry(PFN_xrGetSystemProperties fn, XrInstance instance, XrSystemId system,
                                    XrSystemProperties *properties, XrResult result) {
    if (!space_warp_emul || XR_SUCCEEDED(result) || !properties) return result;
    XrBaseOutStructure *prev = (XrBaseOutStructure *)properties, *sw = NULL;
    for (XrBaseOutStructure *p = properties->next; p; prev = p, p = p->next)
        if (p->type == XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB) { sw = p; break; }
    if (!sw) return result;
    prev->next = sw->next;
    XrResult again = fn(instance, system, properties);
    prev->next = sw;
    ((XrSystemSpaceWarpPropertiesFB *)sw)->recommendedMotionVectorImageRectWidth = 0;
    ((XrSystemSpaceWarpPropertiesFB *)sw)->recommendedMotionVectorImageRectHeight = 0;
    LOG("space_warp_emul: runtime refused the space warp properties (result=%d); without them: %d", result, again);
    return again;
}

static void sw_vk_free(sw_swapchain *s) {
    for (int i = 0; i < SW_IMAGES; ++i) {
        if (s->image[i]) vk.DestroyImage(s->device, s->image[i], NULL);
        if (s->memory[i]) vk.FreeMemory(s->device, s->memory[i], NULL);
        s->image[i] = VK_NULL_HANDLE;
        s->memory[i] = VK_NULL_HANDLE;
    }
}

// Vulkan images in the layout a runtime hands out (colour/depth attachment optimal), made on the app's queue.
static int sw_vk_create(const XrSwapchainCreateInfo *info, sw_swapchain *s) {
    if (!info->mipCount || !info->arraySize || (info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) return 0;
    int ok = 0, depth = sw_vk_depth_format(info->format);
    VkCommandPool pool = VK_NULL_HANDLE;
    pthread_mutex_lock(&flip_lock);
    if (!vk_ready()) goto done;
    PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)vk.GetDeviceProcAddr(vk.device, "vkDestroyCommandPool");
    PFN_vkQueueWaitIdle wait_idle = (PFN_vkQueueWaitIdle)vk.GetDeviceProcAddr(vk.device, "vkQueueWaitIdle");
    if (!destroy_pool || !wait_idle) goto done;
    s->device = vk.device;
    VkImageUsageFlags usage = 0;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_MND) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkImageCreateFlags flags = (info->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    for (const XrBaseInStructure *n = (const XrBaseInStructure *)info->next; n; n = n->next)
        if (n->type == XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META) {  // XR_META_vulkan_swapchain_create_info
            usage |= ((const XrVulkanSwapchainCreateInfoMETA *)n)->additionalUsageFlags;
            flags |= ((const XrVulkanSwapchainCreateInfoMETA *)n)->additionalCreateFlags;
        }
    if (!usage) usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkSampleCountFlagBits samples = info->sampleCount >= 1 && info->sampleCount <= 64 ? (VkSampleCountFlagBits)info->sampleCount
                                                                                       : VK_SAMPLE_COUNT_1_BIT;
    for (int i = 0; i < SW_IMAGES; ++i) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, flags, VK_IMAGE_TYPE_2D, (VkFormat)info->format,
            {info->width, info->height, 1}, info->mipCount, info->arraySize, samples, VK_IMAGE_TILING_OPTIMAL, usage,
            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        if (vk.CreateImage(s->device, &ci, NULL, &s->image[i]) != VK_SUCCESS) goto done;
        VkMemoryRequirements req;
        vk.GetImageMemoryRequirements(s->device, s->image[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < vk.memory.memoryTypeCount && type == UINT32_MAX; ++t)
            if ((req.memoryTypeBits & (1u << t)) && (vk.memory.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = t;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
        if (type == UINT32_MAX || vk.AllocateMemory(s->device, &ai, NULL, &s->memory[i]) != VK_SUCCESS ||
            vk.BindImageMemory(s->device, s->image[i], s->memory[i], 0) != VK_SUCCESS) goto done;
    }
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, vk.family};
    if (vk.CreateCommandPool(s->device, &pi, NULL, &pool) != VK_SUCCESS) goto done;
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vk.AllocateCommandBuffers(s->device, &alloc, &cmd) != VK_SUCCESS) goto done;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    if (vk.BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) goto done;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (depth) aspect = info->format == 127 ? VK_IMAGE_ASPECT_STENCIL_BIT : info->format >= 128
        ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageLayout layout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAccessFlags access = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                 : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    for (int i = 0; i < SW_IMAGES; ++i) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, access, VK_IMAGE_LAYOUT_UNDEFINED, layout,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, s->image[i],
            {aspect, 0, info->mipCount, 0, info->arraySize}};
        vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    if (vk.EndCommandBuffer(cmd) != VK_SUCCESS) goto done;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    if (vk.QueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) goto done;
    ok = wait_idle(vk.queue) == VK_SUCCESS;
done:
    if (pool) ((PFN_vkDestroyCommandPool)vk.GetDeviceProcAddr(vk.device, "vkDestroyCommandPool"))(s->device, pool, NULL);
    if (!ok && s->device) sw_vk_free(s);
    pthread_mutex_unlock(&flip_lock);
    return ok;
}

// GL textures (2D, or 2D arrays for multiview) with the requested internal format, made on the app's current context.
static int sw_gl_create(const XrSwapchainCreateInfo *info, sw_swapchain *s) {
    if ((!p_eglCreateContext && !emul_load_gl()) || p_eglGetCurrentContext() == EGL_NO_CONTEXT) return 0;
    if (!sw_glTexStorage3D) {
        void *gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        if (gles) sw_glTexStorage3D = (void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei))dlsym(gles, "glTexStorage3D");
    }
    GLenum target = info->arraySize > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    if (target == GL_TEXTURE_2D_ARRAY && !sw_glTexStorage3D) return 0;
    GLsizei levels = info->mipCount > 1 ? (GLsizei)info->mipCount : 1;
    for (int i = 0; i < 8 && p_glGetError() != GL_NO_ERROR; ++i) {}
    GLint previous = 0;
    p_glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_2D_ARRAY, &previous);
    p_glGenTextures(SW_IMAGES, s->tex);
    GLenum err = GL_NO_ERROR;
    for (int i = 0; i < SW_IMAGES && err == GL_NO_ERROR; ++i) {
        p_glBindTexture(target, s->tex[i]);
        if (target == GL_TEXTURE_2D)
            p_glTexStorage2D(GL_TEXTURE_2D, levels, (GLenum)info->format, (GLsizei)info->width, (GLsizei)info->height);
        else
            sw_glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, (GLenum)info->format, (GLsizei)info->width,
                              (GLsizei)info->height, (GLsizei)info->arraySize);
        err = p_glGetError();
    }
    p_glBindTexture(target, (GLuint)previous);  // the app's binding, untouched
    if (err != GL_NO_ERROR) {
        p_glDeleteTextures(SW_IMAGES, s->tex);
        memset(s->tex, 0, sizeof s->tex);
        LOG("space_warp_emul: GL refused a %ux%u format 0x%llx texture (0x%x)", info->width, info->height,
            (unsigned long long)info->format, err);
        return 0;
    }
    return 1;
}

// xrCreateSwapchain: a space-warp swapchain is served here (1), never by the runtime.
static int sw_create(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *out) {
    if (!out || !sw_wanted(session, info)) return 0;
    int slot = -1;
    pthread_mutex_lock(&sw_lock);
    for (int i = 0; i < SW_MAX && slot < 0; ++i)
        if (!sw_chains[i].handle) slot = i;
    if (slot >= 0) sw_chains[slot].handle = (XrSwapchain)1;  // reserved
    pthread_mutex_unlock(&sw_lock);
    if (slot < 0) { LOG("space_warp_emul: too many space warp swapchains, this one goes to the runtime"); return 0; }
    sw_swapchain s = {0};
    s.session = session;
    s.info = *info;
    s.info.next = NULL;
    int ok = gles_session ? sw_gl_create(info, &s) : sw_vk_create(info, &s);
    pthread_mutex_lock(&sw_lock);
    if (ok) {
        s.handle = FAKE_HANDLE(XrSwapchain);
        sw_chains[slot] = s;
        *out = s.handle;
    } else sw_chains[slot].handle = XR_NULL_HANDLE;
    pthread_mutex_unlock(&sw_lock);
    LOG("space_warp_emul: xrCreateSwapchain %ux%u format=%lld samples=%u array=%u usage=0x%llx mips=%u: %s (%s, %d images)",
        info->width, info->height, (long long)info->format, info->sampleCount, info->arraySize,
        (unsigned long long)info->usageFlags, info->mipCount, ok ? "served by FrameBridge" : "FAILED, goes to the runtime",
        gles_session ? "GL textures" : "Vulkan images", SW_IMAGES);
    return ok;
}

static XrResult sw_enumerate(XrSwapchain handle, uint32_t capacity, uint32_t *count, XrSwapchainImageBaseHeader *images) {
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = SW_IMAGES;
    if (!capacity) return XR_SUCCESS;
    if (capacity < SW_IMAGES) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!images) return XR_ERROR_VALIDATION_FAILURE;
    int slot = sw_find(handle);
    if (slot < 0) return XR_ERROR_HANDLE_INVALID;
    pthread_mutex_lock(&sw_lock);
    sw_swapchain *s = &sw_chains[slot];
    XrResult r = XR_SUCCESS;
    if (s->image[0]) {
        if (images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) r = XR_ERROR_VALIDATION_FAILURE;
        else for (int i = 0; i < SW_IMAGES; ++i) ((XrSwapchainImageVulkanKHR *)images)[i].image = s->image[i];
    } else {
        if (images->type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) r = XR_ERROR_VALIDATION_FAILURE;
        else for (int i = 0; i < SW_IMAGES; ++i) ((emul_gles_image *)images)[i].image = s->tex[i];
    }
    pthread_mutex_unlock(&sw_lock);
    return r;
}

static XrResult sw_acquire(XrSwapchain handle, uint32_t *index) {
    if (!index) return XR_ERROR_VALIDATION_FAILURE;
    int slot = sw_find(handle);
    if (slot < 0) return XR_ERROR_HANDLE_INVALID;
    pthread_mutex_lock(&sw_lock);
    *index = sw_chains[slot].next;
    sw_chains[slot].next = (sw_chains[slot].next + 1) % SW_IMAGES;
    pthread_mutex_unlock(&sw_lock);
    if (__atomic_add_fetch(&sw_acquires, 1, __ATOMIC_RELAXED) == 600)
        LOG("space_warp_emul: space warp images: 600 acquired, %d waited, %d released (all by FrameBridge)",
            __atomic_load_n(&sw_waits, __ATOMIC_RELAXED), __atomic_load_n(&sw_releases, __ATOMIC_RELAXED));
    return XR_SUCCESS;
}

static void sw_destroy_slot(int i) {  // sw_lock held
    sw_swapchain *s = &sw_chains[i];
    if (s->image[0]) sw_vk_free(s);
    else if (s->tex[0] && p_eglGetCurrentContext && p_eglGetCurrentContext() != EGL_NO_CONTEXT) p_glDeleteTextures(SW_IMAGES, s->tex);
    memset(s, 0, sizeof *s);
}

static void sw_destroy(XrSwapchain handle) {
    pthread_mutex_lock(&sw_lock);
    for (int i = 0; i < SW_MAX; ++i)
        if (sw_chains[i].handle == handle) sw_destroy_slot(i);
    pthread_mutex_unlock(&sw_lock);
}

static void sw_session_destroy(XrSession session) {
    pthread_mutex_lock(&sw_lock);
    for (int i = 0; i < SW_MAX; ++i)
        if (sw_chains[i].handle && sw_chains[i].session == session) sw_destroy_slot(i);
    pthread_mutex_unlock(&sw_lock);
}

// Wait/release wrappers (installed in front of whatever wait/release hook the other settings chose).
static PFN_xrWaitSwapchainImage sw_next_wait;
static PFN_xrReleaseSwapchainImage sw_next_release;
static XRAPI_ATTR XrResult XRAPI_CALL sw_wait(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *info) {
    if (sw_served_wait(swapchain)) return XR_SUCCESS;
    return sw_next_wait ? sw_next_wait(swapchain, info) : XR_ERROR_FUNCTION_UNSUPPORTED;
}
static XRAPI_ATTR XrResult XRAPI_CALL sw_release(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info) {
    if (sw_served_release(swapchain)) return XR_SUCCESS;
    return sw_next_release ? sw_next_release(swapchain, info) : XR_ERROR_FUNCTION_UNSUPPORTED;
}

// xrEnumerateSwapchainFormats (only hooked with the setting): the formats space warp asks for, appended after the
// runtime's own when it lacks them, so an app that checks the list before creating its motion-vector and depth
// swapchains finds them. Eye-sized swapchains in these formats still go to the runtime (swapchain_fix's fallback).
static XRAPI_ATTR XrResult XRAPI_CALL sw_enumerate_formats(XrSession session, uint32_t capacity, uint32_t *count, int64_t *formats) {
    PFN_xrEnumerateSwapchainFormats fn = (PFN_xrEnumerateSwapchainFormats)lookup(active_instance, "xrEnumerateSwapchainFormats");
    if (!fn) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!count || (capacity && !formats)) return XR_ERROR_VALIDATION_FAILURE;
    uint32_t total = 0;
    XrResult r = fn(session, 0, &total, NULL);
    if (XR_FAILED(r)) return r;
    int64_t *all = calloc(total + 8, sizeof *all);
    if (!all) return XR_ERROR_OUT_OF_MEMORY;
    if (total && XR_FAILED(r = fn(session, total, &total, all))) { free(all); return r; }
    static const int64_t gl_extra[] = {0x881A, 0x88F0, 0x81A6, 0x8CAC, 0x81A5};  // RGBA16F, D24S8, D24, D32F, D16
    static const int64_t vk_extra[] = {97, 129, 126, 125, 124};                // the same in Vulkan
    const int64_t *extra = gles_session ? gl_extra : vk_extra;
    uint32_t n = total;
    char added[160] = "";
    for (int e = 0; e < 5; ++e) {
        int present = 0;
        for (uint32_t i = 0; i < total; ++i) present |= all[i] == extra[e];
        if (present) continue;
        all[n++] = extra[e];
        size_t used = strlen(added);
        snprintf(added + used, sizeof added - used, " %lld", (long long)extra[e]);
    }
    static int logged;
    if (!logged++) {
        char list[512] = "";
        for (uint32_t i = 0; i < total; ++i) {
            size_t used = strlen(list);
            snprintf(list + used, sizeof list - used, " %lld", (long long)all[i]);
        }
        LOG("space_warp_emul: runtime swapchain formats:%s; added:%s", list, *added ? added : " none");
    }
    *count = n;
    if (capacity) {
        if (capacity < n) { free(all); return XR_ERROR_SIZE_INSUFFICIENT; }
        memcpy(formats, all, n * sizeof *all);
    }
    free(all);
    return XR_SUCCESS;
}

// xrEndFrame: what a projection view's chain must not take to the runtime: space warp info, and depth info that shows
// one of FrameBridge's space-warp images.
static int sw_removed(const XrBaseInStructure *n) {
    return n->type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB ||
           (n->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR &&
            sw_is_fake(((const XrCompositionLayerDepthInfoKHR *)n)->subImage.swapchain));
}

// The view's chain without those structs. The chain is const: depth infos in front of a removed struct are copied
// (into `copies`) so they can skip it; an unknown struct is kept with everything after it, unless a removed struct
// follows it, then the whole chain is dropped (as hide_space_warp always did). Returns `head` when nothing is removed.
static const void *sw_strip_chain(const void *head, XrCompositionLayerDepthInfoKHR copies[4], int *changed) {
    int remove_any = 0;
    for (const XrBaseInStructure *n = head; n; n = n->next) remove_any |= sw_removed(n);
    if (!remove_any) return head;
    *changed = 1;
    const XrBaseInStructure *kept[5];
    int k = 0, unknown_tail = 0;
    for (const XrBaseInStructure *n = head; n; n = n->next) {
        if (sw_removed(n)) continue;
        if (n->type != XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
            for (const XrBaseInStructure *m = n->next; m; m = m->next)
                if (sw_removed(m)) return NULL;
            kept[k++] = n;
            unknown_tail = 1;
            break;
        }
        if (k == 4) return NULL;
        kept[k++] = n;
    }
    const void *next = NULL;
    for (int i = k - 1; i >= 0; --i) {
        if (unknown_tail && i == k - 1) { next = kept[i]; continue; }
        copies[i] = *(const XrCompositionLayerDepthInfoKHR *)kept[i];
        copies[i].next = next;
        next = &copies[i];
    }
    return next;
}
