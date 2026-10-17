// SPDX-License-Identifier: GPL-3.0-only
// Included by frame_adapter.c. Compatibility only: the Android runtime exposes no
// colour conversion API. REC709 describes the conventional SDR content path, not
// a measured native panel gamut. Accepting other spaces does not convert pixels.
// Proc lookup is also exposed through framebridge_extension_proc for an installed
// xrshim: OVRPort's fixed dispatcher cannot forward these new function names.
// Shim installation and dispatcher extension-list synthesis live outside this adapter.
static int emulate_color_space;
static const XrColorSpaceFB color_spaces[] = {
    XR_COLOR_SPACE_UNMANAGED_FB, XR_COLOR_SPACE_REC2020_FB, XR_COLOR_SPACE_REC709_FB,
    XR_COLOR_SPACE_RIFT_CV1_FB, XR_COLOR_SPACE_RIFT_S_FB, XR_COLOR_SPACE_QUEST_FB,
    XR_COLOR_SPACE_P3_FB, XR_COLOR_SPACE_ADOBE_RGB_FB
};

static XRAPI_ATTR XrResult XRAPI_CALL emu_enumerate_color_spaces(XrSession session,
        uint32_t capacity, uint32_t *count, XrColorSpaceFB *spaces) {
    (void)session;
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = sizeof(color_spaces) / sizeof(color_spaces[0]);
    if (!capacity) return XR_SUCCESS;
    if (!spaces) return XR_ERROR_VALIDATION_FAILURE;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(spaces, color_spaces, sizeof(color_spaces));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL emu_set_color_space(XrSession session, XrColorSpaceFB space) {
    (void)session;
    for (size_t i = 0; i < sizeof(color_spaces) / sizeof(color_spaces[0]); ++i) {
        if (space != color_spaces[i]) continue;
        static int logged;
        if (!logged) {
            logged = 1;
            LOG("color_space: accepted space=%d; no colour conversion (runtime has no API)", (int)space);
        }
        return XR_SUCCESS;
    }
    return XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB;
}

static void color_space_properties(XrSystemProperties *properties) {
    if (!emulate_color_space || !properties) return;
    for (XrBaseOutStructure *p = properties->next; p; p = p->next)
        if (p->type == XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB)
            ((XrSystemColorSpacePropertiesFB *)p)->colorSpace = XR_COLOR_SPACE_REC709_FB;
}

static PFN_xrVoidFunction color_space_emulation(const char *name) {
    if (!emulate_color_space) return NULL;
    if (!strcmp(name, "xrEnumerateColorSpacesFB")) return (PFN_xrVoidFunction)emu_enumerate_color_spaces;
    if (!strcmp(name, "xrSetColorSpaceFB")) return (PFN_xrVoidFunction)emu_set_color_space;
    return NULL;
}
