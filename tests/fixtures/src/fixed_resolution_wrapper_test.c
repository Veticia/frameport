#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <openxr/openxr.h>
static int missing, valid_size=1, calls;
static XrInstance active_instance;
static XrResult XRAPI_CALL runtime(XrSession session, const XrRecommendedLayerResolutionGetInfoMETA *info,
        XrRecommendedLayerResolutionMETA *out) {
    (void)session; (void)info; ++calls;
    out->recommendedImageDimensions=(XrExtent2Di){2704,2704};
    out->isValid=XR_FALSE;
    return XR_SUCCESS;
}
static PFN_xrVoidFunction lookup(XrInstance instance, const char *name) {
    (void)instance; (void)name; return missing ? NULL : (PFN_xrVoidFunction)runtime;
}
static int swapchain_size(XrSwapchain handle, uint32_t *w, uint32_t *h) {
    (void)handle; *w=2768; *h=2768; return valid_size;
}
static void log_stub(const char *fmt, ...) { (void)fmt; }
#define LOG(...) log_stub(__VA_ARGS__)
#include "fixed_resolution.h"
/* WRAPPER */
int main(void) {
    XrCompositionLayerQuad layer={XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrRecommendedLayerResolutionGetInfoMETA info={XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META};
    info.layer=(void *)&layer;
    XrRecommendedLayerResolutionMETA out={XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META};
    int tail=42; out.next=&tail;
    for (missing=0; missing<2; ++missing) {
        assert(fixed_recommended_resolution(XR_NULL_HANDLE,&info,&out)==XR_SUCCESS);
        assert(out.isValid==XR_TRUE && out.recommendedImageDimensions.width==2768);
        assert(out.recommendedImageDimensions.height==2768 && out.next==&tail && tail==42);
    }
    assert(calls==1);
    valid_size=0; missing=0;
    assert(fixed_recommended_resolution(XR_NULL_HANDLE,&info,&out)==XR_SUCCESS);
    assert(out.recommendedImageDimensions.width==2704 && !out.isValid);
    missing=1;
    assert(fixed_recommended_resolution(XR_NULL_HANDLE,&info,&out)==XR_ERROR_FUNCTION_UNSUPPORTED);
    assert(fixed_recommended_resolution(XR_NULL_HANDLE,NULL,&out)==XR_ERROR_VALIDATION_FAILURE);
    assert(fixed_recommended_resolution(XR_NULL_HANDLE,&info,NULL)==XR_ERROR_VALIDATION_FAILURE);
    return 0;
}
