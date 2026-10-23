// SPDX-License-Identifier: GPL-3.0-only
// Included by frame_adapter.c (after layer_emul_gl.c, whose GL entry points it shares). cube_standin (default on):
// the Frame's runtime has no cube layers (XR_KHR_composition_layer_cube) and refuses cube swapchains (faceCount 6)
// with XR_ERROR_RUNTIME_FAILURE. OVRPlugin doesn't check: it goes on with "CreateSwapchain for eye 0: 0x0, 0 stages"
// and writes past its empty image list in the next ovrp_EndFrame4 (Unity's render thread SIGSEGV in memset, e.g.
// Budget Cuts Ultimate's cube-map overlay, GitHub #107). Only when the runtime refused one, a cube swapchain is served
// by the adapter instead: one GL cube-map texture or Vulkan cube-compatible image with the requested format,
// size and mip levels. Acquire/wait/release succeed at once. The app renders or copies into it as usual; layers
// are converted to six quad faces by cube_quads when no projection is present.
#define STANDIN_MAX 8
typedef struct {
    XrSwapchain handle;
    XrSession session;
    XrSwapchainCreateInfo info;
    uint32_t slice;
    GLuint tex;
    VkDevice device;
    VkImage image;
    VkDeviceMemory memory;
} standin_swapchain;
static standin_swapchain standins[STANDIN_MAX];
static pthread_mutex_t standin_lock = PTHREAD_MUTEX_INITIALIZER;

static int standin_find(XrSwapchain handle) {
    if (!handle) return -1;
    int slot = -1;
    pthread_mutex_lock(&standin_lock);
    for (int i = 0; i < STANDIN_MAX && slot < 0; ++i)
        if (standins[i].handle == handle) slot = i;
    pthread_mutex_unlock(&standin_lock);
    return slot;
}

static int is_standin(XrSwapchain handle) { return cube_standin && standin_find(handle) >= 0; }

// Uses the app's Vulkan queue, with a private pool so the blitter's command buffer stays untouched.
static int standin_vk_create(const XrSwapchainCreateInfo *info, standin_swapchain *s) {
    if (info->sampleCount != 1 || !info->mipCount || !info->arraySize ||
        (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        (info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) return 0;
    int ok = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    pthread_mutex_lock(&flip_lock);
    if (!vk_ready()) goto done;
    PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)vk.GetDeviceProcAddr(vk.device, "vkDestroyCommandPool");
    PFN_vkQueueWaitIdle wait_idle = (PFN_vkQueueWaitIdle)vk.GetDeviceProcAddr(vk.device, "vkQueueWaitIdle");
    if (!destroy_pool || !wait_idle) goto done;
    s->device = vk.device;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_MND) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        VK_IMAGE_TYPE_2D, (VkFormat)info->format, {info->width, info->height, 1}, info->mipCount, 6,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        VK_IMAGE_LAYOUT_UNDEFINED};
    if (vk.CreateImage(s->device, &ci, NULL, &s->image) != VK_SUCCESS) goto done;
    VkMemoryRequirements req;
    vk.GetImageMemoryRequirements(s->device, s->image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (vk.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (type == UINT32_MAX || vk.AllocateMemory(s->device, &ai, NULL, &s->memory) != VK_SUCCESS ||
        vk.BindImageMemory(s->device, s->image, s->memory, 0) != VK_SUCCESS) goto done;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, vk.family};
    if (vk.CreateCommandPool(s->device, &pi, NULL, &pool) != VK_SUCCESS) goto done;
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vk.AllocateCommandBuffers(s->device, &alloc, &cmd) != VK_SUCCESS) goto done;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    if (vk.BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) goto done;
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, s->image,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, info->mipCount, 0, 6}};
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, 1, &b);
    if (vk.EndCommandBuffer(cmd) != VK_SUCCESS) goto done;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    if (vk.QueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) goto done;
    ok = wait_idle(vk.queue) == VK_SUCCESS;
done:
    if (pool) destroy_pool(s->device, pool, NULL);
    if (!ok) {
        if (s->image) vk.DestroyImage(s->device, s->image, NULL);
        if (s->memory) vk.FreeMemory(s->device, s->memory, NULL);
        s->device = VK_NULL_HANDLE;
        s->image = VK_NULL_HANDLE;
        s->memory = VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&flip_lock);
    return ok;
}

