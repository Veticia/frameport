// SPDX-License-Identifier: GPL-3.0-only
// Included by frame_adapter.c. The Vulkan session binding (device, queue, a one-shot command buffer for fade_quad.c
// and cube_standin.c) and the tracking of the app's swapchain images (acquire/release, the shown image) that
// swapchain_pool.c, alpha_blend_vk.c and surface_composite.c use.
// XR_FB_composition_layer_image_layout's VERTICAL_FLIP (ignored by the Frame's runtime) is emulated by
// swapchain_pool.c: a flagged quad shows an upside-down copy of the app's image in a FrameBridge atlas, refreshed every
// frame (flip_source below hands it the image). History: a runtime swapchain per flipped quad ran into the runtime's
// swapchain cap (#138); turning the app's own image upside down in place (1.2.0 - 1.3.x) raced the app's drawing:
// Unity finishes some panels after xrEndFrame without acquiring them again, so that drawing stayed upside down (AC
// Nexus' notices, the "calibrate arms" screen), and every repair (release tracking, fingerprints) stayed timing-bound.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_TIMESPEC
#include <time.h>
#include <openxr/openxr_platform.h>

#define XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB_VALUE 1000040000
#define MAX_TRACKED_SWAPCHAINS 128
#define MAX_IMAGES 8

static int flip_emul = 1;             // setting


static struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family, index;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    int pending;  // vk.cmd was submitted and its fence hadn't signalled yet (vk_cmd_submit)
    int state;  // 0 = not initialized, 1 = ready, -1 = failed
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdClearColorImage CmdClearColorImage;  // fade_quad.c
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateBuffer CreateBuffer;  // flip_dump
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    VkPhysicalDeviceMemoryProperties memory;
} vk;

typedef struct {
    XrSwapchain handle;
    XrSwapchainCreateInfo info;
    VkImage images[MAX_IMAGES];
    uint32_t image_count;
    uint32_t last_acquired;
    uint32_t acquired[MAX_IMAGES], acquired_count, waited_count;
    // images acquired and not yet released, oldest first (xrReleaseSwapchainImage releases the oldest), and the last
    // released one: the image a layer shows. An image still held at xrEndFrame isn't drawn yet.
    uint32_t held[MAX_IMAGES], held_count, last_released;
    int released_any;
    uint32_t alpha_inverted[MAX_IMAGES];  // alpha_blend_vk.c: array layers whose alpha was inverted since the acquire
} tracked_swapchain;

static pthread_mutex_t flip_lock = PTHREAD_MUTEX_INITIALIZER;
static tracked_swapchain tracked[MAX_TRACKED_SWAPCHAINS];

static tracked_swapchain *find_tracked(XrSwapchain handle, int create) {
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; ++i)
        if (tracked[i].handle == handle) return &tracked[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; ++i)
        if (tracked[i].handle == XR_NULL_HANDLE) {
            memset(&tracked[i], 0, sizeof(tracked[i]));
            tracked[i].handle = handle;
            return &tracked[i];
        }
    return NULL;
}

static void flip_on_create_session(const XrSessionCreateInfo *info) {
    for (const XrBaseInStructure *p = info ? (const XrBaseInStructure *)info->next : NULL; p; p = p->next)
        if (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            const XrGraphicsBindingVulkanKHR *b = (const XrGraphicsBindingVulkanKHR *)p;
            vk.instance = b->instance;
            vk.physical = b->physicalDevice;
            vk.device = b->device;
            vk.family = b->queueFamilyIndex;
            vk.index = b->queueIndex;
            vk.state = 0;
            vk.pending = 0;
            LOG("flip: Vulkan session (queue family %u index %u)", vk.family, vk.index);
        }
}

static void flip_on_create_swapchain(XrSwapchain handle, const XrSwapchainCreateInfo *info) {
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(handle, 1);
    if (t) { t->info = *info; t->info.next = NULL; }
    pthread_mutex_unlock(&flip_lock);
}

