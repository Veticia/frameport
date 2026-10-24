// FramePort FrameBridge: tells FramePort's foveation centring layer (native/foveation/centre.h, in
// libVkLayer_fdm_darken.so, which this library links on arm64) where each eye's view is (GitHub #394). Every
// xrEndFrame: the first projection layer's views as swapchain size, array layer, imageRect and the eye's optical
// centre (the straight-ahead direction from the view's FOV angles; not the rect's centre when the FOV is
// asymmetric), in the rows of the Vulkan image the density map covers (GLES through Zink: GL's bottom row is the
// image's first row). Sent only when something changed.
#include <stddef.h>

typedef struct {
    uint32_t width, height, layer;
    int32_t x, y, w, h;
    float cx, cy;
} fdm_view_info;

// One eye's view; 0 when the swapchain or the FOV is unusable.
static int fdm_view_of(const XrCompositionLayerProjectionView *pv, int gles, fdm_view_info *o) {
    uint32_t w = 0, h = 0;
    if (!swapchain_size(pv->subImage.swapchain, &w, &h)) return 0;
    const XrRect2Di *r = &pv->subImage.imageRect;
    float l = tanf(pv->fov.angleLeft), rt = tanf(pv->fov.angleRight);
    float up = tanf(pv->fov.angleUp), dn = tanf(pv->fov.angleDown);
    if (!(rt - l > 1e-4f) || !(up - dn > 1e-4f) || r->extent.width <= 0 || r->extent.height <= 0) return 0;
    o->width = w, o->height = h, o->layer = pv->subImage.imageArrayIndex;
    o->x = r->offset.x, o->y = r->offset.y, o->w = r->extent.width, o->h = r->extent.height;
    o->cx = (float)r->offset.x + (float)r->extent.width * (-l / (rt - l));
    o->cy = (float)r->offset.y + (float)r->extent.height * ((gles ? -dn : up) / (up - dn));
    return 1;
}

// In libVkLayer_fdm_darken.so (a DT_NEEDED of the arm64 build); NULL without it (32-bit builds).
extern void fp_fdm_set_views(const fdm_view_info *views, uint32_t count) __attribute__((weak));

static void fdm_views_end_frame(const XrFrameEndInfo *info) {
    void (*set_views)(const fdm_view_info *, uint32_t) = fp_fdm_set_views;
    if (!set_views || !info) return;
    const XrCompositionLayerProjection *proj = NULL;
    for (uint32_t i = 0; i < info->layerCount && !proj; ++i)
        if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            proj = (const XrCompositionLayerProjection *)info->layers[i];
    if (!proj) return;  // keep the last views (a frame of quads only doesn't move the eyes)
    fdm_view_info views[4];
    uint32_t n = 0;
    for (uint32_t v = 0; v < proj->viewCount && n < 4; ++v)
        n += (uint32_t)fdm_view_of(&proj->views[v], !vk.device, &views[n]);
    static fdm_view_info last[4];
    static uint32_t last_n;
    // the FOV moves a little every frame: resend when a centre moved by a pixel or more
    int changed = n != last_n;
    for (uint32_t i = 0; i < n && !changed; ++i)
        changed = memcmp(&views[i], &last[i], offsetof(fdm_view_info, cx)) || fabsf(views[i].cx - last[i].cx) >= 1.0f ||
                  fabsf(views[i].cy - last[i].cy) >= 1.0f;
    if (!changed) return;
    memcpy(last, views, sizeof(views));
    last_n = n;
    set_views(views, n);
    static int logged;
    for (uint32_t i = 0; i < n && logged < 8; ++i, ++logged)
        LOG("fdm views: %ux%u layer %u rect=%d,%d %dx%d optical centre %.0f,%.0f", views[i].width, views[i].height,
            views[i].layer, views[i].x, views[i].y, views[i].w, views[i].h, views[i].cx, views[i].cy);
}