// After the runtime refused a swapchain: serve a cube stand-in if the graphics API can supply it.
static XrResult standin_after_failure(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *out, XrResult result) {
    if (XR_SUCCEEDED(result) || !cube_standin || !info || !out || info->faceCount != 6) return result;
    if (info->width != info->height || !info->width || info->arraySize > 1) return result;
    int gles = (p_eglCreateContext || emul_load_gl()) && p_eglGetCurrentContext() != EGL_NO_CONTEXT;
    int slot = -1;
    pthread_mutex_lock(&standin_lock);
    for (int i = 0; i < STANDIN_MAX && slot < 0; ++i)
        if (!standins[i].handle) slot = i;
    if (slot >= 0) standins[slot].handle = (XrSwapchain)1;  // reserved
    pthread_mutex_unlock(&standin_lock);
    if (slot < 0) { LOG("cube_standin: too many cube swapchains, the runtime's error stays"); return result; }
    cube_outputs_destroy();
    standins[slot].session = session;
    standins[slot].info = *info;
    if (!gles) {
        if (!standin_vk_create(info, &standins[slot])) {
            pthread_mutex_lock(&standin_lock);
            standins[slot].handle = XR_NULL_HANDLE;
            pthread_mutex_unlock(&standin_lock);
            LOG("cube_standin: couldn't make a Vulkan cube image, the runtime's error stays");
            return result;
        }
        pthread_mutex_lock(&standin_lock);
        *out = standins[slot].handle = FAKE_HANDLE(XrSwapchain);
        pthread_mutex_unlock(&standin_lock);
        LOG("cube_standin: runtime refused a %ux%u cube swapchain (result=%d): served by FrameBridge "
            "(Vulkan cube image, %u levels); cube_quads can display its layers", info->width, info->height, result, info->mipCount);
        return XR_SUCCESS;
    }
    GLsizei levels = info->mipCount > 1 ? (GLsizei)info->mipCount : 1;
    for (int i = 0; i < 8 && p_glGetError() != GL_NO_ERROR; ++i) {}
    GLint previous = 0;
    p_glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &previous);
    GLuint tex = 0;
    p_glGenTextures(1, &tex);
    p_glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    p_glTexStorage2D(GL_TEXTURE_CUBE_MAP, levels, (GLenum)info->format, (GLsizei)info->width, (GLsizei)info->height);
    GLenum err = p_glGetError();
    p_glBindTexture(GL_TEXTURE_CUBE_MAP, (GLuint)previous);  // the app's binding, untouched
    if (err != GL_NO_ERROR) {
        p_glDeleteTextures(1, &tex);
        pthread_mutex_lock(&standin_lock);
        standins[slot].handle = XR_NULL_HANDLE;
        pthread_mutex_unlock(&standin_lock);
        LOG("cube_standin: couldn't make a %ux%u cube map (GL 0x%x), the runtime's error stays", info->width,
            info->height, err);
        return result;
    }
    XrSwapchain handle = FAKE_HANDLE(XrSwapchain);
    pthread_mutex_lock(&standin_lock);
    standins[slot].handle = handle;
    standins[slot].tex = tex;
    pthread_mutex_unlock(&standin_lock);
    *out = handle;
    LOG("cube_standin: runtime refused a %ux%u cube swapchain (result=%d): served by FrameBridge (GL cube map %u, "
        "%d levels); cube_quads can display its layers", info->width, info->height, result, tex, (int)levels);
    return XR_SUCCESS;
}

static XrResult standin_enumerate(XrSwapchain handle, uint32_t capacity, uint32_t *count, XrSwapchainImageBaseHeader *images) {
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = 1;
    if (!capacity) return XR_SUCCESS;
    if (!images) return XR_ERROR_VALIDATION_FAILURE;
    int slot = standin_find(handle);
    if (slot < 0) return XR_ERROR_HANDLE_INVALID;
    if (standins[slot].image) {
        if (images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return XR_ERROR_VALIDATION_FAILURE;
        ((XrSwapchainImageVulkanKHR *)images)[0].image = standins[slot].image;
    } else {
        if (images->type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) return XR_ERROR_VALIDATION_FAILURE;
        ((emul_gles_image *)images)[0].image = standins[slot].tex;
    }
    return XR_SUCCESS;
}

static void standin_destroy(XrSwapchain handle) {
    cube_outputs_destroy();
    pthread_mutex_lock(&standin_lock);
    for (int i = 0; i < STANDIN_MAX; ++i)
        if (standins[i].handle == handle) {
            if (standins[i].image) {
                vk.DestroyImage(standins[i].device, standins[i].image, NULL);
                vk.FreeMemory(standins[i].device, standins[i].memory, NULL);
            } else if (p_eglGetCurrentContext && p_eglGetCurrentContext() != EGL_NO_CONTEXT) p_glDeleteTextures(1, &standins[i].tex);
            memset(&standins[i], 0, sizeof(standins[i]));
        }
    pthread_mutex_unlock(&standin_lock);
}

// A layer that shows a stand-in: never passed to the runtime (whatever layer_fix says).
static int layer_uses_standin(const XrCompositionLayerBaseHeader *layer) {
    if (!cube_standin || !layer) return 0;
    switch (layer->type) {
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
        return is_standin(((const XrCompositionLayerCubeKHR *)layer)->swapchain);
    case XR_TYPE_COMPOSITION_LAYER_QUAD:
        return is_standin(((const XrCompositionLayerQuad *)layer)->subImage.swapchain);
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
        return is_standin(((const XrCompositionLayerCylinderKHR *)layer)->subImage.swapchain);
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
        return is_standin(((const XrCompositionLayerEquirectKHR *)layer)->subImage.swapchain);
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
        return is_standin(((const XrCompositionLayerEquirect2KHR *)layer)->subImage.swapchain);
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
        const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)layer;
        for (uint32_t v = 0; v < p->viewCount; ++v)
            if (is_standin(p->views[v].subImage.swapchain)) return 1;
        return 0;
    }
    default:
        return 0;
    }
}