static void flip_on_enumerate_images(XrSwapchain handle, uint32_t count, const XrSwapchainImageBaseHeader *images) {
    if (!images || !count || images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return;
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(handle, 0);
    if (t) {
        const XrSwapchainImageVulkanKHR *v = (const XrSwapchainImageVulkanKHR *)images;
        t->image_count = count < MAX_IMAGES ? count : MAX_IMAGES;
        for (uint32_t i = 0; i < t->image_count; ++i) t->images[i] = v[i].image;
    }
    pthread_mutex_unlock(&flip_lock);
}

static void flip_on_acquire(XrSwapchain handle, uint32_t index) {
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(handle, 0);
    if (t) {
        t->last_acquired = index;
        if (index < MAX_IMAGES) t->alpha_inverted[index] = 0;
        if(t->acquired_count<MAX_IMAGES)t->acquired[t->acquired_count++]=index;
        if (t->held_count < MAX_IMAGES) t->held[t->held_count++] = index;
    }
    pthread_mutex_unlock(&flip_lock);
}

static int flip_release_tracked;  // a release hook reports releases (flip_on_release), so held images are known

static void flip_on_release(XrSwapchain handle) {
    pthread_mutex_lock(&flip_lock);
    if (!flip_release_tracked) LOG("flip: release tracking active");
    flip_release_tracked = 1;
    tracked_swapchain *t = find_tracked(handle, 0);
    if (t && t->held_count) {
        t->last_released = t->held[0];
        t->released_any = 1;
        memmove(t->held, t->held + 1, (--t->held_count) * sizeof(t->held[0]));
    }
    pthread_mutex_unlock(&flip_lock);
}

// The image a layer of this swapchain shows: the last released one (the last acquired one may still be held and
// undrawn at xrEndFrame). Without release reports (or before the first release) the last acquired one.
static uint32_t shown_image(const tracked_swapchain *t) {
    return flip_release_tracked && t->released_any ? t->last_released : t->last_acquired;
}

static void flip_on_destroy(XrSwapchain handle) {
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(handle, 0);
    if (t) memset(t, 0, sizeof(*t));
    pthread_mutex_unlock(&flip_lock);
}

// xrCreateSwapchain: single-layer colour swapchains of a Vulkan session (quad candidates) also get transfer usage:
// a flipped quad's image is the source of swapchain_pool.c's upside-down copy (destination usage kept as before).
static int flip_wants_transfer(const XrSwapchainCreateInfo *info) {
    return flip_emul && vk.device && !runtime_has_image_layout && info->arraySize == 1 && info->faceCount == 1 &&
           info->sampleCount == 1 && (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
}

static int vk_ready(void) {
    if (vk.state) return vk.state > 0;
    vk.state = -1;
    if (!vk.device) return 0;
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
    if (!gipa) return 0;
    vk.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(vk.instance, "vkGetDeviceProcAddr");
    if (!vk.GetDeviceProcAddr) return 0;
#define DEV(name) vk.name = (PFN_vk##name)vk.GetDeviceProcAddr(vk.device, "vk" #name); if (!vk.name) return 0;
    DEV(GetDeviceQueue) DEV(CreateCommandPool) DEV(AllocateCommandBuffers) DEV(BeginCommandBuffer)
    DEV(EndCommandBuffer) DEV(ResetCommandBuffer) DEV(CmdPipelineBarrier) DEV(CmdBlitImage) DEV(CmdClearColorImage) DEV(QueueSubmit)
    DEV(CreateFence) DEV(WaitForFences) DEV(ResetFences) DEV(CreateImage) DEV(DestroyImage)
    DEV(GetImageMemoryRequirements) DEV(AllocateMemory) DEV(FreeMemory) DEV(BindImageMemory)
    DEV(CreateBuffer) DEV(DestroyBuffer) DEV(GetBufferMemoryRequirements) DEV(BindBufferMemory) DEV(MapMemory)
    DEV(UnmapMemory) DEV(CmdCopyImageToBuffer)
#undef DEV
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(vk.instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!memory_properties) return 0;
    memory_properties(vk.physical, &vk.memory);
    vk.GetDeviceQueue(vk.device, vk.family, vk.index, &vk.queue);
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, vk.family};
    if (vk.CreateCommandPool(vk.device, &pool, NULL, &vk.pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, vk.pool,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vk.AllocateCommandBuffers(vk.device, &alloc, &vk.cmd) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    if (vk.CreateFence(vk.device, &fence, NULL, &vk.fence) != VK_SUCCESS) return 0;
    vk.state = 1;
    LOG("flip: Vulkan blitter ready");
    return 1;
}

static void barrier(VkImage image, uint32_t layer, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src_access, dst_access, from, to,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1}};
    vk.CmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL,
                          0, NULL, 1, &b);
}

static int wants_vertical_flip(const XrCompositionLayerQuad *q) {
    for (const XrBaseInStructure *n = (const XrBaseInStructure *)q->next; n; n = n->next)
        if (n->type == XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB_VALUE)
            return (((const XrCompositionLayerImageLayoutFB *)n)->flags & XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB) != 0;
    return 0;
}

