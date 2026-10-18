// SPDX-License-Identifier: GPL-3.0-only
// swapchain_size is the adapter's mutex-protected table, shared with rect_clamp.
static int fixed_layer_size(const XrCompositionLayerBaseHeader *layer, XrExtent2Di *full) {
    if (!layer) return 0;
    uint32_t w = 0, h = 0;
    if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)layer;
        if (!p->viewCount || !p->views) return 0;
        for (uint32_t v = 0; v < p->viewCount; ++v) {
            uint32_t vw, vh;
            if (!swapchain_size(p->views[v].subImage.swapchain, &vw, &vh)) return 0;
            if (v && (vw != w || vh != h)) return 0; // no single full size for this layer
            w = vw; h = vh;
        }
    } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad *)layer;
        if (!swapchain_size(q->subImage.swapchain, &w, &h)) return 0;
    } else return 0; // preserve runtime behavior for other layer types
    if (!w || !h || w > INT32_MAX || h > INT32_MAX) return 0;
    full->width = (int32_t)w; full->height = (int32_t)h;
    return 1;
}
