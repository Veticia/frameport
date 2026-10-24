// Included after layer_emul_gl.c (GLES entry points) and flip_vk.c (the Vulkan session binding: vk, vk_ready,
// flip_lock and the tracking of the app's images: flip_source).
// Proactive at eight app swapchains: an atlas cannot be allocated after the runtime cap.
// GLES sessions: private textures, blitted into the atlas in the app's context. Vulkan sessions (XR_KHR_vulkan_enable
// and enable2 share the binding type): private VkImages, copied with vkCmdCopyImage on the binding's queue inside
// xrEndFrame. Vulkan pools only panels in the atlas format (sRGB RGBA8); eye images (arraySize 2) never are pooled.
// Vertical flips (Vulkan, XR_FB_composition_layer_image_layout, which the Frame's runtime ignores; also without
// swapchain_pool): a flagged quad shows an upside-down copy in an atlas, made every frame it is submitted. Pooled
// panels get it from their atlas copy itself (a blit with reversed rows instead of vkCmdCopyImage, same cost); runtime
// swapchains get a per-frame atlas area ("mirror") with a blit of the app's shown image. The app's images are never
// changed, so drawing that lands late (Unity finishes some panels after xrEndFrame without acquiring them again) shows
// upright one frame later instead of staying upside down, and no flip can be applied twice.
#include "pool_rect.h"
#define POOL_MAX 128
#define POOL_SIDE 4096
#define POOL_VK_RING 3
static XrSession pool_session;
static int pool_vulkan;  // pool_session has a Vulkan graphics binding
static int pool_flips;   // ... and flipped quads are emulated through the atlas (flip_emulated())
static struct {
    XrSwapchain handle;
    GLuint images[16];
    VkImage vk_images[16];
    uint32_t count, index;
    int acquired, waited;
    int vk_dst;  // Vulkan: this frame's atlas image is in TRANSFER_DST_OPTIMAL in the open command buffer
} pool_atlas[2];
static struct {
    XrSwapchain handle;
    GLuint tex;
    PoolRect rect;
    int atlas, state, released, copied;
    GLsync fence, read_fence;
    VkImage vk_image;
    VkDeviceMemory vk_memory;
    int vk_ready;  // moved out of UNDEFINED into COLOR_ATTACHMENT_OPTIMAL (at its first acquire)
} pool_images[POOL_MAX];
// Vulkan copies: own command buffers in a small ring, so xrEndFrame never waits for this frame's GPU work. They run
// on the session's queue after the app's rendering (OpenXR: the app has submitted its writes before it releases),
// and flip_vk.c's blitter uses the same queue: every submit holds flip_lock (lock order pool_lock -> flip_lock).
static struct {
    int state;  // 0 = not loaded, 1 = ready, -1 = failed
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd[POOL_VK_RING];
    VkFence fence[POOL_VK_RING];
    int submitted[POOL_VK_RING];
    int slot, recording;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
} pvk;
static unsigned pool_errors;
// layer_debug: per ~5 s, the frame's copy volume and every wait inside xrEndFrame (ring fence, atlas acquire+wait)
static struct {
    unsigned frames, copies;
    uint64_t pixels, ring_ns, ring_max, atlas_ns, atlas_max, total_ns, total_max;
} pool_stats;
static uint64_t pool_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void pool_stat(uint64_t *sum, uint64_t *max, uint64_t ns) {
    *sum += ns;
    if (ns > *max) *max = ns;
}
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
// Why a create was not pooled: logged once per reason (the first device run of #424 pooled nothing, silently).
enum {
    POOL_NO_SESSION, POOL_SHAPE, POOL_SAMPLES, POOL_MIPS, POOL_USAGE, POOL_FLAGS, POOL_SIZE, POOL_FORMAT, POOL_CHAIN,
    POOL_DEVICE, POOL_GL, POOL_SLOTS, POOL_FULL, POOL_ATLAS, POOL_IMAGE
};
static unsigned pool_declined;
static int pool_last_decline = -1;  // for the host harness
#define POOL_DECLINE(reason, ...)                                                                                      \
    do {                                                                                                               \
        pool_last_decline = (reason);                                                                                  \
        if (!(pool_declined & (1u << (reason)))) {                                                                     \
            pool_declined |= 1u << (reason);                                                                           \
            LOG(__VA_ARGS__);                                                                                          \
        }                                                                                                              \
        return 0;                                                                                                      \
    } while (0)
static int pool_should_create_locked(XrSession session) {
    if (!swapchain_pool) return 0;
    if (session != pool_session) {
        static int logged;
        if (!logged++)
            LOG("swapchain_pool: create in session %p, not the pooled session %p", (void *)(uintptr_t)session,
                (void *)(uintptr_t)pool_session);
        return 0;
    }
    static unsigned announced;
    if (pool_runtime_count >= 8 && !announced++)
        LOG("swapchain_pool: %u live runtime swapchains: pooling further panels", pool_runtime_count);
    return pool_runtime_count >= 8;
}
static int pool_find_locked(XrSwapchain h) {
    for (int i = 0; h && i < POOL_MAX; ++i)
        if (pool_images[i].handle == h) return i;
    return -1;
}
static void pool_error(void) {
    if (pool_errors++ % 240 == 0) LOG("swapchain_pool: atlas update failed; omitting affected layers");
}

