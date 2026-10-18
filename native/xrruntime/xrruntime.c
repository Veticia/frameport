/* SPDX-License-Identifier: MIT
 * Runtime endpoint for an application's embedded Khronos loader.
 * Only the application's loader paths change: the inner loader still finds SteamVR.
 */
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#ifdef __ANDROID__
#include <android/log.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "fp_xrruntime", __VA_ARGS__)
#else
#include <stdio.h>
#define LOG(...) do { fprintf(stderr, "fp_xrruntime: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif
#define EXPORT __attribute__((visibility("default")))
static pthread_once_t once = PTHREAD_ONCE_INIT;
static PFN_xrGetInstanceProcAddr chain;

static void resolve(void) {
    /* RTLD_LOCAL and handle-specific dlsym prevent binding to the game's own xr* exports. */
    void *bridge = dlopen("libopenxr_loader_generic.so", RTLD_NOW | RTLD_LOCAL);
    if (bridge) chain = (PFN_xrGetInstanceProcAddr)dlsym(bridge, "xrGetInstanceProcAddr");
    if (chain) LOG("chain resolved: FrameBridge");
    else LOG("chain resolution failed: %s", dlerror());
}

EXPORT XrResult XRAPI_CALL xrInitializeLoaderKHR(const XrLoaderInitInfoBaseHeaderKHR *info) {
    pthread_once(&once, resolve);
    if (!chain || !info) return XR_ERROR_INITIALIZATION_FAILED;
    PFN_xrVoidFunction proc = NULL;
    XrResult result = chain(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &proc);
    if (XR_SUCCEEDED(result) && proc)
        result = ((PFN_xrInitializeLoaderKHR)proc)(info);
    else if (XR_SUCCEEDED(result)) result = XR_ERROR_INITIALIZATION_FAILED;
    LOG("xrInitializeLoaderKHR forwarded to FrameBridge: %d", result);
    return result;
}

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
        const XrNegotiateLoaderInfo *loader, XrNegotiateRuntimeRequest *runtime) {
    if (!loader || !runtime ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof(*loader) ||
        runtime->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtime->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || runtime->structSize != sizeof(*runtime) ||
        loader->minInterfaceVersion > 1 || loader->maxInterfaceVersion < 1 ||
        loader->minApiVersion > loader->maxApiVersion ||
        loader->minApiVersion > XR_CURRENT_API_VERSION || loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0))
        return XR_ERROR_INITIALIZATION_FAILED;
    pthread_once(&once, resolve);
    if (!chain) return XR_ERROR_INITIALIZATION_FAILED;
    runtime->runtimeInterfaceVersion = 1;
    runtime->runtimeApiVersion = loader->maxApiVersion < XR_CURRENT_API_VERSION ?
        loader->maxApiVersion : XR_CURRENT_API_VERSION;
    runtime->getInstanceProcAddr = chain;
    LOG("negotiated runtime interface 1: FrameBridge");
    return XR_SUCCESS;
}