// vk.cmd and vk.fence are free again: waits for a submission that was still running (vk.pending), at most 2 s.
static int vk_cmd_idle(void) {
    if (!vk.pending) return 1;
    if (vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return 0;
    vk.ResetFences(vk.device, 1, &vk.fence);
    vk.pending = 0;
    return 1;
}

// Submits vk.cmd and waits up to `timeout` ns: 0 = not submitted, 1 = done, 2 = still running (it still runs, so it
// counts as done; vk_cmd_idle waits for it before vk.cmd is reused).
static int vk_cmd_submit(uint64_t timeout) {
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &vk.cmd, 0, NULL};
    if (vk.QueueSubmit(vk.queue, 1, &submit, vk.fence) != VK_SUCCESS) return 0;
    if (vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, timeout) == VK_SUCCESS) {
        vk.ResetFences(vk.device, 1, &vk.fence);
        return 1;
    }
    vk.pending = 1;
    return 2;
}

// Vertical flips are emulated in this session (swapchain_pool.c): the setting is on and the runtime lacks the extension.
static int flip_emulated(void) { return flip_emul && !runtime_has_image_layout; }

// The app image a flipped quad of a runtime swapchain shows, with its format and size. 0 (and a log line once per
// reason) when it can't be copied: unknown swapchain, images not enumerated, no transfer-source usage (created before
// the Vulkan session was known), or nothing drawn and released yet. Caller holds pool_lock (order pool -> flip).
static int flip_source(XrSwapchain handle, uint32_t layer, VkImage *image, int64_t *format, uint32_t *width,
                       uint32_t *height) {
    static unsigned logged;
    int why = 0;
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(handle, 0);
    uint32_t index = t ? shown_image(t) : 0;
    if (!t || !t->image_count || index >= t->image_count) why = 1;
    else if (!(t->info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT)) why = 2;
    else if (flip_release_tracked && !t->released_any) why = 3;
    else if (layer >= t->info.arraySize) why = 4;
    if (!why) {
        *image = t->images[index];
        *format = t->info.format;
        *width = t->info.width;
        *height = t->info.height;
    }
    pthread_mutex_unlock(&flip_lock);
    if (why && !(logged & (1u << why))) {
        logged |= 1u << why;
        LOG("flip: quad swapchain=%p left as it is (%s)", (void *)(uintptr_t)handle,
            why == 1 ? "unknown swapchain or images" : why == 2 ? "no transfer usage" :
            why == 3 ? "nothing released yet" : "array layer out of range");
    }
    return !why;
}

// flip_dump=N (diagnostics): what the runtime is given for flipped quads, read back from the atlas region after the
// upside-down copy, without waiting: the copy goes into a host-visible buffer in the same command buffer and is
// written as files/fb_flip_<n>.ppm once that submission's fence has signalled (swapchain_pool.c's ring). Each flipped
// source is sampled (every ~0.5 s, later ~5 s); a file is written when its content changed (empty panels are
// skipped), at most N files. Diagnostics only: hashing and writing files on the render thread cause 45-60 ms hitches.
#define FLIP_DUMP_SLOTS 4
#define FLIP_DUMP_BYTES (16u << 20)
static int flip_dumps;  // files written so far
static struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize used;
    int count;
    struct { XrSwapchain source; uint32_t w, h; VkDeviceSize offset; } item[8];
} fdump[FLIP_DUMP_SLOTS];
static struct { XrSwapchain source; uint32_t hash, frames; } fdump_seen[64];

static int flip_dump_due(XrSwapchain source) {
    if (!flip_dump || flip_dumps >= flip_dump || !*snap_dir) return 0;
    for (int i = 0; i < 64; ++i)
        if (fdump_seen[i].source == source) {  // every ~0.5 s for its first ~30 s shown, then every ~5 s: the
            uint32_t f = fdump_seen[i].frames++;  // readbacks cost frames (69 instead of 72 fps when every 0.5 s)
            return f % (f < 2160 ? 36 : 360) == 0;
        }
    for (int i = 0; i < 64; ++i)
        if (!fdump_seen[i].source) {
            fdump_seen[i].source = source;
            fdump_seen[i].hash = 0;
            fdump_seen[i].frames = 1;
            return 1;
        }
    return 0;
}