// XR_FB_swapchain_update_state on a stand-in: nothing to update (only hooked when the runtime has the function).
static PFN_xrUpdateSwapchainFB real_update_swapchain;
static PFN_xrGetSwapchainStateFB real_get_swapchain_state;
static XRAPI_ATTR XrResult XRAPI_CALL standin_update_swapchain(XrSwapchain swapchain, const XrSwapchainStateBaseHeaderFB *state) {
    if (is_standin(swapchain)) return XR_SUCCESS;
    return real_update_swapchain ? real_update_swapchain(swapchain, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
}
static XRAPI_ATTR XrResult XRAPI_CALL standin_get_swapchain_state(XrSwapchain swapchain, XrSwapchainStateBaseHeaderFB *state) {
    if (is_standin(swapchain)) return XR_ERROR_VALIDATION_FAILURE;  // no state to report (apps treat it as "unsupported")
    return real_get_swapchain_state ? real_get_swapchain_state(swapchain, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
}

// Six runtime swapchains per size/format. Array slices keep different stand-ins independent,
// even when several cube layers of the same size appear in one frame.
static struct {
    XrSession session;
    uint32_t size, slices;
    int64_t format;
    int vulkan, failed, ready;
    XrSwapchain face[6];
} cube_pools[STANDIN_MAX], *cube_out;

static void cube_pool_destroy(int pool) {
    PFN_xrDestroySwapchain destroy = (PFN_xrDestroySwapchain)lookup(active_instance, "xrDestroySwapchain");
    for (int f = 0; f < 6; ++f)
        if (cube_pools[pool].face[f] && destroy) destroy(cube_pools[pool].face[f]);
    memset(&cube_pools[pool], 0, sizeof(cube_pools[pool]));
}

static void cube_outputs_destroy(void) {
    for (int i = 0; i < STANDIN_MAX; ++i) cube_pool_destroy(i);
    cube_out = NULL;
}

// Keep a failed key so a refused allocation/copy is diagnosed once, then falls back without retrying each frame.
static void cube_output_disable(void) {
    XrSession session = cube_out->session;
    uint32_t size = cube_out->size;
    int64_t format = cube_out->format;
    int vulkan = cube_out->vulkan;
    cube_pool_destroy((int)(cube_out - cube_pools));
    cube_out->session = session;
    cube_out->size = size;
    cube_out->format = format;
    cube_out->vulkan = vulkan;
    cube_out->failed = 1;
}

static int cube_outputs_create(XrSession session, const standin_swapchain *s) {
    int pool = -1, empty = -1;
    for (int i = 0; i < STANDIN_MAX; ++i) {
        if (!cube_pools[i].session && empty < 0) empty = i;
        if (cube_pools[i].session == session && cube_pools[i].size == s->info.width &&
            cube_pools[i].format == s->info.format && cube_pools[i].vulkan == !!s->image) pool = i;
    }
    if (pool < 0) pool = empty;
    if (pool < 0) return 0;
    cube_out = &cube_pools[pool];
    if (cube_out->failed) return 0;
    if (cube_out->face[5]) return 1;
    cube_out->slices = 0;
    for (int i = 0; i < STANDIN_MAX; ++i)
        if (standins[i].handle && standins[i].session == session && standins[i].info.width == s->info.width &&
            standins[i].info.format == s->info.format && !!standins[i].image == !!s->image)
            standins[i].slice = cube_out->slices++;
    cube_out->session = session;
    cube_out->size = s->info.width;
    cube_out->format = s->info.format;
    cube_out->vulkan = !!s->image;
    PFN_xrCreateSwapchain create = (PFN_xrCreateSwapchain)lookup(active_instance, "xrCreateSwapchain");
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL, 0,
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        s->info.format, 1, s->info.width, s->info.height, 1, cube_out->slices, 1};
    for (int f = 0; f < 6; ++f) {
        XrResult r = create ? create(session, &ci, &cube_out->face[f]) : XR_ERROR_FUNCTION_UNSUPPORTED;
        if (XR_FAILED(r)) {
            cube_output_disable();
            LOG("cube_quads: face swapchain creation failed (result=%d); dropping cube layers", r);
            return 0;
        }
    }
    LOG("cube_quads: created six %ux%u face swapchains (%s)", s->info.width, s->info.height,
        s->image ? "Vulkan" : "GLES");
    return 1;
}

static int cube_copy_face(const standin_swapchain *s, int face, uint32_t index, uint32_t slice) {
    PFN_xrEnumerateSwapchainImages enumerate = (PFN_xrEnumerateSwapchainImages)lookup(active_instance, "xrEnumerateSwapchainImages");
    uint32_t n = 0;
    if (!enumerate || XR_FAILED(enumerate(cube_out->face[face], 0, &n, NULL)) || index >= n) return 0;
    int ok = 0;
    if (s->image) {
        XrSwapchainImageVulkanKHR *images = calloc(n, sizeof(*images));
        if (!images) return 0;
        for (uint32_t i = 0; i < n; ++i) images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        if (XR_SUCCEEDED(enumerate(cube_out->face[face], n, &n, (void *)images))) {
            pthread_mutex_lock(&flip_lock);
            PFN_vkQueueWaitIdle idle = (PFN_vkQueueWaitIdle)vk.GetDeviceProcAddr(vk.device, "vkQueueWaitIdle");
            VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
            if (idle && vk_cmd_idle() && vk.ResetCommandBuffer(vk.cmd, 0) == VK_SUCCESS && vk.BeginCommandBuffer(vk.cmd, &begin) == VK_SUCCESS) {
                VkImage dst = images[index].image;
                barrier(s->image, face, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                barrier(dst, slice, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                int32_t size = (int32_t)s->info.width;
                // Vulkan quad v=0 is the top; GLES quad t=0 is the bottom. Reverse rows for the same pose.
                VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)face, 1}, {{0, 0, 0}, {size, size, 1}},
                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, slice, 1}, {{0, size, 0}, {size, 0, 1}}};
                vk.CmdBlitImage(vk.cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
                barrier(s->image, face, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                barrier(dst, slice, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
                if (vk.EndCommandBuffer(vk.cmd) == VK_SUCCESS) {
                    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &vk.cmd, 0, NULL};
                    ok = vk.QueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && idle(vk.queue) == VK_SUCCESS;
                }
            }
            pthread_mutex_unlock(&flip_lock);
        }
        free(images);
    } else {
        if (!p_eglGetCurrentContext || p_eglGetCurrentContext() == EGL_NO_CONTEXT) return 0;
        emul_gles_image *images = calloc(n, sizeof(*images));
        if (!images) return 0;
        for (uint32_t i = 0; i < n; ++i) images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        if (XR_SUCCEEDED(enumerate(cube_out->face[face], n, &n, (void *)images))) {
            for (int i = 0; i < 8 && p_glGetError() != GL_NO_ERROR; ++i) {}
            GLint read, draw, scissor = p_glIsEnabled(GL_SCISSOR_TEST);
            p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
            p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
            GLuint fb[2];
            p_glGenFramebuffers(2, fb);
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
            p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, s->tex, 0);
            p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
            if (cube_out->slices == 1)
                p_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[index].image, 0);
            else p_glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, images[index].image, 0, slice);
            ok = p_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                p_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            if (ok) {
                p_glDisable(GL_SCISSOR_TEST);
                p_glBlitFramebuffer(0, 0, s->info.width, s->info.height, 0, 0, s->info.width, s->info.height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
                ok = p_glGetError() == GL_NO_ERROR;
                p_glFlush();
            }
            if (scissor) p_glEnable(GL_SCISSOR_TEST);
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
            p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
            p_glDeleteFramebuffers(2, fb);
        }
        free(images);
    }
    return ok;
}

