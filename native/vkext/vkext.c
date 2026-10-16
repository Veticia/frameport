// SPDX-License-Identifier: GPL-3.0-only
// libfp_vkext.so (patch frame.vk_android_extensions, builds for this PC only): Android-only Vulkan device
// extensions for engines that refuse to start without them. AXRB's direct mode hands the guest the Windows driver's
// device extensions, which lack Android's external-memory ones and the fragment density map Quest GPUs have;
// CryEngine (The Climb 2) then fails "Renderer initialization" before it creates a device. The engine's import of
// vkEnumerateDeviceExtensionProperties is renamed to fpEnumerateDeviceExtensionProperties (same length,
// elf.rename_imports), which lists the real extensions plus the missing ones of EXTRA. AXRB's vkCreateDevice leaves out requested extensions the host GPU doesn't have, so nothing
// else changes; on a device that has them (the Frame, where the patch is never applied) the list is the same.
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const char* const EXTRA[] = {
    "VK_KHR_external_memory_fd",
    "VK_ANDROID_external_memory_android_hardware_buffer",
    "VK_EXT_queue_family_foreign",
    // Quest GPUs have it, desktop NVIDIA drivers don't: The Climb 2 requires it (no device without it; with it listed
    // and left out again by AXRB, the VrApi bridge's full-rate foveation stand-ins take over)
    "VK_EXT_fragment_density_map",
};
#define N_EXTRA (sizeof(EXTRA) / sizeof(EXTRA[0]))

__attribute__((visibility("default"))) VkResult fpEnumerateDeviceExtensionProperties(
    VkPhysicalDevice device, const char* layer, uint32_t* count, VkExtensionProperties* props) {
    if (layer || !count) return vkEnumerateDeviceExtensionProperties(device, layer, count, props);
    uint32_t n = 0;
    VkResult r = vkEnumerateDeviceExtensionProperties(device, NULL, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties* all = calloc(n + N_EXTRA, sizeof(*all));
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = vkEnumerateDeviceExtensionProperties(device, NULL, &n, all);
    if (r < 0) {
        free(all);
        return r;
    }
    uint32_t total = n;
    static int logged;
    for (unsigned e = 0; e < N_EXTRA; ++e) {
        int have = 0;
        for (uint32_t i = 0; i < n && !have; ++i) have = !strcmp(all[i].extensionName, EXTRA[e]);
        if (have) continue;
        strncpy(all[total].extensionName, EXTRA[e], VK_MAX_EXTENSION_NAME_SIZE - 1);
        all[total].specVersion = 1;
        if (!logged) __android_log_print(ANDROID_LOG_INFO, "FrameBridge", "fp_vkext: listing %s (not on this GPU)", EXTRA[e]);
        ++total;
    }
    logged = 1;
    if (!props) {
        *count = total;
        free(all);
        return VK_SUCCESS;
    }
    const uint32_t c = *count < total ? *count : total;
    memcpy(props, all, c * sizeof(*props));
    *count = c;
    free(all);
    return c < total ? VK_INCOMPLETE : VK_SUCCESS;
}
