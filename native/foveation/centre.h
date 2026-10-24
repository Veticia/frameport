// SPDX-License-Identifier: GPL-3.0-only
// VK_LAYER_FRAMEPORT_fdm_centre (GitHub #394): puts the full-density area of Valve's foveation on each eye.
//
// Valve's VALVE_fdm_injection layer gives every eye swapchain a fragment density map sized to the WHOLE swapchain
// (ceil(w/32) x ceil(h/32) texels, one array layer per eye) and draws its preset rectangles centred on that map,
// i.e. on the swapchain's centre. The eye's view is the imageRect the game submits, often smaller than the swapchain
// and placed off-centre (dynamic resolution: a corner-anchored rect; asymmetric eyes: rect=0,0 / rect=287,0 inside a
// wider swapchain), and the eye's optical axis isn't the rect's centre either (asymmetric FOV, e.g. -1.03/+0.88 rad).
// So the sharp area sat towards the swapchain centre: off to one side or the bottom right of what the eye sees.
//
// This layer runs BELOW Valve's (inserted right after it in Android's debug-layer list), so it sees the render passes
// with Valve's density map attached and the per-view density-map offsets Valve passes at the end of the pass
// (VK_EXT/QCOM_fragment_density_map_offset: the map is shifted by the offset). It replaces each view's offset with
// one that puts the map's sharp centre on that eye's optical centre (Fixed: no gaze; Valve passes no offsets then and
// this layer adds them) or on the gaze point inside the eye's rect (Eye-tracked: Valve's gaze offset is measured from
// the swapchain centre and scaled by the render area; it is re-based on the rect). FrameBridge reports the views
// (fp_fdm_set_views, every xrEndFrame): swapchain size, array layer, imageRect and the optical centre, all in the
// Vulkan image's pixel rows. Passes are matched to views by framebuffer size and layer, like Valve matches maps.
// Off with FRAMEPORT_FDM_CENTRE=0 (developers: compare against Valve's placement).
#define CENTRE_NAME "VK_LAYER_FRAMEPORT_fdm_centre"
#define FP_FDM_MAX_VIEWS 4

typedef struct {
    uint32_t width, height, layer;   // swapchain image size, array layer
    int32_t x, y, w, h;              // imageRect in Vulkan image rows (top-down)
    float cx, cy;                    // optical centre (straight-ahead direction) in the same pixels
} fp_fdm_view;

static fp_fdm_view c_views[FP_FDM_MAX_VIEWS];
static uint32_t c_view_count;
static int c_lock;
static int c_level = 1;      // Valve's preset: 1 lo, 2 med, 3 hi (FOVE_LEVEL / FDM_DEBUG, as Valve reads them)
static int c_fixed;          // FramePort's Fixed mode (FDM_DEBUG had disable_offsets): ignore Valve's gaze
static int c_disabled;       // FRAMEPORT_FDM_CENTRE=0

EXPORT void fp_fdm_set_views(const fp_fdm_view *views, uint32_t count) {
    if (count > FP_FDM_MAX_VIEWS) count = FP_FDM_MAX_VIEWS;
    take_lock(&c_lock);
    if (views && count) memcpy(c_views, views, count * sizeof(*views));
    c_view_count = views ? count : 0;
    drop_lock(&c_lock);
}

// Valve's full-density rectangle per preset (x, y, w, h in map-normalized coordinates; texel i is sampled at
// i / (texels - 1)), from libVkLayer_VALVE_fdm_injection.so (SteamOS 0.5.x, 2026-10).
static const float c_preset[4][4] = {{0, 0, 0, 0}, {.364f, .367f, .295f, .286f}, {.386f, .388f, .25f, .245f},
                                     {.386f, .404f, .246f, .211f}};

// Where Turnip puts the centre of Valve's sharp texels, in framebuffer pixels: Turnip samples the map with a texel
// size of 2^ceil(log2(framebuffer / map)) pixels (at least 32), see tu_fragment_density_map_sample().
static float c_valve_centre(uint32_t fb, uint32_t texels, int axis) {
    if (!texels) return fb / 2.0f;
    uint32_t ratio = fb / texels, shift = 0;
    while ((1u << shift) < ratio) ++shift;
    if (shift < 5) shift = 5;
    if (shift > 10) shift = 10;
    float lo = c_preset[c_level][axis], hi = lo + c_preset[c_level][axis + 2];
    float n = (float)(texels > 1 ? texels - 1 : 1);
    int first = (int)ceilf(lo * n - 1e-4f), last = (int)floorf(hi * n + 1e-4f);
    if (last < first) last = first;
    return (float)(1u << shift) * ((float)(first + last + 1) / 2.0f);
}

// ---------------------------------------------------------------- object tracking (handles below Valve's layer)

