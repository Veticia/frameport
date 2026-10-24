// Host harness for native/foveation/centre.h (GitHub #394): a fake next layer records the density-map offsets the
// centring layer hands down for the render passes Valve's foveation layer would record.
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#define CHECK(c)                                                                                                     \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "line %d: %s\n", __LINE__, #c);                                                            \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
#define LOG(...) ((void)0)
#define EXPORT
#define MAX_HANDLES 16
static void take_lock(int *p) { while (__atomic_exchange_n(p, 1, __ATOMIC_ACQUIRE)) {} }
static void drop_lock(int *p) { __atomic_store_n(p, 0, __ATOMIC_RELEASE); }
static void *key_of(const void *handle) { return handle ? *(void *const *)handle : NULL; }
static PFN_vkVoidFunction own(const char *name) { (void)name; return NULL; }
#include "centre.h"

// ---- fake next layer
static void *dispatch_key = (void *)0x1234;
static struct { void *key; } device_obj = {(void *)0x1234}, cmd_obj = {(void *)0x1234};
static uintptr_t next_handle = 0x1000;
static VkImageCreateFlags last_image_flags;
static int ends, image_changed;
static uint32_t got_count;
static VkOffset2D got[4];

static VKAPI_ATTR VkResult VKAPI_CALL n_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a,
                                                    VkImage *img) {
    (void)d, (void)a;
    last_image_flags = ci->flags;
    *img = (VkImage)(next_handle += 8);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL n_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci,
                                                        const VkAllocationCallbacks *a, VkImageView *v) {
    (void)d, (void)ci, (void)a;
    *v = (VkImageView)(next_handle += 8);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL n_CreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2 *ci,
                                                          const VkAllocationCallbacks *a, VkRenderPass *rp) {
    (void)d, (void)ci, (void)a;
    *rp = (VkRenderPass)(next_handle += 8);
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL n_CreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *ci,
                                                          const VkAllocationCallbacks *a, VkFramebuffer *fb) {
    (void)d, (void)ci, (void)a;
    *fb = (VkFramebuffer)(next_handle += 8);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL n_CmdBeginRenderPass2(VkCommandBuffer c, const VkRenderPassBeginInfo *b,
                                                        const VkSubpassBeginInfo *s) {
    (void)c, (void)b, (void)s;
}
static void record(const void *next) {
    ++ends;
    got_count = 0;
    const c_offset_end *o = (const void *)c_chain(next, ST_FDM_OFFSET_END);
    if (o) {
        got_count = o->count;
        memcpy(got, o->offsets, o->count * sizeof(VkOffset2D));
    }
}
static VKAPI_ATTR void VKAPI_CALL n_CmdEndRenderPass2(VkCommandBuffer c, const VkSubpassEndInfo *e) {
    (void)c;
    record(e ? e->pNext : NULL);
}
static VKAPI_ATTR void VKAPI_CALL n_CmdEndRenderPass(VkCommandBuffer c) {
    (void)c;
    record(NULL);
}

// One pass into a w x h swapchain with Valve's ceil(w/32) x ceil(h/32) two-layer map: multiview (view_mask 3, the
// whole map) or a one-eye pass (view_mask 0, the map view of `eye`'s layer, as Valve attaches it); ends with `valve`
// offsets (NULL: none). `flagged`: the map has the offset flag (Valve's offsets on).
static void pass_ex(uint32_t w, uint32_t h, const VkOffset2D *valve, uint32_t count, uint32_t view_mask, uint32_t eye,
                    int flagged) {
    VkDevice dev = (VkDevice)&device_obj;
    VkCommandBuffer cmd = (VkCommandBuffer)&cmd_obj;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent = (VkExtent3D){(w + 31) / 32, (h + 31) / 32, 1};
    ici.arrayLayers = 2;
    ici.usage = FDM_USAGE;
    ici.flags = flagged ? FDM_OFFSET_FLAG : 0;
    VkImage map;
    c_CreateImage(dev, &ici, NULL, &map);
    if ((last_image_flags & FDM_OFFSET_FLAG) != (ici.flags & FDM_OFFSET_FLAG)) image_changed = 1;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = map;
    vci.subresourceRange.baseArrayLayer = view_mask ? 0 : eye;
    vci.subresourceRange.layerCount = view_mask ? 2 : 1;
    VkImageView view;
    c_CreateImageView(dev, &vci, NULL, &view);
    VkRenderPassFragmentDensityMapCreateInfoEXT fdm = {ST_FDM_RP_INFO, NULL, {1, VK_IMAGE_LAYOUT_GENERAL}};
    VkSubpassDescription2 sub = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    sub.viewMask = view_mask;
    VkRenderPassCreateInfo2 rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, &fdm};
    rci.subpassCount = 1, rci.pSubpasses = &sub;
    VkRenderPass rp;
    c_CreateRenderPass2(dev, &rci, NULL, &rp);
    VkImageView atts[2] = {(VkImageView)0x99, view};
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = rp, fci.attachmentCount = 2, fci.pAttachments = atts, fci.width = w, fci.height = h, fci.layers = 1;
    VkFramebuffer fb;
    c_CreateFramebuffer(dev, &fci, NULL, &fb);
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp, bi.framebuffer = fb, bi.renderArea.extent = (VkExtent2D){w, h};
    c_CmdBeginRenderPass2(cmd, &bi, NULL);
    if (valve) {
        c_offset_end o = {ST_FDM_OFFSET_END, NULL, count, valve};
        VkSubpassEndInfo e = {VK_STRUCTURE_TYPE_SUBPASS_END_INFO, &o};
        c_CmdEndRenderPass2(cmd, &e);
    } else {
        c_CmdEndRenderPass(cmd);
    }
}
static void pass(uint32_t w, uint32_t h, const VkOffset2D *valve, uint32_t count) {
    pass_ex(w, h, valve, count, 3, 0, 1);
}