static void cube_outputs_begin(void) {
    for (int i = 0; i < STANDIN_MAX; ++i) cube_pools[i].ready = 0;
}

static void cube_session_destroy(XrSession session) {
    cube_outputs_destroy();
    for (int i = 0; i < STANDIN_MAX; ++i)
        if (standins[i].session == session && standins[i].handle) standin_destroy(standins[i].handle);
}

// xrEndSession may be followed by another xrBeginSession without destroying the session or app cube handles.
static XRAPI_ATTR XrResult XRAPI_CALL cube_end_session(XrSession session) {
    cube_outputs_destroy();
    PFN_xrEndSession end = (PFN_xrEndSession)lookup(active_instance, "xrEndSession");
    return end ? end(session) : XR_ERROR_FUNCTION_UNSUPPORTED;
}

static int cube_layer_quads(XrSession session, const XrFrameEndInfo *info, uint32_t at, XrCompositionLayerQuad *out) {
    const XrCompositionLayerCubeKHR *c = (const void *)info->layers[at];
    int slot = standin_find(c->swapchain);
    if (slot < 0 || c->imageArrayIndex != 0) return 0;
    // Quads always cover projections on Frame, including alpha projections. Conservatively skip any mixed frame.
    for (uint32_t i = 0; i < info->layerCount; ++i)
        if (info->layers[i] && (info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)) {
            static int logged;
            if (!logged++) LOG("cube_quads: skipping cube with projection; preserving scene visibility");
            return 0;
        }
    standin_swapchain *s = &standins[slot];
    if (s->session != session || !cube_outputs_create(session, s)) return 0;
    PFN_xrAcquireSwapchainImage acquire = (PFN_xrAcquireSwapchainImage)lookup(active_instance, "xrAcquireSwapchainImage");
    PFN_xrWaitSwapchainImage wait = (PFN_xrWaitSwapchainImage)lookup(active_instance, "xrWaitSwapchainImage");
    PFN_xrReleaseSwapchainImage release = (PFN_xrReleaseSwapchainImage)lookup(active_instance, "xrReleaseSwapchainImage");
    if (!acquire || !wait || !release) return 0;
    if (!cube_out->ready) for (int f = 0; f < 6; ++f) {
        uint32_t index;
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, NULL};
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION};
        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, NULL};
        if (XR_FAILED(acquire(cube_out->face[f], &ai, &index))) goto fail;
        if (XR_FAILED(wait(cube_out->face[f], &wi))) goto fail;
        int ok = 1;
        for (int k = 0; k < STANDIN_MAX && ok; ++k) {
            if (!standins[k].handle || standins[k].session != session || standins[k].info.width != s->info.width ||
                standins[k].info.format != s->info.format || !!standins[k].image != !!s->image) continue;
            int submitted = 0;
            for (uint32_t l = 0; l < info->layerCount; ++l)
                if (info->layers[l] && info->layers[l]->type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR &&
                    ((const XrCompositionLayerCubeKHR *)info->layers[l])->swapchain == standins[k].handle) submitted = 1;
            if (submitted) ok = cube_copy_face(&standins[k], f, index, standins[k].slice);
        }
        XrResult r = release(cube_out->face[f], &ri);
        if (!ok || XR_FAILED(r)) goto fail;
    }
    cube_out->ready = 1;
    XrVector3f centre = {0, 0, 0};
    PFN_xrLocateViews locate = (PFN_xrLocateViews)lookup(active_instance, "xrLocateViews");
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, info->displayTime, c->space};
    XrViewState state = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t nviews;
    if (locate && XR_SUCCEEDED(locate(session, &li, &state, 2, &nviews, views)) && nviews == 2 &&
        (state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
        centre = (XrVector3f){(views[0].pose.position.x + views[1].pose.position.x) / 2,
            (views[0].pose.position.y + views[1].pose.position.y) / 2,
            (views[0].pose.position.z + views[1].pose.position.z) / 2};
    for (int f = 0; f < 6; ++f) {
        out[f] = (XrCompositionLayerQuad){XR_TYPE_COMPOSITION_LAYER_QUAD, NULL, c->layerFlags, c->space,
            c->eyeVisibility, {cube_out->face[f], {{0, 0}, {(int32_t)s->info.width, (int32_t)s->info.height}}, s->slice},
            lm_cube_quad_pose(c->orientation, f, 50), {100, 100}};
        out[f].pose.position.x += centre.x;
        out[f].pose.position.y += centre.y;
        out[f].pose.position.z += centre.z;
    }
    {
        static int logged;
        if (!logged++) LOG("cube_quads: submitting cube as six quads (half-size=50m, mip=0)");
    }
    return 1;
fail:
    cube_output_disable();
    LOG("cube_quads: face copy/acquire/wait/release failed; dropping cube layers");
    return 0;
}
