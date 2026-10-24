// FramePort FrameBridge: eye-image snapshots of Vulkan games (setting snapshot=<seconds>; snapshot_gl.c has the GLES
// half and the timing). At xrEndFrame both views of the first projection layer are read back from the image the
// layer shows (its whole swapchain array layer, not only the imageRect) and written at half size as
// files/fb_snap_vk_<n>_v<view>.ppm (n = 0-7); the log line gives the imageRect and FOV so the submitted rect and the
// eye's optical centre can be drawn over it. With "Darken coarse fragments (test)" this shows where Valve's foveation
// rendered at full density (GitHub #394). Diagnostics only: the copy waits for the GPU on the render thread, and
// projection swapchains get transfer-source usage while snapshots are on.
#define SNAP_VK_SCALE 2

static int snap_vk_wants_transfer(const XrSwapchainCreateInfo *info) {
    return snapshot && vk.device && info && info->faceCount == 1 && info->sampleCount == 1 &&
           (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
}

static void snap_vk_write(const unsigned char *px, uint32_t w, uint32_t h, int bgra, int n, uint32_t view) {
    char path[320];
    snprintf(path, sizeof(path), "%s/fb_snap_vk_%d_v%u.ppm", snap_dir, n, view);
    FILE *f = fopen(path, "wb");
    if (!f) { LOG("snapshot: can't write %s", path); return; }
    uint32_t ow = w / SNAP_VK_SCALE, oh = h / SNAP_VK_SCALE;
    fprintf(f, "P6\n%u %u\n255\n", ow, oh);
    unsigned char *row = malloc((size_t)ow * 3);
    for (uint32_t y = 0; row && y < oh; ++y) {
        const unsigned char *src = px + (size_t)y * SNAP_VK_SCALE * w * 4;  // Vulkan rows are top-down
        for (uint32_t x = 0; x < ow; ++x) {
            const unsigned char *p = src + (size_t)x * SNAP_VK_SCALE * 4;
            row[x * 3] = p[bgra ? 2 : 0], row[x * 3 + 1] = p[1], row[x * 3 + 2] = p[bgra ? 0 : 2];
        }
        fwrite(row, 1, (size_t)ow * 3, f);
    }
    free(row);
    fclose(f);
}

static void snap_vk_end_frame(const XrFrameEndInfo *info, int n) {
    const XrCompositionLayerProjection *proj = NULL;
    for (uint32_t i = 0; i < info->layerCount && !proj; ++i)
        if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION &&
            ((const XrCompositionLayerProjection *)info->layers[i])->viewCount)
            proj = (const XrCompositionLayerProjection *)info->layers[i];
    if (!proj || !vk_ready() || !vk_cmd_idle()) return;
    uint32_t views = proj->viewCount < 2 ? proj->viewCount : 2;
    for (uint32_t v = 0; v < views; ++v) {
        const XrCompositionLayerProjectionView *pv = &proj->views[v];
        const XrSwapchainSubImage *sub = &pv->subImage;
        VkImage image = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0;
        int64_t format = 0;
        int why = 0;
        pthread_mutex_lock(&flip_lock);
        tracked_swapchain *t = find_tracked(sub->swapchain, 0);
        uint32_t index = t ? shown_image(t) : 0;
        if (!t || !t->image_count || index >= t->image_count) why = 1;
        else if (!(t->info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT)) why = 2;
        else if (sub->imageArrayIndex >= t->info.arraySize) why = 3;
        else image = t->images[index], w = t->info.width, h = t->info.height, format = t->info.format;
        pthread_mutex_unlock(&flip_lock);
        int bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
        if (!why && !bgra && format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM) why = 4;
        if (why) {
            static unsigned logged;
            if (!(logged & (1u << why))) {
                logged |= 1u << why;
                LOG("snapshot: Vulkan view %u not readable (%s)", v, why == 1 ? "unknown swapchain or images" :
                    why == 2 ? "no transfer usage" : why == 3 ? "array layer out of range" : "not an 8-bit RGBA format");
            }
            continue;
        }
        VkDeviceSize size = (VkDeviceSize)w * h * 4;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (vk.CreateBuffer(vk.device, &bi, NULL, &buf) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vk.GetBufferMemoryRequirements(vk.device, buf, &req);
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (vk.memory.memoryTypes[i].propertyFlags & want) == want) type = i;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
        if (type == UINT32_MAX || vk.AllocateMemory(vk.device, &ai, NULL, &mem) != VK_SUCCESS ||
            vk.BindBufferMemory(vk.device, buf, mem, 0) != VK_SUCCESS) {
            if (mem) vk.FreeMemory(vk.device, mem, NULL);
            vk.DestroyBuffer(vk.device, buf, NULL);
            return;
        }
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                          VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        int ok = vk.ResetCommandBuffer(vk.cmd, 0) == VK_SUCCESS && vk.BeginCommandBuffer(vk.cmd, &begin) == VK_SUCCESS;
        if (ok) {
            // released swapchain images are in COLOR_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable)
            barrier(image, sub->imageArrayIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, sub->imageArrayIndex, 1}, {0, 0, 0},
                                        {w, h, 1}};
            vk.CmdCopyImageToBuffer(vk.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
            barrier(image, sub->imageArrayIndex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            ok = vk.EndCommandBuffer(vk.cmd) == VK_SUCCESS && vk_cmd_submit(2000000000ull) == 1;
        }
        void *mapped = NULL;
        if (ok && vk.MapMemory(vk.device, mem, 0, size, 0, &mapped) == VK_SUCCESS) {
            snap_vk_write(mapped, w, h, bgra, n, v);
            vk.UnmapMemory(vk.device, mem);
            const XrRect2Di *r = &sub->imageRect;
            LOG("snapshot: vk view %u -> fb_snap_vk_%d_v%u.ppm (1/%d) image %ux%u array=%u rect=%d,%d %dx%d "
                "fov=%.4f/%.4f/%.4f/%.4f", v, n, v, SNAP_VK_SCALE, w, h, sub->imageArrayIndex, r->offset.x,
                r->offset.y, r->extent.width, r->extent.height, pv->fov.angleLeft, pv->fov.angleRight,
                pv->fov.angleUp, pv->fov.angleDown);
        } else {
            static int logged;
            if (logged++ < 3) LOG("snapshot: Vulkan readback of view %u failed", v);
        }
        if (vk.pending && !vk_cmd_idle()) return;  // still running: leak the buffer rather than free it under the GPU
        vk.FreeMemory(vk.device, mem, NULL);
        vk.DestroyBuffer(vk.device, buf, NULL);
    }
}
