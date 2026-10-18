#include <assert.h>
#include <stdint.h>
#include <openxr/openxr.h>
static int swapchain_size(XrSwapchain handle, uint32_t *w, uint32_t *h) {
    if (!handle) return 0;
    *w = handle == (XrSwapchain)(uintptr_t)2 ? 1024 : 2768; *h = *w;
    return 1;
}
#include "fixed_resolution.h"
int main(void) {
    XrExtent2Di full = {0};
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                               {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection p = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    p.views = views; p.viewCount = 2;
    for (int i=0; i<2; ++i) {
        views[i].subImage.swapchain = (XrSwapchain)(uintptr_t)1;
        views[i].subImage.imageRect = (XrRect2Di){{0,64},{2704,2704}};
    }
    assert(fixed_layer_size((void *)&p, &full));
    assert(full.width == 2768 && full.height == 2768);
    assert(views[0].subImage.imageRect.extent.width == 2704); // never stretch a submitted subrect
    views[1].subImage.swapchain = (XrSwapchain)(uintptr_t)2;
    assert(!fixed_layer_size((void *)&p, &full));
    views[1].subImage.swapchain = XR_NULL_HANDLE;
    assert(!fixed_layer_size((void *)&p, &full));
    p.viewCount=0; assert(!fixed_layer_size((void *)&p, &full));
    assert(!fixed_layer_size(NULL, &full));
    XrCompositionLayerQuad q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.subImage.swapchain=(XrSwapchain)(uintptr_t)2;
    assert(fixed_layer_size((void *)&q, &full) && full.width == 1024);
    q.type=XR_TYPE_UNKNOWN; assert(!fixed_layer_size((void *)&q, &full));
    return 0;
}