// ---- Vulkan helpers
static int pool_vk_load_locked(void) {
    if (pvk.state) return pvk.state > 0;
    pthread_mutex_lock(&flip_lock);
    int ready = vk_ready();  // the session binding's device, queue and memory types
    pthread_mutex_unlock(&flip_lock);
    pvk.state = -1;
    if (!ready) {
        LOG("swapchain_pool: Vulkan device unavailable; panels keep runtime swapchains");
        return 0;
    }
#define PVK(name)                                                                                                      \
    pvk.name = (PFN_vk##name)vk.GetDeviceProcAddr(vk.device, "vk" #name);                                             \
    if (!pvk.name) return 0;
    PVK(CreateImage) PVK(DestroyImage) PVK(GetImageMemoryRequirements) PVK(AllocateMemory) PVK(FreeMemory)
    PVK(BindImageMemory) PVK(CreateCommandPool) PVK(DestroyCommandPool) PVK(AllocateCommandBuffers)
    PVK(BeginCommandBuffer) PVK(EndCommandBuffer) PVK(ResetCommandBuffer) PVK(CmdPipelineBarrier) PVK(CmdCopyImage)
    PVK(CmdBlitImage)
    PVK(QueueSubmit) PVK(CreateFence) PVK(DestroyFence) PVK(WaitForFences) PVK(ResetFences)
#undef PVK
    VkCommandPoolCreateInfo cp = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                  VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, vk.family};
    if (pvk.CreateCommandPool(vk.device, &cp, NULL, &pvk.cmd_pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pvk.cmd_pool,
                                      VK_COMMAND_BUFFER_LEVEL_PRIMARY, POOL_VK_RING};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    int ok = pvk.AllocateCommandBuffers(vk.device, &ca, pvk.cmd) == VK_SUCCESS;
    for (int k = 0; ok && k < POOL_VK_RING; ++k) ok = pvk.CreateFence(vk.device, &fi, NULL, &pvk.fence[k]) == VK_SUCCESS;
    if (!ok) {
        for (int k = 0; k < POOL_VK_RING; ++k)
            if (pvk.fence[k]) pvk.DestroyFence(vk.device, pvk.fence[k], NULL);
        pvk.DestroyCommandPool(vk.device, pvk.cmd_pool, NULL);  // frees its command buffers
        memset(pvk.fence, 0, sizeof pvk.fence);
        pvk.cmd_pool = VK_NULL_HANDLE;
        return 0;
    }
    pvk.state = 1;
    LOG("swapchain_pool: Vulkan copies ready (queue family %u index %u)", vk.family, vk.index);
    return 1;
}
// Our earlier copies may still read a private image or write an atlas image: wait before freeing either.
static void pool_vk_idle_locked(void) {
    if (pvk.state <= 0) return;
    for (int k = 0; k < POOL_VK_RING; ++k)
        if (pvk.submitted[k]) {
            int done = pvk.WaitForFences(vk.device, 1, &pvk.fence[k], VK_TRUE, 1000000000ull) == VK_SUCCESS;
            pvk.ResetFences(vk.device, 1, &pvk.fence[k]);
            pvk.submitted[k] = 0;
            flip_dump_collect(k, done);
        }
}
static int pool_vk_submit_locked(VkCommandBuffer cmd, VkFence fence) {
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    pthread_mutex_lock(&flip_lock);
    VkResult r = pvk.QueueSubmit(vk.queue, 1, &submit, fence);
    pthread_mutex_unlock(&flip_lock);
    return r == VK_SUCCESS;
}
static void pool_vk_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                            VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src, dst, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, image,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};  // every mip level
    pvk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                           NULL, 1, &b);
}
// The same for one array layer of an app's runtime swapchain image (a mirrored quad's source).
static void pool_vk_barrier_layer(VkCommandBuffer cmd, VkImage image, uint32_t layer, VkImageLayout from,
                                  VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src, dst, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, image,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, layer, 1}};
    pvk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                           NULL, 1, &b);
}
// Opens the next ring command buffer (once per frame), after its previous use has finished.
static VkCommandBuffer pool_vk_begin_locked(void) {
    int k = pvk.slot;
    if (pvk.recording) return pvk.cmd[k];
    if (pvk.submitted[k]) {  // submitted POOL_VK_RING frames ago: normally long finished, so no real wait
        uint64_t t0 = pool_ns();
        VkResult waited = pvk.WaitForFences(vk.device, 1, &pvk.fence[k], VK_TRUE, 1000000000ull);
        pool_stat(&pool_stats.ring_ns, &pool_stats.ring_max, pool_ns() - t0);
        if (waited != VK_SUCCESS) return VK_NULL_HANDLE;
        pvk.ResetFences(vk.device, 1, &pvk.fence[k]);
        pvk.submitted[k] = 0;
        flip_dump_collect(k, 1);
    }
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    if (pvk.ResetCommandBuffer(pvk.cmd[k], 0) != VK_SUCCESS ||
        pvk.BeginCommandBuffer(pvk.cmd[k], &begin) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    pvk.recording = 1;
    return pvk.cmd[k];
}
// Atlas images back to the layout the runtime expects at release, then one submit for the frame's copies.
static int pool_vk_end_locked(void) {
    if (!pvk.recording) return 1;
    int k = pvk.slot;
    for (int a = 0; a < 2; ++a)
        if (pool_atlas[a].vk_dst) {
            pool_vk_barrier(pvk.cmd[k], pool_atlas[a].vk_images[pool_atlas[a].index],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_SHADER_READ_BIT);
            pool_atlas[a].vk_dst = 0;
        }
    pvk.recording = 0;
    int ok = pvk.EndCommandBuffer(pvk.cmd[k]) == VK_SUCCESS && pool_vk_submit_locked(pvk.cmd[k], pvk.fence[k]);
    if (ok) {
        pvk.submitted[k] = 1;
        pvk.slot = (k + 1) % POOL_VK_RING;
    } else {
        flip_dump_collect(k, 0);
    }
    return ok;
}
static VkImageUsageFlags pool_vk_usage(XrSwapchainUsageFlags u) {
    // + transfer source (the atlas copy) and destination (flip_vk.c turns flipped quads upright in place)
    VkImageUsageFlags v = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (u & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) v |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (u & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) v |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (u & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) v |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (u & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) v |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return v;
}
// The app's mip levels are kept (it may render or generate them); level 0 is what goes into the atlas. Flags and usage
// an XrVulkanSwapchainCreateInfoMETA asked for are added.
static int pool_vk_image_locked(const XrSwapchainCreateInfo *ci, VkImageCreateFlags extra_flags,
                                VkImageUsageFlags extra_usage, VkImage *image, VkDeviceMemory *memory) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL,
                            extra_flags | ((ci->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)
                                               ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
                                               : 0),
                            VK_IMAGE_TYPE_2D, (VkFormat)ci->format, {ci->width, ci->height, 1}, ci->mipCount, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, pool_vk_usage(ci->usageFlags) | extra_usage,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    *image = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    if (pvk.CreateImage(vk.device, &ii, NULL, image) != VK_SUCCESS) {
        *image = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryRequirements req;
    pvk.GetImageMemoryRequirements(vk.device, *image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (vk.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if (req.memoryTypeBits & (1u << i)) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (type == UINT32_MAX || pvk.AllocateMemory(vk.device, &ai, NULL, memory) != VK_SUCCESS ||
        pvk.BindImageMemory(vk.device, *image, *memory, 0) != VK_SUCCESS) {
        if (*memory) pvk.FreeMemory(vk.device, *memory, NULL);
        pvk.DestroyImage(vk.device, *image, NULL);
        *image = VK_NULL_HANDLE;
        *memory = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}
// First acquire (a call in which the runtime may use the queue too): UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL, the
// layout an app may expect after xrWaitSwapchainImage. Once per image, so this one waits for its fence.
static int pool_vk_init_layout_locked(int i) {
    if (pool_images[i].vk_ready) return 1;
    VkCommandBuffer cmd = pvk.recording ? VK_NULL_HANDLE : pool_vk_begin_locked();
    if (!cmd) return 0;
    pool_vk_barrier(cmd, pool_images[i].vk_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    0, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    int k = pvk.slot;
    if (!pool_vk_end_locked()) return 0;
    if (pvk.WaitForFences(vk.device, 1, &pvk.fence[k], VK_TRUE, 1000000000ull) != VK_SUCCESS) return 0;
    pvk.ResetFences(vk.device, 1, &pvk.fence[k]);
    pvk.submitted[k] = 0;
    pool_images[i].vk_ready = 1;
    return 1;
}
// This frame's image of atlas a in TRANSFER_DST_OPTIMAL in the open command buffer (once per frame).
static VkCommandBuffer pool_vk_atlas_dst_locked(int a) {
    VkCommandBuffer cmd = pool_vk_begin_locked();
    if (!cmd) return VK_NULL_HANDLE;
    if (!pool_atlas[a].vk_dst) {  // the runtime hands out waited images in COLOR_ATTACHMENT_OPTIMAL
        pool_vk_barrier(cmd, pool_atlas[a].vk_images[pool_atlas[a].index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
        pool_atlas[a].vk_dst = 1;
    }
    return cmd;
}
// The app's released image into its atlas rectangle (flip: rows reversed). Queue order puts this after the app's own
// rendering; the image returns to COLOR_ATTACHMENT_OPTIMAL before anything submitted later (the app's next frame)
// touches it.
static int pool_vk_copy_locked(int i, int a, int flip) {
    if (!pool_images[i].vk_ready) return 0;
    VkCommandBuffer cmd = pool_vk_atlas_dst_locked(a);
    if (!cmd) return 0;
    VkImage dst = pool_atlas[a].vk_images[pool_atlas[a].index];
    PoolRect r = pool_images[i].rect;
    VkImage src = pool_images[i].vk_image;
    pool_vk_barrier(cmd, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    if (flip) {
        VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {r.w, r.h, 1}},
                            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{r.x, r.y + r.h, 0}, {r.x + r.w, r.y, 1}}};
        pvk.CmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &blit, VK_FILTER_NEAREST);
    } else {
        VkImageCopy region = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {r.x, r.y, 0}, {(uint32_t)r.w, (uint32_t)r.h, 1}};
        pvk.CmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &region);
    }
    pool_vk_barrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    ++pool_stats.copies;
    pool_stats.pixels += (uint64_t)r.w * r.h;
    if (flip && flip_dump_due(pool_images[i].handle))
        flip_dump_record(cmd, pvk.slot, pool_images[i].handle, dst, r.x, r.y, (uint32_t)r.w, (uint32_t)r.h);
    return 1;
}

// Creates atlas a (a runtime swapchain) and enumerates its images: 0 = refused (logged by the caller).
static XrResult pool_atlas_result;
static int pool_atlas_create_locked(XrSession session, int a) {
    XrSwapchainCreateInfo ai = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ai.width = ai.height = POOL_SIDE;
    ai.arraySize = ai.faceCount = ai.sampleCount = ai.mipCount = 1;
    ai.format = pool_vulkan ? VK_FORMAT_R8G8B8A8_SRGB : GL_SRGB8_ALPHA8;
    // transfer source: flip_dump reads atlas regions back
    ai.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                    (pool_vulkan ? XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT : 0);
    PFN_xrCreateSwapchain create = (PFN_xrCreateSwapchain)lookup(active_instance, "xrCreateSwapchain");
    PFN_xrEnumerateSwapchainImages enumerate =
        (PFN_xrEnumerateSwapchainImages)lookup(active_instance, "xrEnumerateSwapchainImages");
    pool_atlas_result = create(session, &ai, &pool_atlas[a].handle);
    if (XR_FAILED(pool_atlas_result)) {
        pool_atlas[a].handle = XR_NULL_HANDLE;
        return 0;
    }
    int ok;
    if (pool_vulkan) {
        XrSwapchainImageVulkanKHR images[16];
        for (int j = 0; j < 16; ++j) images[j] = (XrSwapchainImageVulkanKHR){XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
        ok = XR_SUCCEEDED(enumerate(pool_atlas[a].handle, 16, &pool_atlas[a].count, (void *)images)) &&
             pool_atlas[a].count && pool_atlas[a].count <= 16;
        for (uint32_t j = 0; ok && j < pool_atlas[a].count; ++j) pool_atlas[a].vk_images[j] = images[j].image;
    } else {
        emul_gles_image images[16];
        for (int j = 0; j < 16; ++j) images[j] = (emul_gles_image){XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
        ok = XR_SUCCEEDED(enumerate(pool_atlas[a].handle, 16, &pool_atlas[a].count, (void *)images)) &&
             pool_atlas[a].count && pool_atlas[a].count <= 16;
        for (uint32_t j = 0; ok && j < pool_atlas[a].count; ++j) pool_atlas[a].images[j] = images[j].image;
    }
    if (!ok) {
        ((PFN_xrDestroySwapchain)lookup(active_instance, "xrDestroySwapchain"))(pool_atlas[a].handle);
        memset(&pool_atlas[a], 0, sizeof pool_atlas[a]);
        pool_atlas_result = XR_ERROR_RUNTIME_FAILURE;
        return 0;
    }
    if (pool_vulkan) LOG("swapchain_pool: Vulkan atlas %d (%u images)", a, pool_atlas[a].count);
    return 1;
}
static int pool_create_locked(XrSession session, const XrSwapchainCreateInfo *ci, XrSwapchain *out, int limited) {
    if (!swapchain_pool || !out) return 0;
    if (session != pool_session)
        POOL_DECLINE(POOL_NO_SESSION, "swapchain_pool: not pooled: session %p is not the pooled session %p",
                     (void *)(uintptr_t)session, (void *)(uintptr_t)pool_session);
    if (ci->faceCount != 1 || ci->arraySize != 1)  // eye images (arraySize 2) and cube maps stay runtime swapchains
        POOL_DECLINE(POOL_SHAPE, "swapchain_pool: not pooled: %ux%u array=%u faces=%u (eye images stay runtime)",
                     ci->width, ci->height, ci->arraySize, ci->faceCount);
    if (ci->sampleCount != 1)
        POOL_DECLINE(POOL_SAMPLES, "swapchain_pool: not pooled: %ux%u samples=%u", ci->width, ci->height,
                     ci->sampleCount);
    if (ci->mipCount < 1 || (ci->mipCount != 1 && !pool_vulkan))
        POOL_DECLINE(POOL_MIPS, "swapchain_pool: not pooled: %ux%u mips=%u", ci->width, ci->height, ci->mipCount);
    if (!(ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT))
        POOL_DECLINE(POOL_USAGE, "swapchain_pool: not pooled: %ux%u usage=0x%llx (no colour attachment)", ci->width,
                     ci->height, (unsigned long long)ci->usageFlags);
    if (ci->createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT)
        POOL_DECLINE(POOL_FLAGS, "swapchain_pool: not pooled: %ux%u flags=0x%llx", ci->width, ci->height,
                     (unsigned long long)ci->createFlags);
    if (!ci->width || !ci->height || ci->width > POOL_SIDE || ci->height > POOL_SIDE)
        POOL_DECLINE(POOL_SIZE, "swapchain_pool: not pooled: %ux%u (atlas side %d)", ci->width, ci->height,
                     POOL_SIDE);
    if (pool_vulkan ? ci->format != VK_FORMAT_R8G8B8A8_SRGB  // vkCmdCopyImage: the atlas format only
                    : ci->format != GL_RGBA8 && ci->format != GL_SRGB8_ALPHA8)
        POOL_DECLINE(POOL_FORMAT, "swapchain_pool: not pooled: %ux%u format=%lld (%s)", ci->width, ci->height,
                     (long long)ci->format, pool_vulkan ? "Vulkan pools format 43 only" : "GLES pools RGBA8/SRGB8_ALPHA8");
    // Chained structs: foveation requests don't matter for a private image the app only renders into; Meta's extra
    // Vulkan flags/usage are honoured (Vulkan). Anything else keeps a runtime swapchain.
    VkImageCreateFlags extra_flags = 0;
    VkImageUsageFlags extra_usage = 0;
    for (const XrBaseInStructure *n = ci->next; n; n = n->next) {
        if (n->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB && pool_vulkan) continue;
        if (n->type == XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META && pool_vulkan) {
            extra_flags |= ((const XrVulkanSwapchainCreateInfoMETA *)n)->additionalCreateFlags;
            extra_usage |= ((const XrVulkanSwapchainCreateInfoMETA *)n)->additionalUsageFlags;
            continue;
        }
        POOL_DECLINE(POOL_CHAIN, "swapchain_pool: not pooled: %ux%u chains struct type %d", ci->width, ci->height,
                     (int)n->type);
    }
    if (pool_vulkan) {
        if (!pool_vk_load_locked())
            POOL_DECLINE(POOL_DEVICE, "swapchain_pool: not pooled: no Vulkan device functions");
    } else {
        if (!p_eglCreateContext && !emul_load_gl()) POOL_DECLINE(POOL_GL, "swapchain_pool: not pooled: no GLES");
        if (p_eglGetCurrentContext() == EGL_NO_CONTEXT)
            POOL_DECLINE(POOL_GL, "swapchain_pool: not pooled: no current EGL context");
        // Like emul_virtual_create: stale application GL errors must not look like allocation failures.
        for (int i = 0; i < 8 && p_glGetError() != GL_NO_ERROR; ++i) {
        }
    }
    int slot = -1;
    for (int i = 0; i < POOL_MAX; ++i)
        if (!pool_images[i].handle) {
            slot = i;
            break;
        }
    if (slot < 0) POOL_DECLINE(POOL_SLOTS, "swapchain_pool: not pooled: all %d pool slots in use", POOL_MAX);
    for (int a = 0; a < 2; ++a) {
        PoolRect used[POOL_MAX], rect;
        int n = 0;
        for (int i = 0; i < POOL_MAX; ++i)
            if (pool_images[i].handle && pool_images[i].atlas == a) used[n++] = pool_images[i].rect;
        if (!pool_place(used, n, POOL_SIDE, ci->width, ci->height, &rect)) continue;
        if (!pool_atlas[a].handle && !pool_atlas_create_locked(session, a))
            POOL_DECLINE(POOL_ATLAS, "swapchain_pool: not pooled: atlas %d %dx%d refused or not enumerated (%d)", a,
                         POOL_SIDE, POOL_SIDE, pool_atlas_result);
        if (pool_vulkan) {
            if (!pool_vk_image_locked(ci, extra_flags, extra_usage, &pool_images[slot].vk_image,
                                      &pool_images[slot].vk_memory))
                POOL_DECLINE(POOL_IMAGE, "swapchain_pool: not pooled: no Vulkan image for %ux%u mips=%u", ci->width,
                             ci->height, ci->mipCount);
            pool_images[slot].vk_ready = 0;
        } else {
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
                POOL_DECLINE(POOL_IMAGE, "swapchain_pool: not pooled: GL texture %ux%u failed (0x%x)", ci->width,
                             ci->height, err);
            }
            pool_images[slot].tex = tex;
        }
        pool_images[slot].handle = FAKE_HANDLE(XrSwapchain);
        pool_images[slot].rect = rect;
        pool_images[slot].atlas = a;
        *out = pool_images[slot].handle;
        note_swapchain_size(*out, ci->width, ci->height);
        remember_swapchain(*out);
        emul_on_create_swapchain(*out, ci);
        if (pool_vulkan) {  // tracked like runtime swapchains (acquire/release state)
            XrSwapchainCreateInfo tracked_info = *ci;
            tracked_info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            flip_on_create_swapchain(*out, &tracked_info);
        }
        LOG("swapchain_pool: virtual %ux%u%s (%s)", ci->width, ci->height, pool_vulkan ? " Vulkan" : "",
            limited ? "runtime limit reached" : "runtime limit proactively avoided");
        return 1;
    }
    POOL_DECLINE(POOL_FULL, "swapchain_pool: not pooled: %ux%u fits in neither atlas", ci->width, ci->height);
}
static void pool_destroy_locked(int i) {
    emul_on_destroy_swapchain(pool_images[i].handle);
    if (pool_images[i].vk_image) {
        pool_vk_idle_locked();
        pvk.DestroyImage(vk.device, pool_images[i].vk_image, NULL);
        pvk.FreeMemory(vk.device, pool_images[i].vk_memory, NULL);
        flip_on_destroy(pool_images[i].handle);
    }
    if (pool_images[i].fence && p_eglGetCurrentContext && p_eglGetCurrentContext() != EGL_NO_CONTEXT)
        p_glDeleteSync(pool_images[i].fence);
    if (pool_images[i].tex && p_eglGetCurrentContext && p_eglGetCurrentContext() != EGL_NO_CONTEXT)
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
    if (pool_images[i].vk_image) {
        if (!images || images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return XR_ERROR_VALIDATION_FAILURE;
        ((XrSwapchainImageVulkanKHR *)images)->image = pool_images[i].vk_image;
        return XR_SUCCESS;
    }
    if (!images || images->type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) return XR_ERROR_VALIDATION_FAILURE;
    ((emul_gles_image *)images)->image = pool_images[i].tex;
    return XR_SUCCESS;
}
static XrResult pool_cycle_locked(int i, int op, uint32_t *index) {
    int *state = &pool_images[i].state;
    int vulkan = pool_images[i].vk_image != VK_NULL_HANDLE;
    if (op == 0) {
        if (!index) return XR_ERROR_VALIDATION_FAILURE;
        if (*state) return XR_ERROR_CALL_ORDER_INVALID;
        if (vulkan) {
            if (!pool_vk_init_layout_locked(i)) return XR_ERROR_RUNTIME_FAILURE;
        } else if (pool_images[i].read_fence) {
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
        if (!vulkan) {  // Vulkan: the app's writes are already on the queue our copy follows
            if (p_eglGetCurrentContext() == EGL_NO_CONTEXT) return XR_ERROR_RUNTIME_FAILURE;
            if (pool_images[i].fence) p_glDeleteSync(pool_images[i].fence);
            pool_images[i].fence = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (!pool_images[i].fence) return XR_ERROR_RUNTIME_FAILURE;
            p_glFlush();
        }
        *state = 0;
        pool_images[i].released = 1;
    }
    return XR_SUCCESS;
}
static void pool_cleanup_locked(XrSession session) {
    if (session != pool_session) return;
    pool_vk_idle_locked();
    for (int i = 0; i < POOL_MAX; ++i)
        if (pool_images[i].handle) pool_destroy_locked(i);
    for (int a = 0; a < 2; ++a)
        if (pool_atlas[a].handle) {
            ((PFN_xrDestroySwapchain)lookup(active_instance, "xrDestroySwapchain"))(pool_atlas[a].handle);
            memset(&pool_atlas[a], 0, sizeof pool_atlas[a]);
        }
    if (pvk.state > 0) {  // device objects of this session's binding
        for (int k = 0; k < POOL_VK_RING; ++k) pvk.DestroyFence(vk.device, pvk.fence[k], NULL);
        pvk.DestroyCommandPool(vk.device, pvk.cmd_pool, NULL);
        flip_dump_free();
    }
    memset(&pvk, 0, sizeof pvk);
    memset(pool_runtime, 0, sizeof pool_runtime);
    pool_runtime_count = 0;
    pool_session = XR_NULL_HANDLE;
    pool_vulkan = 0;
    pool_flips = 0;
}
// Called on final layers, after strips, flips and rect_clamp. Copy every referenced image
// each frame: runtime atlas images rotate, and untouched areas are not preserved.
#define POOL_MIRRORS 16
static struct {
    XrSwapchain source;
    XrRect2Di src;
    uint32_t layer;
    int atlas;
    PoolRect rect;
} pool_mirrors[POOL_MIRRORS];
static int pool_mirror_count;  // this frame's
static void pool_begin_frame_locked(void) {
    for (int i = 0; i < POOL_MAX; ++i) pool_images[i].copied = 0;
    pool_mirror_count = 0;
}
static int pool_gl_copy_locked(int i, int a) {
    if (p_eglGetCurrentContext() == EGL_NO_CONTEXT) return 0;
    PoolRect r = pool_images[i].rect;
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
    return pool_images[i].read_fence != NULL;
}
// This frame's acquired, waited image of atlas a.
static int pool_atlas_begin_locked(int a) {
    if (pool_atlas[a].acquired && pool_atlas[a].waited) return 1;
    uint64_t t0 = pool_ns();
    int ok = 1;
    PFN_xrAcquireSwapchainImage acquire =
        (PFN_xrAcquireSwapchainImage)lookup(active_instance, "xrAcquireSwapchainImage");
    PFN_xrWaitSwapchainImage wait = (PFN_xrWaitSwapchainImage)lookup(active_instance, "xrWaitSwapchainImage");
    if (!pool_atlas[a].acquired) {
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(acquire(pool_atlas[a].handle, &ai, &pool_atlas[a].index))) ok = 0;
        else pool_atlas[a].acquired = 1;
    }
    if (ok && !pool_atlas[a].waited) {
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION};
        if (wait(pool_atlas[a].handle, &wi) != XR_SUCCESS) ok = 0;
        else pool_atlas[a].waited = 1;
    }
    pool_stat(&pool_stats.atlas_ns, &pool_stats.atlas_max, pool_ns() - t0);
    return ok;
}
// A free w x h area of atlas a this frame: around the pooled panels' rectangles and this frame's mirrors.
static int pool_place_in_locked(int a, int w, int h, PoolRect *out) {
    PoolRect used[POOL_MAX + POOL_MIRRORS];
    int n = 0;
    for (int i = 0; i < POOL_MAX; ++i)
        if (pool_images[i].handle && pool_images[i].atlas == a) used[n++] = pool_images[i].rect;
    for (int m = 0; m < pool_mirror_count; ++m)
        if (pool_mirrors[m].atlas == a) used[n++] = pool_mirrors[m].rect;
    return pool_place(used, n, POOL_SIDE, w, h, out);
}
// A flipped quad of a runtime swapchain: its rectangle of the app's shown image, upside down, into a free atlas area
// for this frame (the two eyes' quads of a panel share one copy). 1 with sub left as it is when it can't be done (the
// panel then shows upside down, as without emulation): unknown/undrawn source, other format, no room, no atlas.
static int pool_mirror_locked(XrSwapchainSubImage *sub) {
    for (int m = 0; m < pool_mirror_count; ++m)
        if (pool_mirrors[m].source == sub->swapchain && pool_mirrors[m].layer == sub->imageArrayIndex &&
            !memcmp(&pool_mirrors[m].src, &sub->imageRect, sizeof sub->imageRect)) {
            PoolRect r = pool_mirrors[m].rect;
            sub->swapchain = pool_atlas[pool_mirrors[m].atlas].handle;
            sub->imageRect = (XrRect2Di){{r.x, r.y}, {r.w, r.h}};
            sub->imageArrayIndex = 0;
            return 1;
        }
    static unsigned logged;
    VkImage image;
    int64_t format;
    uint32_t width, height;
    if (!pool_vulkan || !pool_vk_load_locked() ||
        !flip_source(sub->swapchain, sub->imageArrayIndex, &image, &format, &width, &height))
        return 1;
    if (format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_B8G8R8A8_SRGB) {  // a blit into the sRGB atlas
        if (!(logged & 1)) LOG("flip: quad swapchain=%p format %lld: left as it is", (void *)(uintptr_t)sub->swapchain, (long long)format);
        logged |= 1;
        return 1;
    }
    XrRect2Di src = sub->imageRect;
    clamp_rect(&src, width, height);
    if (src.offset.x < 0 || src.offset.y < 0 || src.extent.width <= 0 || src.extent.height <= 0 ||
        (int64_t)src.offset.x + src.extent.width > width || (int64_t)src.offset.y + src.extent.height > height ||
        pool_mirror_count >= POOL_MIRRORS)
        return 1;
    PoolRect place;
    int a = -1;
    for (int k = 0; k < 2 && a < 0; ++k)  // atlases that exist, then a new one
        if (pool_atlas[k].handle && pool_place_in_locked(k, src.extent.width, src.extent.height, &place)) a = k;
    for (int k = 0; k < 2 && a < 0; ++k)
        if (!pool_atlas[k].handle) {
            if (!pool_atlas_create_locked(pool_session, k)) {
                if (!(logged & 2)) LOG("flip: atlas %d refused (%d): flipped quads stay as they are", k, pool_atlas_result);
                logged |= 2;
                return 1;
            }
            if (pool_place_in_locked(k, src.extent.width, src.extent.height, &place)) a = k;
            break;
        }
    if (a < 0) {
        if (!(logged & 4)) LOG("flip: no atlas room for a %dx%d quad this frame", src.extent.width, src.extent.height);
        logged |= 4;
        return 1;
    }
    if (!pool_atlas_begin_locked(a)) return 0;
    VkCommandBuffer cmd = pool_vk_atlas_dst_locked(a);
    if (!cmd) return 0;
    VkImage dst = pool_atlas[a].vk_images[pool_atlas[a].index];
    uint32_t layer = sub->imageArrayIndex;
    pool_vk_barrier_layer(cmd, image, layer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT);
    VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                        {{src.offset.x, src.offset.y, 0},
                         {src.offset.x + src.extent.width, src.offset.y + src.extent.height, 1}},
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        {{place.x, place.y + place.h, 0}, {place.x + place.w, place.y, 1}}};  // rows reversed
    pvk.CmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &blit, VK_FILTER_NEAREST);
    pool_vk_barrier_layer(cmd, image, layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    ++pool_stats.copies;
    pool_stats.pixels += (uint64_t)place.w * place.h;
    if (flip_dump_due(sub->swapchain))
        flip_dump_record(cmd, pvk.slot, sub->swapchain, dst, place.x, place.y, (uint32_t)place.w, (uint32_t)place.h);
    if (!(logged & 8)) LOG("flip: quads flagged upside down are shown as upright copies (atlas %d)", a);
    logged |= 8;
    pool_mirrors[pool_mirror_count].source = sub->swapchain;
    pool_mirrors[pool_mirror_count].src = sub->imageRect;
    pool_mirrors[pool_mirror_count].layer = layer;
    pool_mirrors[pool_mirror_count].atlas = a;
    pool_mirrors[pool_mirror_count++].rect = place;
    sub->swapchain = pool_atlas[a].handle;
    sub->imageRect = (XrRect2Di){{place.x, place.y}, {place.w, place.h}};
    sub->imageArrayIndex = 0;
    return 1;
}
// flip: a quad flagged VERTICAL_FLIP in a flip-emulating Vulkan session.
static int pool_subimage_locked(XrSwapchainSubImage *sub, int flip) {
    int i = pool_find_locked(sub->swapchain);
    if (i < 0) return flip ? pool_mirror_locked(sub) : 1;
    int a = pool_images[i].atlas;
    if (!pool_images[i].released || pool_images[i].state || sub->imageArrayIndex) return 0;
    if (!pool_atlas_begin_locked(a)) return 0;
    PoolRect r = pool_images[i].rect;
    flip = flip && pool_images[i].vk_image;
    if (!pool_images[i].copied) {
        if (!(pool_images[i].vk_image ? pool_vk_copy_locked(i, a, flip) : pool_gl_copy_locked(i, a))) return 0;
        pool_images[i].copied = flip ? 2 : 1;
    } else if ((pool_images[i].copied == 2) != flip) {
        static int mixed;
        if (!mixed++) LOG("flip: a pooled panel is shown flipped and unflipped in one frame: both as copied first");
    }
    clamp_rect(&sub->imageRect, r.w, r.h);
    if (sub->imageRect.offset.x < 0 || sub->imageRect.offset.y < 0 ||
        (int64_t)sub->imageRect.offset.x + sub->imageRect.extent.width > r.w ||
        (int64_t)sub->imageRect.offset.y + sub->imageRect.extent.height > r.h)
        return 0;
    if (pool_images[i].copied == 2)  // the whole image is upside down in the atlas: so is the quad's rectangle
        sub->imageRect.offset.y = r.h - sub->imageRect.offset.y - sub->imageRect.extent.height;
    sub->swapchain = pool_atlas[a].handle;
    sub->imageRect.offset.x += r.x;
    sub->imageRect.offset.y += r.y;
    return 1;
}
static int pool_finish_locked(void) {
    int ok = 1;
    // Vulkan: the frame's copies go to the queue before the atlas images are released to the runtime.
    if (pvk.recording && !pool_vk_end_locked()) ok = 0;
    for (int a = 0; a < 2; ++a)
        if (pool_atlas[a].acquired && pool_atlas[a].waited) {
            if (!pool_vulkan) p_glFlush();
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

// xrCreateSession: pool only sessions whose graphics binding a pool path exists for.
// Vulkan sessions also for flip emulation alone (atlas mirrors of flipped quads, no pooled panels).
static void pool_on_create_session(XrSession session, const XrSessionCreateInfo *info) {
    if ((!swapchain_pool && !flip_emulated()) || !info) return;
    pthread_mutex_lock(&pool_lock);
    for (const XrBaseInStructure *p = info->next; p; p = p->next) {
        if (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR && swapchain_pool) {
            pool_session = session;
            pool_vulkan = 0;
            pool_flips = 0;
        }
        if (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {  // = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR
            pool_session = session;
            pool_vulkan = 1;
            pool_flips = flip_emulated();
            pvk.state = 0;  // a new binding: device objects are created on the first pooled panel or flip
            LOG("swapchain_pool: Vulkan session (panel pool %d, flips %d)", swapchain_pool, pool_flips);
        }
    }
    pthread_mutex_unlock(&pool_lock);
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
    pthread_mutex_lock(&pool_lock);  // one lock for the frame: a destroy must not free an image with recorded copies
    uint64_t t0 = pool_ns();
    pool_begin_frame_locked();
    uint32_t n = 0;
    for (uint32_t j = 0; j < count; ++j) {
        const XrCompositionLayerBaseHeader *l = layers[j];
        int ok = 1;
        if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            frame->quads[j] = *(const XrCompositionLayerQuad *)l;
            ok = pool_subimage_locked(&frame->quads[j].subImage,
                                      pool_flips && wants_vertical_flip((const XrCompositionLayerQuad *)l));
            l = (const void *)&frame->quads[j];
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            frame->cylinders[j] = *(const XrCompositionLayerCylinderKHR *)l;
            ok = pool_subimage_locked(&frame->cylinders[j].subImage, 0);
            l = (const void *)&frame->cylinders[j];
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection *p = (const void *)l;
            if (p->viewCount <= 2) {
                frame->projections[j] = *p;
                for (uint32_t v = 0; v < p->viewCount; ++v) {
                    frame->views[j][v] = p->views[v];
                    ok &= pool_subimage_locked(&frame->views[j][v].subImage, 0);
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
    int finished = pool_finish_locked();
    pool_stat(&pool_stats.total_ns, &pool_stats.total_max, pool_ns() - t0);
    if (++pool_stats.frames >= 360) {
        if (layer_debug)
            LOG("swapchain_pool: %.1f copies/frame (%.2f Mpx), in xrEndFrame avg %.2f ms max %.2f ms; ring waits avg "
                "%.2f max %.2f ms; atlas acquire+wait avg %.2f max %.2f ms",
                pool_stats.copies / 360.0, pool_stats.pixels / 360.0 / 1e6, pool_stats.total_ns / 360.0 / 1e6,
                pool_stats.total_max / 1e6, pool_stats.ring_ns / 360.0 / 1e6, pool_stats.ring_max / 1e6,
                pool_stats.atlas_ns / 360.0 / 1e6, pool_stats.atlas_max / 1e6);
        memset(&pool_stats, 0, sizeof pool_stats);
    }
    pthread_mutex_unlock(&pool_lock);
    if (!finished) {
        pool_error();
        return 0;
    }
    return n;
}