static int near(int got_value, float want, int tolerance) { return fabsf((float)got_value - want) <= (float)tolerance; }

int main(void) {
    (void)dispatch_key;
    c_device d;
    memset(&d, 0, sizeof(d));
    d.key = device_obj.key, d.device = (VkDevice)&device_obj, d.gdpa = (PFN_vkGetDeviceProcAddr)1, d.gran = (VkExtent2D){8, 8};
    d.CreateImage = n_CreateImage, d.CreateImageView = n_CreateImageView, d.CreateRenderPass2 = n_CreateRenderPass2;
    d.CreateFramebuffer = n_CreateFramebuffer, d.CmdBeginRenderPass2 = n_CmdBeginRenderPass2;
    d.CmdEndRenderPass2 = n_CmdEndRenderPass2, d.CmdEndRenderPass = n_CmdEndRenderPass;
    c_devices[0] = d;

    // Valve's lo centre on a 2016x1728 swapchain (63x54 map, 32 px texels): texels 23-40 and 20-34
    CHECK(c_valve_centre(2016, 63, 0) == 1024.0f && c_valve_centre(1728, 54, 1) == 880.0f);
    c_level = 3;
    CHECK(c_valve_centre(2016, 63, 0) == 1024.0f);  // hi: texels 24-39
    c_level = 1;

    // AC Nexus' asymmetric eyes: rect=0,0 / 287,0 1726x1728 inside 2016x1728, optical centres from the FOV
    fp_fdm_view views[2] = {{2016, 1728, 0, 0, 0, 1726, 1728, 1002, 696}, {2016, 1728, 1, 287, 0, 1726, 1728, 1001, 709}};
    fp_fdm_set_views(views, 2);

    // Eye-tracked with no gaze (all 0): straight ahead = the optical centre. Both offsets Turnip reads are written.
    c_fixed = 0;
    VkOffset2D zero[2] = {{0, 0}, {0, 0}};
    pass(2016, 1728, zero, 2);
    CHECK(ends == 1 && got_count == 2);
    CHECK(near(got[0].x, 1002 - 1024, 4) && near(got[0].y, 696 - 880, 4));
    CHECK(near(got[1].x, 1001 - 1024, 4) && near(got[1].y, 709 - 880, 4));
    CHECK(got[0].x % 8 == 0 && got[0].y % 8 == 0);
    CHECK(!image_changed);  // images are never re-flagged

    // Valve declares 1 offset for a two-view pass: Turnip still reads 2, so the layer hands down 2
    pass(2016, 1728, zero, 1);
    CHECK(got_count == 2 && near(got[1].x, 1001 - 1024, 4) && near(got[1].y, 709 - 880, 4));

    // A one-eye pass (no multiview) with the right eye's map layer: its single offset is the right eye's
    pass_ex(2016, 1728, zero, 1, 0, 1, 1);
    CHECK(got_count == 1 && near(got[0].x, 1001 - 1024, 4) && near(got[0].y, 709 - 880, 4));

    // Eye-tracked gaze: Valve's offset (gaze NDC * half the framebuffer, from the swapchain centre) is re-based on
    // the eye's rect: gaze right by a quarter of the framebuffer = a quarter of the rect from the rect's centre
    VkOffset2D gaze[2] = {{504, 0}, {-504, 432}};
    pass(2016, 1728, gaze, 2);
    CHECK(near(got[0].x, 863 + 504 * 1726.0f / 2016 - 1024, 4) && near(got[0].y, 864 - 880, 4));
    CHECK(near(got[1].x, 287 + 863 - 504 * 1726.0f / 2016 - 1024, 4) && near(got[1].y, 864 + 432 - 880, 4));

    // Valve computes both eyes' gaze offsets but declares count 1 (seen on the Frame): each eye uses its own
    pass(2016, 1728, gaze, 1);
    CHECK(got_count == 2 && near(got[1].x, 287 + 863 - 504 * 1726.0f / 2016 - 1024, 4));

    // Fixed (Valve's offsets on underneath): its gaze is ignored, the optical centre wins
    c_fixed = 1;
    pass(2016, 1728, gaze, 2);
    CHECK(got_count == 2 && near(got[0].x, -22, 4) && near(got[1].y, -171, 4));

    // A map without the offset flag (Valve's offsets off, e.g. it read FDM_DEBUG before this layer changed it): left
    // alone, nothing added
    pass_ex(2016, 1728, NULL, 0, 3, 0, 0);
    CHECK(got_count == 0);

    // A pass of another size (no reported view): Valve's offsets go through unchanged
    pass(1024, 1024, gaze, 2);
    CHECK(got_count == 2 && got[0].x == 504 && got[1].y == 432);

    // Both eyes side by side in one layer: one map can't serve both, left alone
    fp_fdm_view wide[2] = {{3456, 1728, 0, 0, 0, 1728, 1728, 1000, 700}, {3456, 1728, 0, 1728, 0, 1728, 1728, 2450, 700}};
    fp_fdm_set_views(wide, 2);
    pass(3456, 1728, gaze, 2);
    CHECK(got_count == 2 && got[0].x == 504);

    // Off (FRAMEPORT_FDM_CENTRE=0): nothing changes
    fp_fdm_set_views(views, 2);
    c_disabled = 1;
    pass(2016, 1728, gaze, 2);
    CHECK(got_count == 2 && got[0].x == 504 && got[1].x == -504);
    c_disabled = 0;

    // Fixed keeps Valve's offsets on: disable_offsets is dropped from FDM_DEBUG, other flags stay
    setenv("FDM_DEBUG", "enable,disable_offsets,debug", 1);
    c_fixed = 1;
    c_fixed_with_offsets();
    CHECK(!strcmp(getenv("FDM_DEBUG"), "enable,debug"));

    // configuration as Valve reads it: FOVE_LEVEL beats FDM_DEBUG's lo/med/hi
    setenv("FDM_DEBUG", "enable,disable_offsets,med", 1);
    unsetenv("FOVE_LEVEL");
    unsetenv("FRAMEPORT_FDM_CENTRE");
    c_read_config();
    CHECK(c_fixed && c_level == 2 && !c_disabled);
    setenv("FOVE_LEVEL", "hi", 1);
    setenv("FDM_DEBUG", "enable", 1);
    setenv("FRAMEPORT_FDM_CENTRE", "0", 1);
    c_read_config();
    CHECK(!c_fixed && c_level == 3 && c_disabled);
    CHECK(c_has_token("enable,disable_offsets", "disable_offsets") && !c_has_token("enable", "disable_offsets"));
    CHECK(c_has_token("enable,hi", "hi") && !c_has_token("enable,high", "hi"));
    puts("ok");
    return 0;
}
