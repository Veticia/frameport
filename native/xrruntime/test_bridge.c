/* SPDX-License-Identifier: MIT */
#include <openxr/openxr.h>
#include <string.h>
static XrResult init(const XrLoaderInitInfoBaseHeaderKHR *info) {
    return info->type == XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR && info->next == (void *)0x1234 ?
        XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}
XrResult xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *fn) {
    (void)instance;
    if (!strcmp(name, "xrInitializeLoaderKHR")) {
        *fn = (PFN_xrVoidFunction)init;
        return XR_SUCCESS;
    }
    *fn = NULL;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