#define C_SLOTS 2048
typedef struct { uint64_t key; uint64_t a, b, c, d; } c_slot;  // a/b/c/d meaning per table
static c_slot c_images[C_SLOTS], c_ivs[C_SLOTS], c_rps[C_SLOTS], c_fbs[C_SLOTS], c_cmds[C_SLOTS];
static int c_obj_lock;
// Not the darkening layer's lock: it holds that one while asking the next layer (Valve's, then this one) for
// functions in vkCreateDevice.
static int c_dev_lock;

static c_slot *c_find(c_slot *t, uint64_t key, int create) {
    if (!key) return NULL;
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 53) & (C_SLOTS - 1);
    c_slot *free_slot = NULL;
    for (uint32_t i = 0; i < C_SLOTS; ++i) {
        c_slot *s = &t[(h + i) & (C_SLOTS - 1)];
        if (s->key == key) return s;
        if (s->key == 0) { if (!free_slot) free_slot = s; break; }
        if (s->key == ~0ull && !free_slot) free_slot = s;
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->key = key;
    return free_slot;
}
static void c_forget(c_slot *t, uint64_t key) {
    take_lock(&c_obj_lock);
    c_slot *s = c_find(t, key, 0);
    if (s) s->key = ~0ull;  // tombstone
    drop_lock(&c_obj_lock);
}
#define H(x) ((uint64_t)(uintptr_t)(x))

typedef void (VKAPI_PTR *PFN_fp_EndRendering2)(VkCommandBuffer, const void *);
typedef struct {
    void *key;
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;
    VkExtent2D gran;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkCreateRenderPass2 CreateRenderPass2, CreateRenderPass2KHR;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdBeginRenderPass2 CmdBeginRenderPass2, CmdBeginRenderPass2KHR;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdEndRenderPass2 CmdEndRenderPass2, CmdEndRenderPass2KHR;
    PFN_vkCmdBeginRendering CmdBeginRendering, CmdBeginRenderingKHR;
    PFN_vkCmdEndRendering CmdEndRendering, CmdEndRenderingKHR;
    PFN_fp_EndRendering2 CmdEndRendering2EXT, CmdEndRendering2KHR;
} c_device;
static struct { void *key; VkInstance instance; PFN_vkGetInstanceProcAddr gipa; } c_instances[MAX_HANDLES];
static c_device c_devices[MAX_HANDLES];

static c_device *c_dev(const void *handle) {
    void *key = key_of(handle);
    c_device *d = NULL;
    take_lock(&c_dev_lock);
    for (int i = 0; i < MAX_HANDLES && !d; i++)
        if (c_devices[i].gdpa && c_devices[i].key == key) d = &c_devices[i];
    drop_lock(&c_dev_lock);
    return d;
}

static const VkBaseInStructure *c_chain(const void *next, VkStructureType type) {
    for (const VkBaseInStructure *b = next; b; b = b->pNext)
        if (b->sType == type) return b;
    return NULL;
}

#define ST_FDM_RP_INFO ((VkStructureType)1000218002)        // VkRenderPassFragmentDensityMapCreateInfoEXT
#define ST_FDM_RENDERING_INFO ((VkStructureType)1000044007) // VkRenderingFragmentDensityMapAttachmentInfoEXT
#define ST_FDM_OFFSET_END ((VkStructureType)1000425002)     // VkRenderPassFragmentDensityMapOffsetEndInfoEXT
#define ST_RENDERING_END ((VkStructureType)1000619003)      // VkRenderingEndInfoKHR
#define ST_RP_ATTACHMENT_BEGIN ((VkStructureType)1000108003)
#define ST_FDM_OFFSET_PROPS ((VkStructureType)1000425001)
#define FDM_USAGE 0x200u
#define FDM_OFFSET_FLAG 0x8000u
typedef struct { VkStructureType sType; const void *pNext; uint32_t count; const VkOffset2D *offsets; } c_offset_end;
typedef struct { VkStructureType sType; const void *pNext; } c_rendering_end;

