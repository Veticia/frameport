"""Android-only Vulkan device extensions for builds for this PC (native/vkext): AXRB's direct mode, The Climb 2."""
from __future__ import annotations

from ...analysis import elf
from ..base import ApkContext, Patch, register
from . import artifact

HELPER = "libfp_vkext.so"
RENAMES = {"vkEnumerateDeviceExtensionProperties": "fpEnumerateDeviceExtensionProperties"}
# not the game's renderer: XR/Meta/FramePort libraries that may import the function too
NOT_ENGINE = ("libopenxr", "libOVR", "libovr", "libvrapi", "libfp_", "libframe", "libVkLayer", "libc++")


class VulkanAndroidExtensions(Patch):
    id = "frame.vk_android_extensions"
    on_pc = True
    title = "Vulkan: list Android's external-memory extensions (this PC)"
    description = (
        "Only in builds for this PC (AXRB's direct mode). Direct mode gives the game the Windows graphics driver's "
        "Vulkan device, which lacks extensions every Quest has: Android's external memory "
        "(VK_KHR_external_memory_fd, VK_ANDROID_external_memory_android_hardware_buffer, "
        "VK_EXT_queue_family_foreign) and VK_EXT_fragment_density_map. Engines that call VrApi with Vulkan "
        "themselves, such as CryEngine in The Climb 2, refuse to create their device without them (\"CrySystem "
        "initialization failed\" right after \"Renderer initialization\"). The game's renderer gets them listed "
        "(libfp_vkext.so); AXRB leaves them out again when the device is created.")
    order = 73

    def applies(self, a):  # never in a Steam Frame recipe: its driver has these extensions
        return False

    def pc_detect(self, a) -> bool:
        return a.direct_vrapi and a.graphics.startswith("Vulkan") and "arm64-v8a" in a.abis

    def apply(self, ctx: ApkContext) -> bool:
        ws = ctx.ws
        if ws.abi != "arm64-v8a":
            return False
        changed = []
        for lib in ws.libs():
            if lib.startswith(NOT_ENGINE):
                continue
            data = ws.read_lib(lib)
            if not elf.is_elf(data) or not set(RENAMES) & elf.dyn_symbols(data, defined=False):
                continue
            patched, renamed = elf.rename_imports(data, RENAMES)
            if renamed:
                ws.put(ws.lib(lib), elf.add_needed(patched, HELPER))
                changed.append(lib)
        if not changed:
            return False
        ws.put(ws.lib(HELPER), artifact(ws.abi, HELPER))
        ctx.notes.append(f"{', '.join(changed)}: Vulkan device extensions through {HELPER}")
        return True

    def validate(self, ctx: ApkContext):
        ws = ctx.ws
        if ws.abi != "arm64-v8a" or not ws.has(ws.lib(HELPER)):
            return []
        return [("Android Vulkan extensions listed", True, HELPER)]


register(VulkanAndroidExtensions)