// Records the readback of an atlas region (atlas image in TRANSFER_DST_OPTIMAL, and left so).
static void flip_dump_record(VkCommandBuffer cmd, int slot, XrSwapchain source, VkImage atlas, int32_t x, int32_t y,
                             uint32_t w, uint32_t h) {
    if (slot < 0 || slot >= FLIP_DUMP_SLOTS) return;
    __typeof__(fdump[0]) *d = &fdump[slot];
    VkDeviceSize size = (VkDeviceSize)w * h * 4;
    if (d->count >= 8 || d->used + size > FLIP_DUMP_BYTES) return;
    if (!d->buf) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, FLIP_DUMP_BYTES,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (vk.CreateBuffer(vk.device, &bi, NULL, &d->buf) != VK_SUCCESS) { d->buf = VK_NULL_HANDLE; return; }
        VkMemoryRequirements req;
        vk.GetBufferMemoryRequirements(vk.device, d->buf, &req);
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (vk.memory.memoryTypes[i].propertyFlags & want) == want) type = i;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
        if (type == UINT32_MAX || vk.AllocateMemory(vk.device, &ai, NULL, &d->mem) != VK_SUCCESS ||
            vk.BindBufferMemory(vk.device, d->buf, d->mem, 0) != VK_SUCCESS) {
            if (d->mem) vk.FreeMemory(vk.device, d->mem, NULL);
            vk.DestroyBuffer(vk.device, d->buf, NULL);
            d->buf = VK_NULL_HANDLE;
            d->mem = VK_NULL_HANDLE;
            return;
        }
    }
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                              atlas, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy region = {d->used, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {x, y, 0}, {w, h, 1}};
    vk.CmdCopyImageToBuffer(cmd, atlas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->buf, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    d->item[d->count].source = source;
    d->item[d->count].w = w;
    d->item[d->count].h = h;
    d->item[d->count++].offset = d->used;
    d->used += size;
}

// The slot's submission has finished (its fence signalled): writes the changed regions. ok = 0: discards them.
static void flip_dump_collect(int slot, int ok) {
    if (slot < 0 || slot >= FLIP_DUMP_SLOTS || !fdump[slot].count) return;
    __typeof__(fdump[0]) *d = &fdump[slot];
    void *mapped = NULL;
    if (ok && vk.MapMemory(vk.device, d->mem, 0, d->used, 0, &mapped) == VK_SUCCESS) {
        for (int k = 0; k < d->count; ++k) {
            const unsigned char *px = (const unsigned char *)mapped + d->item[k].offset;
            VkDeviceSize n = (VkDeviceSize)d->item[k].w * d->item[k].h * 4;
            uint64_t h = 1469598103934665603ull, nonzero = 0;
            for (VkDeviceSize i = 0; i < n; ++i) { h = (h ^ px[i]) * 1099511628211ull; nonzero |= px[i]; }
            uint32_t hash = (uint32_t)(h ^ (h >> 32));
            int seen = -1;
            for (int i = 0; i < 64; ++i)
                if (fdump_seen[i].source == d->item[k].source) seen = i;
            if (!nonzero || seen < 0 || fdump_seen[seen].hash == hash || flip_dumps >= flip_dump) continue;
            fdump_seen[seen].hash = hash;
            char path[320];
            snprintf(path, sizeof(path), "%s/fb_flip_%d.ppm", snap_dir, ++flip_dumps);
            FILE *f = fopen(path, "wb");
            if (!f) continue;
            fprintf(f, "P6\n%u %u\n255\n", d->item[k].w, d->item[k].h);
            for (VkDeviceSize i = 0; i < n; i += 4) fwrite(px + i, 1, 3, f);
            fclose(f);
            LOG("flip_dump: wrote %s: quad swapchain=%p %ux%u as submitted (atlas copy)", path,
                (void *)(uintptr_t)d->item[k].source, d->item[k].w, d->item[k].h);
        }
        vk.UnmapMemory(vk.device, d->mem);
    }
    d->count = 0;
    d->used = 0;
}

// Session end (swapchain_pool.c, after its submissions finished): the readback buffers belong to that device.
static void flip_dump_free(void) {
    for (int s = 0; s < FLIP_DUMP_SLOTS; ++s) {
        if (fdump[s].buf) vk.DestroyBuffer(vk.device, fdump[s].buf, NULL);
        if (fdump[s].mem) vk.FreeMemory(vk.device, fdump[s].mem, NULL);
    }
    memset(fdump, 0, sizeof fdump);
    memset(fdump_seen, 0, sizeof fdump_seen);
}