// Images are never changed here: Valve flags them for offsets itself (all but external-memory ones) when its offsets
// are on. Flagging more (imported Zink/AHardwareBuffer images, as a first version did for Fixed) crashed Turnip.
static VKAPI_ATTR VkResult VKAPI_CALL c_CreateImage(VkDevice device, const VkImageCreateInfo *ci,
                                                    const VkAllocationCallbacks *a, VkImage *image) {
    VkResult r = c_dev(device)->CreateImage(device, ci, a, image);
    if (r == VK_SUCCESS && (ci->usage & FDM_USAGE)) {
        take_lock(&c_obj_lock);
        c_slot *s = c_find(c_images, H(*image), 1);
        if (s) s->a = ci->extent.width, s->b = ci->extent.height, s->c = ci->flags;
        drop_lock(&c_obj_lock);
        static int logged;
        if (logged++ < 4)
            LOG("fdm_centre: density map %ux%u[%u] flags 0x%x", ci->extent.width, ci->extent.height, ci->arrayLayers,
                ci->flags);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL c_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *a) {
    c_forget(c_images, H(image));
    c_dev(device)->DestroyImage(device, image, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL c_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                        const VkAllocationCallbacks *a, VkImageView *view) {
    VkResult r = c_dev(device)->CreateImageView(device, ci, a, view);
    if (r == VK_SUCCESS) {
        take_lock(&c_obj_lock);
        c_slot *img = c_find(c_images, H(ci->image), 0);
        c_slot *s = img ? c_find(c_ivs, H(*view), 1) : NULL;
        // d = first array layer | layer count << 32: Valve gives a one-eye pass the map layer of that eye
        if (s) s->a = img->a, s->b = img->b, s->c = img->c,
               s->d = ci->subresourceRange.baseArrayLayer | (uint64_t)ci->subresourceRange.layerCount << 32;
        drop_lock(&c_obj_lock);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL c_DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *a) {
    c_forget(c_ivs, H(view));
    c_dev(device)->DestroyImageView(device, view, a);
}
// render pass: a = map attachment index, b = the subpasses' view masks ORed (multiview)
static void c_note_rp(const void *next, VkRenderPass rp, uint32_t view_mask) {
    const VkRenderPassFragmentDensityMapCreateInfoEXT *f = (const void *)c_chain(next, ST_FDM_RP_INFO);
    if (!f || f->fragmentDensityMapAttachment.attachment == VK_ATTACHMENT_UNUSED) return;
    take_lock(&c_obj_lock);
    c_slot *s = c_find(c_rps, H(rp), 1);
    if (s) s->a = f->fragmentDensityMapAttachment.attachment, s->b = view_mask;
    drop_lock(&c_obj_lock);
}
static uint32_t c_masks2(const VkRenderPassCreateInfo2 *ci) {
    uint32_t m = 0;
    for (uint32_t i = 0; ci->pSubpasses && i < ci->subpassCount; ++i) m |= ci->pSubpasses[i].viewMask;
    return m;
}
static uint32_t c_masks1(const VkRenderPassCreateInfo *ci) {
    const VkRenderPassMultiviewCreateInfo *mv = (const void *)c_chain(ci->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO);
    uint32_t m = 0;
    for (uint32_t i = 0; mv && mv->pViewMasks && i < mv->subpassCount; ++i) m |= mv->pViewMasks[i];
    return m;
}
static VKAPI_ATTR VkResult VKAPI_CALL c_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci,
                                                         const VkAllocationCallbacks *a, VkRenderPass *rp) {
    VkResult r = c_dev(device)->CreateRenderPass(device, ci, a, rp);
    if (r == VK_SUCCESS) c_note_rp(ci->pNext, *rp, c_masks1(ci));
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL c_CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *ci,
                                                          const VkAllocationCallbacks *a, VkRenderPass *rp) {
    VkResult r = c_dev(device)->CreateRenderPass2(device, ci, a, rp);
    if (r == VK_SUCCESS) c_note_rp(ci->pNext, *rp, c_masks2(ci));
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL c_CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2 *ci,
                                                             const VkAllocationCallbacks *a, VkRenderPass *rp) {
    VkResult r = c_dev(device)->CreateRenderPass2KHR(device, ci, a, rp);
    if (r == VK_SUCCESS) c_note_rp(ci->pNext, *rp, c_masks2(ci));
    return r;
}
static VKAPI_ATTR void VKAPI_CALL c_DestroyRenderPass(VkDevice device, VkRenderPass rp, const VkAllocationCallbacks *a) {
    c_forget(c_rps, H(rp));
    c_dev(device)->DestroyRenderPass(device, rp, a);
}
// framebuffer: a = width | height << 32, b = density map view (0: imageless), c = map attachment index + 1,
// d = layers | render pass view mask << 32
static VKAPI_ATTR VkResult VKAPI_CALL c_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *ci,
                                                          const VkAllocationCallbacks *a, VkFramebuffer *fb) {
    VkResult r = c_dev(device)->CreateFramebuffer(device, ci, a, fb);
    if (r == VK_SUCCESS) {
        take_lock(&c_obj_lock);
        c_slot *rp = c_find(c_rps, H(ci->renderPass), 0);
        if (rp) {
            c_slot *s = c_find(c_fbs, H(*fb), 1);
            if (s) {
                s->a = ci->width | (uint64_t)ci->height << 32;
                s->b = (!(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->pAttachments &&
                        rp->a < ci->attachmentCount) ? H(ci->pAttachments[rp->a]) : 0;
                s->c = rp->a + 1;
                s->d = ci->layers | rp->b << 32;
            }
        }
        drop_lock(&c_obj_lock);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL c_DestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks *a) {
    c_forget(c_fbs, H(fb));
    c_dev(device)->DestroyFramebuffer(device, fb, a);
}

// The number of density-map offsets Turnip reads at the end of a pass, whatever count it is given
// (tu_fdm_num_layers): the multiview view count, else the framebuffer's layers (per-layer maps) or 1.
static uint32_t c_pass_views(uint32_t view_mask, uint32_t layers) {
    if (view_mask) {
        uint32_t n = 0;
        while (view_mask >> n) ++n;
        return n;
    }
    return layers ? layers : 1;
}

// command buffer state: a = framebuffer width | height << 32, b = map width | height << 32 (0: no map), c = map flags,
// d = offsets Turnip reads | first map layer << 16 | map view's layer count << 32
static void c_begin(VkCommandBuffer cmd, uint32_t fbw, uint32_t fbh, VkImageView map, uint32_t views) {
    take_lock(&c_obj_lock);
    c_slot *iv = map ? c_find(c_ivs, H(map), 0) : NULL;
    c_slot *s = c_find(c_cmds, H(cmd), iv != NULL);
    if (s) {
        s->a = fbw | (uint64_t)fbh << 32;
        s->b = iv ? (iv->a | iv->b << 32) : 0;
        s->c = iv ? iv->c : 0;
        s->d = iv ? ((views & 0xffff) | (iv->d & 0xffff) << 16 | (iv->d >> 32) << 32) : 0;
    }
    drop_lock(&c_obj_lock);
}
static void c_begin_rp(VkCommandBuffer cmd, const VkRenderPassBeginInfo *info) {
    if (c_disabled || !info) return;
    take_lock(&c_obj_lock);
    c_slot *fb = c_find(c_fbs, H(info->framebuffer), 0);
    uint64_t size = fb ? fb->a : 0, view = fb ? fb->b : 0, index = fb ? fb->c : 0, lm = fb ? fb->d : 0;
    drop_lock(&c_obj_lock);
    if (fb && !view && index) {  // imageless framebuffer: the views come with the begin
        const VkRenderPassAttachmentBeginInfo *ab = (const void *)c_chain(info->pNext, ST_RP_ATTACHMENT_BEGIN);
        if (ab && index - 1 < ab->attachmentCount) view = H(ab->pAttachments[index - 1]);
    }
    c_begin(cmd, (uint32_t)size, (uint32_t)(size >> 32), (VkImageView)(uintptr_t)view,
            c_pass_views((uint32_t)(lm >> 32), (uint32_t)lm));
}
static VKAPI_ATTR void VKAPI_CALL c_CmdBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo *info,
                                                       VkSubpassContents contents) {
    c_begin_rp(cmd, info);
    c_dev(cmd)->CmdBeginRenderPass(cmd, info, contents);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdBeginRenderPass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo *info,
                                                        const VkSubpassBeginInfo *sub) {
    c_begin_rp(cmd, info);
    c_dev(cmd)->CmdBeginRenderPass2(cmd, info, sub);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdBeginRenderPass2KHR(VkCommandBuffer cmd, const VkRenderPassBeginInfo *info,
                                                           const VkSubpassBeginInfo *sub) {
    c_begin_rp(cmd, info);
    c_dev(cmd)->CmdBeginRenderPass2KHR(cmd, info, sub);
}
static void c_begin_rendering(VkCommandBuffer cmd, const VkRenderingInfo *info) {
    if (c_disabled || !info) return;
    const VkRenderingFragmentDensityMapAttachmentInfoEXT *f = (const void *)c_chain(info->pNext, ST_FDM_RENDERING_INFO);
    // Turnip: a dynamic-rendering framebuffer ends where the render area ends
    c_begin(cmd, (uint32_t)(info->renderArea.offset.x + (int32_t)info->renderArea.extent.width),
            (uint32_t)(info->renderArea.offset.y + (int32_t)info->renderArea.extent.height), f ? f->imageView : VK_NULL_HANDLE,
            c_pass_views(info->viewMask, info->layerCount));
}
static VKAPI_ATTR void VKAPI_CALL c_CmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo *info) {
    c_begin_rendering(cmd, info);
    c_dev(cmd)->CmdBeginRendering(cmd, info);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdBeginRenderingKHR(VkCommandBuffer cmd, const VkRenderingInfo *info) {
    c_begin_rendering(cmd, info);
    c_dev(cmd)->CmdBeginRenderingKHR(cmd, info);
}

static int c_round(float v, uint32_t gran) {
    if (!gran) gran = 1;
    return (int)lroundf(v / (float)gran) * (int)gran;
}

// The density-map offsets this pass ends with: in = Valve's array (NULL: none). Writes the n offsets Turnip will read
// to out and returns n (0: leave the pass as it is). Turnip reads one offset per view (or per-layer map layer) whatever
// fragmentDensityOffsetCount says. Valve's layer computes one per view into its array but declares count 1 for a
// two-view pass, so with Valve alone Turnip reads the right eye's offset beyond the count: in[v] is read for every
// view here too (the same memory the driver reads), and every entry handed down is written.
static uint32_t c_offsets(VkCommandBuffer cmd, c_device *d, uint32_t in_count, const VkOffset2D *in, VkOffset2D *out) {
    take_lock(&c_obj_lock);
    c_slot *s = c_find(c_cmds, H(cmd), 0);
    uint64_t size = s ? s->a : 0, map = s ? s->b : 0, flags = s ? s->c : 0, shape = s ? s->d : 0;
    if (s) s->key = ~0ull;  // one pass at a time per command buffer
    drop_lock(&c_obj_lock);
    // A map Valve made without the offset flag (its offsets off): Turnip ignores offsets, change nothing.
    if (!map || !(flags & FDM_OFFSET_FLAG)) return 0;
    uint32_t n = (uint32_t)(shape & 0xffff), first = (uint32_t)((shape >> 16) & 0xffff);
    uint32_t map_layers = (uint32_t)(shape >> 32);
    if (!n || n > FP_FDM_MAX_VIEWS) return 0;
    uint32_t fbw = (uint32_t)size, fbh = (uint32_t)(size >> 32), mw = (uint32_t)map, mh = (uint32_t)(map >> 32);
    fp_fdm_view views[FP_FDM_MAX_VIEWS];
    take_lock(&c_lock);
    uint32_t nviews = c_view_count;
    memcpy(views, c_views, sizeof(views));
    drop_lock(&c_lock);
    float vx = c_valve_centre(fbw, mw, 0), vy = c_valve_centre(fbh, mh, 1);
    if (!in_count) in = NULL;  // no offsets from Valve at all
    // Valve's gaze (Eye-tracked); all zero when it has none (no eye tracking): straight ahead then
    int gaze = 0;
    for (uint32_t i = 0; !c_fixed && in && i < n; ++i) gaze |= in[i].x || in[i].y;
    int matched = 0;
    for (uint32_t v = 0; v < n; ++v) {
        VkOffset2D g = in ? in[v] : (VkOffset2D){0, 0};  // that eye's gaze offset
        out[v] = g;
        // the eye: the map layer this offset applies to (a one-eye pass gets the one-layer map view of its eye)
        uint32_t layer = first + (map_layers > 1 ? v : 0);
        const fp_fdm_view *e = NULL;
        int candidates = 0;
        for (uint32_t i = 0; i < nviews; ++i)
            if (views[i].width == fbw && views[i].height == fbh && views[i].layer == layer) e = &views[i], ++candidates;
        if (candidates != 1) continue;  // e.g. both eyes side by side in one layer: one map can't serve both
        float dx = e->cx, dy = e->cy;
        if (gaze) {
            // Valve: gaze NDC * render-area half size from the swapchain centre; the gaze is in the eye's view
            dx = (float)e->x + (float)e->w / 2.0f + (float)g.x * (float)e->w / (float)fbw;
            dy = (float)e->y + (float)e->h / 2.0f + (float)g.y * (float)e->h / (float)fbh;
        }
        out[v].x = c_round(dx - vx, d->gran.width);
        out[v].y = c_round(dy - vy, d->gran.height);
        ++matched;
    }
    static int logged;
    if (matched && logged++ < 8)
        LOG("fdm_centre: pass %ux%u map %ux%u layer %u+%u (preset %d, Valve centre %.0f,%.0f, %s): %u view(s), %u "
            "offset(s) from Valve %d,%d -> %d,%d%s", fbw, fbh, mw, mh, first, map_layers, c_level, vx, vy,
            c_fixed ? "Fixed" : gaze ? "gaze" : "no gaze", n, in_count, in_count ? in[0].x : 0, in_count ? in[0].y : 0,
            out[0].x, out[0].y, n > 1 ? " (+ more views)" : "");
    return matched ? n : 0;
}

static VKAPI_ATTR void VKAPI_CALL c_end_rp2(VkCommandBuffer cmd, const VkSubpassEndInfo *info, int khr) {
    c_device *d = c_dev(cmd);
    PFN_vkCmdEndRenderPass2 next = khr && d->CmdEndRenderPass2KHR ? d->CmdEndRenderPass2KHR : d->CmdEndRenderPass2;
    const c_offset_end *valve = info ? (const void *)c_chain(info->pNext, ST_FDM_OFFSET_END) : NULL;
    VkOffset2D offsets[FP_FDM_MAX_VIEWS];
    uint32_t n = c_disabled ? 0 : c_offsets(cmd, d, valve ? valve->count : 0, valve ? valve->offsets : NULL, offsets);
    if (!n || (valve && info->pNext != (const void *)valve)) {  // nothing to change, or not first in the chain
        next(cmd, info);
        return;
    }
    c_offset_end mine = {ST_FDM_OFFSET_END, valve ? valve->pNext : info->pNext, n, offsets};
    VkSubpassEndInfo copy = *info;
    copy.pNext = &mine;
    next(cmd, &copy);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdEndRenderPass2(VkCommandBuffer cmd, const VkSubpassEndInfo *info) {
    c_end_rp2(cmd, info, 0);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdEndRenderPass2KHR(VkCommandBuffer cmd, const VkSubpassEndInfo *info) {
    c_end_rp2(cmd, info, 1);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdEndRenderPass(VkCommandBuffer cmd) {
    c_device *d = c_dev(cmd);
    VkOffset2D offsets[FP_FDM_MAX_VIEWS];
    uint32_t n = c_disabled || !d->CmdEndRenderPass2 ? 0 : c_offsets(cmd, d, 0, NULL, offsets);
    if (!n) {
        d->CmdEndRenderPass(cmd);
        return;
    }
    c_offset_end mine = {ST_FDM_OFFSET_END, NULL, n, offsets};
    VkSubpassEndInfo info = {VK_STRUCTURE_TYPE_SUBPASS_END_INFO, &mine};
    d->CmdEndRenderPass2(cmd, &info);
}
static void c_end_rendering2(VkCommandBuffer cmd, const c_rendering_end *info, PFN_fp_EndRendering2 next) {
    c_device *d = c_dev(cmd);
    const c_offset_end *valve = info ? (const void *)c_chain(info->pNext, ST_FDM_OFFSET_END) : NULL;
    VkOffset2D offsets[FP_FDM_MAX_VIEWS];
    uint32_t n = c_disabled ? 0 : c_offsets(cmd, d, valve ? valve->count : 0, valve ? valve->offsets : NULL, offsets);
    if (!n || (valve && info->pNext != (const void *)valve)) {
        next(cmd, info);
        return;
    }
    c_offset_end mine = {ST_FDM_OFFSET_END, valve ? valve->pNext : (info ? info->pNext : NULL), n, offsets};
    c_rendering_end copy = {ST_RENDERING_END, &mine};
    next(cmd, &copy);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdEndRendering2EXT(VkCommandBuffer cmd, const void *info) {
    c_end_rendering2(cmd, info, c_dev(cmd)->CmdEndRendering2EXT);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdEndRendering2KHR(VkCommandBuffer cmd, const void *info) {
    c_end_rendering2(cmd, info, c_dev(cmd)->CmdEndRendering2KHR);
}
static void c_end_rendering(VkCommandBuffer cmd, PFN_vkCmdEndRendering plain) {
    c_device *d = c_dev(cmd);
    PFN_fp_EndRendering2 next = d->CmdEndRendering2EXT ? d->CmdEndRendering2EXT : d->CmdEndRendering2KHR;
    VkOffset2D offsets[FP_FDM_MAX_VIEWS];
    uint32_t n = c_disabled || !next ? 0 : c_offsets(cmd, d, 0, NULL, offsets);
    if (!n) {
        plain(cmd);
        return;
    }
    c_offset_end mine = {ST_FDM_OFFSET_END, NULL, n, offsets};
    c_rendering_end info = {ST_RENDERING_END, &mine};
    next(cmd, &info);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdEndRendering(VkCommandBuffer cmd) {
    c_end_rendering(cmd, c_dev(cmd)->CmdEndRendering);
}
static VKAPI_ATTR void VKAPI_CALL c_CmdEndRenderingKHR(VkCommandBuffer cmd) {
    c_end_rendering(cmd, c_dev(cmd)->CmdEndRenderingKHR);
}

// ---------------------------------------------------------------- layer plumbing

static PFN_vkGetInstanceProcAddr c_instance_gipa(const void *handle, VkInstance *instance) {
    void *key = key_of(handle);
    PFN_vkGetInstanceProcAddr found = NULL, last = NULL;
    take_lock(&c_dev_lock);
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!c_instances[i].gipa) continue;
        last = c_instances[i].gipa;
        if (instance) *instance = c_instances[i].instance;
        if (c_instances[i].key == key) { found = c_instances[i].gipa; break; }
    }
    drop_lock(&c_dev_lock);
    return found ? found : last;
}

static VKAPI_ATTR VkResult VKAPI_CALL c_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
                                                       VkInstance *instance) {
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)next(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(ci, a, instance);
    if (r != VK_SUCCESS) return r;
    take_lock(&c_dev_lock);
    int slot = 0;
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!c_instances[i].gipa || c_instances[i].key == key_of(*instance)) { slot = i; break; }
    c_instances[slot].key = key_of(*instance);
    c_instances[slot].instance = *instance;
    c_instances[slot].gipa = next;
    drop_lock(&c_dev_lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL c_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                     const VkAllocationCallbacks *a, VkDevice *device) {
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkInstance instance = VK_NULL_HANDLE;
    c_instance_gipa(pd, &instance);
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)next_gipa(instance, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(pd, ci, a, device);
    if (r != VK_SUCCESS) return r;
    c_device d;
    memset(&d, 0, sizeof(d));
    d.key = key_of(*device), d.device = *device, d.gdpa = next_gdpa, d.gran = (VkExtent2D){1, 1};
#define C_GET(field, name) d.field = (__typeof__(d.field))next_gdpa(*device, name);
    C_GET(CreateImage, "vkCreateImage") C_GET(DestroyImage, "vkDestroyImage")
    C_GET(CreateImageView, "vkCreateImageView") C_GET(DestroyImageView, "vkDestroyImageView")
    C_GET(CreateRenderPass, "vkCreateRenderPass") C_GET(CreateRenderPass2, "vkCreateRenderPass2")
    C_GET(CreateRenderPass2KHR, "vkCreateRenderPass2KHR") C_GET(DestroyRenderPass, "vkDestroyRenderPass")
    C_GET(CreateFramebuffer, "vkCreateFramebuffer") C_GET(DestroyFramebuffer, "vkDestroyFramebuffer")
    C_GET(CmdBeginRenderPass, "vkCmdBeginRenderPass") C_GET(CmdBeginRenderPass2, "vkCmdBeginRenderPass2")
    C_GET(CmdBeginRenderPass2KHR, "vkCmdBeginRenderPass2KHR") C_GET(CmdEndRenderPass, "vkCmdEndRenderPass")
    C_GET(CmdEndRenderPass2, "vkCmdEndRenderPass2") C_GET(CmdEndRenderPass2KHR, "vkCmdEndRenderPass2KHR")
    C_GET(CmdBeginRendering, "vkCmdBeginRendering") C_GET(CmdBeginRenderingKHR, "vkCmdBeginRenderingKHR")
    C_GET(CmdEndRendering, "vkCmdEndRendering") C_GET(CmdEndRenderingKHR, "vkCmdEndRenderingKHR")
    C_GET(CmdEndRendering2EXT, "vkCmdEndRendering2EXT") C_GET(CmdEndRendering2KHR, "vkCmdEndRendering2KHR")
#undef C_GET
    PFN_vkGetPhysicalDeviceProperties2 props2 =
        (PFN_vkGetPhysicalDeviceProperties2)next_gipa(instance, "vkGetPhysicalDeviceProperties2");
    struct { VkStructureType sType; void *pNext; VkExtent2D gran; } offset_props = {ST_FDM_OFFSET_PROPS, NULL, {0, 0}};
    VkPhysicalDeviceProperties2 p2;
    memset(&p2, 0, sizeof(p2));
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, p2.pNext = &offset_props;
    if (props2) props2(pd, &p2);
    if (offset_props.gran.width && offset_props.gran.height) d.gran = offset_props.gran;
    take_lock(&c_dev_lock);
    int slot = 0;
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!c_devices[i].gdpa || c_devices[i].key == key_of(*device)) { slot = i; break; }
    c_devices[slot] = d;
    drop_lock(&c_dev_lock);
    static int logged;
    if (logged++ < 4)
        LOG("fdm_centre: active on device %p (preset %d, %s, granularity %ux%u)", (void *)*device, c_level,
            c_fixed ? "Fixed: optical centre" : "gaze", d.gran.width, d.gran.height);
    return r;
}

static PFN_vkVoidFunction c_own(const char *name, const c_device *d);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL c_GetDeviceProcAddr(VkDevice device, const char *name) {
    if (name && !strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)c_GetDeviceProcAddr;
    c_device *d = c_dev(device);
    PFN_vkVoidFunction fn = c_own(name, d);
    if (fn) return fn;
    return d ? d->gdpa(device, name) : NULL;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL c_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (name && !strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)c_CreateInstance;
    if (name && !strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)c_CreateDevice;
    if (name && !strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)c_GetInstanceProcAddr;
    if (name && !strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)c_GetDeviceProcAddr;
    if (name && !strncmp(name, "vkEnumerate", 11)) {
        PFN_vkVoidFunction fn = own(name);  // the library's layer enumeration
        if (fn) return fn;
    }
    PFN_vkVoidFunction fn = c_own(name, NULL);
    if (fn) return fn;
    PFN_vkGetInstanceProcAddr next = instance ? c_instance_gipa(instance, NULL) : NULL;
    return next ? next(instance, name) : NULL;
}

// Device functions are only ours where the next layer has them (a NULL stays NULL for the app).
static PFN_vkVoidFunction c_own(const char *name, const c_device *d) {
    if (!name) return NULL;
#define C_OWN(fn, impl, field) if (!strcmp(name, fn)) return (!d || d->field) ? (PFN_vkVoidFunction)impl : NULL;
    C_OWN("vkCreateImage", c_CreateImage, CreateImage) C_OWN("vkDestroyImage", c_DestroyImage, DestroyImage)
    C_OWN("vkCreateImageView", c_CreateImageView, CreateImageView)
    C_OWN("vkDestroyImageView", c_DestroyImageView, DestroyImageView)
    C_OWN("vkCreateRenderPass", c_CreateRenderPass, CreateRenderPass)
    C_OWN("vkCreateRenderPass2", c_CreateRenderPass2, CreateRenderPass2)
    C_OWN("vkCreateRenderPass2KHR", c_CreateRenderPass2KHR, CreateRenderPass2KHR)
    C_OWN("vkDestroyRenderPass", c_DestroyRenderPass, DestroyRenderPass)
    C_OWN("vkCreateFramebuffer", c_CreateFramebuffer, CreateFramebuffer)
    C_OWN("vkDestroyFramebuffer", c_DestroyFramebuffer, DestroyFramebuffer)
    C_OWN("vkCmdBeginRenderPass", c_CmdBeginRenderPass, CmdBeginRenderPass)
    C_OWN("vkCmdBeginRenderPass2", c_CmdBeginRenderPass2, CmdBeginRenderPass2)
    C_OWN("vkCmdBeginRenderPass2KHR", c_CmdBeginRenderPass2KHR, CmdBeginRenderPass2KHR)
    C_OWN("vkCmdEndRenderPass", c_CmdEndRenderPass, CmdEndRenderPass)
    C_OWN("vkCmdEndRenderPass2", c_CmdEndRenderPass2, CmdEndRenderPass2)
    C_OWN("vkCmdEndRenderPass2KHR", c_CmdEndRenderPass2KHR, CmdEndRenderPass2KHR)
    C_OWN("vkCmdBeginRendering", c_CmdBeginRendering, CmdBeginRendering)
    C_OWN("vkCmdBeginRenderingKHR", c_CmdBeginRenderingKHR, CmdBeginRenderingKHR)
    C_OWN("vkCmdEndRendering", c_CmdEndRendering, CmdEndRendering)
    C_OWN("vkCmdEndRenderingKHR", c_CmdEndRenderingKHR, CmdEndRenderingKHR)
    C_OWN("vkCmdEndRendering2EXT", c_CmdEndRendering2EXT, CmdEndRendering2EXT)
    C_OWN("vkCmdEndRendering2KHR", c_CmdEndRendering2KHR, CmdEndRendering2KHR)
#undef C_OWN
    return NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_FRAMEPORT_fdm_centreGetInstanceProcAddr(VkInstance i, const char *n) {
    return c_GetInstanceProcAddr(i, n);
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_FRAMEPORT_fdm_centreGetDeviceProcAddr(VkDevice d, const char *n) {
    return c_GetDeviceProcAddr(d, n);
}

// Valve's preset and offset choice, read the way its layer reads them (FOVE_LEVEL wins over FDM_DEBUG lo/med/hi).
static int c_has_token(const char *list, const char *token) {
    size_t n = strlen(token);
    for (const char *p = list; p && *p;) {
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) ++p;
        const char *start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') ++p;
        if ((size_t)(p - start) == n && !strncmp(start, token, n)) return 1;
    }
    return 0;
}
static void c_read_config(void) {
    const char *flags = getenv("FDM_DEBUG"), *level = getenv("FOVE_LEVEL"), *off = getenv("FRAMEPORT_FDM_CENTRE");
    c_fixed = c_has_token(flags, "disable_offsets");
    c_level = c_has_token(flags, "hi") ? 3 : c_has_token(flags, "med") ? 2 : 1;
    int explicit = c_has_token(level, "hi") ? 3 : c_has_token(level, "med") ? 2 : c_has_token(level, "lo") ? 1 : 0;
    if (explicit) c_level = explicit;
    c_disabled = off && !strcmp(off, "0");
}

// Fixed mode: let Valve run with its offsets on (it then flags the images it should, and passes per-view offsets
// this layer overrides with the optical centre) instead of disable_offsets, which leaves maps without the offset flag
// so no offset can move them. Done in this process before Valve's layer reads FDM_DEBUG (Mesa caches options at the
// first read); if it already read it, its maps carry no offset flag and every pass is left alone (Valve's Fixed).
static void c_fixed_with_offsets(void) {
    const char *flags = getenv("FDM_DEBUG");
    if (!c_fixed || !flags || strlen(flags) >= 512) return;
    char out[512] = "";
    for (const char *p = flags; *p;) {
        const char *start = p;
        while (*p && *p != ',') ++p;
        size_t n = (size_t)(p - start);
        if (n && !(n == 15 && !strncmp(start, "disable_offsets", 15)))
            snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%.*s", *out ? "," : "", (int)n, start);
        if (*p) ++p;
    }
    setenv("FDM_DEBUG", out, 1);
    LOG("fdm_centre: Fixed: FDM_DEBUG \"%s\" -> \"%s\" (Valve's offsets on, overridden with each eye's centre)", flags,
        out);
}
