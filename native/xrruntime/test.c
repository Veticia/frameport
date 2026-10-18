/* SPDX-License-Identifier: MIT */
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <assert.h>
#include <string.h>
extern XrResult xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *, XrNegotiateRuntimeRequest *);
extern XrResult xrInitializeLoaderKHR(const XrLoaderInitInfoBaseHeaderKHR *);
int main(int argc, char **argv) {
    XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION,
        sizeof(li), 1, 1, XR_MAKE_VERSION(1,0,0), XR_CURRENT_API_VERSION};
    XrNegotiateRuntimeRequest r = {XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(r)};
    assert(xrNegotiateLoaderRuntimeInterface(NULL, &r) < 0);
    assert(xrNegotiateLoaderRuntimeInterface(&li, NULL) < 0);
    li.minInterfaceVersion = 2;
    assert(xrNegotiateLoaderRuntimeInterface(&li, &r) < 0);
    li.minInterfaceVersion = 1;
    r.structSize--;
    assert(xrNegotiateLoaderRuntimeInterface(&li, &r) < 0);
    r.structSize++;
    li.minApiVersion = XR_MAKE_VERSION(2,0,0);
    assert(xrNegotiateLoaderRuntimeInterface(&li, &r) < 0);
    li.minApiVersion = XR_MAKE_VERSION(1,0,0);
    li.minApiVersion = XR_MAKE_VERSION(1,0,1);
    li.maxApiVersion = XR_MAKE_VERSION(1,0,0);
    assert(xrNegotiateLoaderRuntimeInterface(&li, &r) < 0);
    li.minApiVersion = XR_MAKE_VERSION(1,0,0);
    li.maxApiVersion = XR_CURRENT_API_VERSION;
    XrResult result = xrNegotiateLoaderRuntimeInterface(&li, &r);
    if (argc > 1 && !strcmp(argv[1], "missing")) {
        assert(result == XR_ERROR_INITIALIZATION_FAILED);
        return 0;
    }
    assert(result == XR_SUCCESS && r.getInstanceProcAddr && r.runtimeInterfaceVersion == 1);
    assert(r.runtimeApiVersion <= li.maxApiVersion && r.runtimeApiVersion >= li.minApiVersion);
    XrLoaderInitInfoBaseHeaderKHR info = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, (void *)0x1234};
    assert(xrInitializeLoaderKHR(&info) == XR_SUCCESS);
    info.next = NULL;
    assert(xrInitializeLoaderKHR(&info) == XR_ERROR_RUNTIME_FAILURE);
    return 0;
}
