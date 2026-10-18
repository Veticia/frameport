// SPDX-License-Identifier: GPL-3.0-only
// Included by scene_emu.c, using the cached OpenXR registry declarations (no private ABI copies).
static int meta_filter_matches(const XrSpaceDiscoveryInfoMETA *info, const scene_entity *e) {
    for (uint32_t k = 0; k < info->filterCount; ++k) {
        const XrSpaceFilterBaseHeaderMETA *f = info->filters[k];
        if (!f) continue;
        if (f->type == XR_TYPE_SPACE_FILTER_COMPONENT_META &&
            !entity_has_component(e, ((const XrSpaceFilterComponentMETA *)f)->componentType)) return 0;
        if (f->type == XR_TYPE_SPACE_FILTER_UUID_META) {
            const XrSpaceFilterUuidMETA *u = (const XrSpaceFilterUuidMETA *)f;
            if (!u->uuids || !uuid_matches(u->uuidCount, u->uuids, e)) return 0;
        }
    }
    return 1;
}
// Diagnostics: MRUK takes the room whose floor outline contains the head as the "current room"; outside it, a game
// can ask for a room scan again and again (Pinball FX). Logs the head against the floor outline (3 times per session).
static void log_head_in_room(void) {
    static int logged;
    static XrSpace view = XR_NULL_HANDLE;
    const scene_entity *f = &entities[ENT_FLOOR];
    if (logged >= 3 || !scene_session || !scene_stage || !last_predicted_time || entity_count <= ENT_FLOOR ||
        !f->has_bounds) return;
    if (!view) {
        PFN_xrCreateReferenceSpace create = (PFN_xrCreateReferenceSpace)lookup(active_instance, "xrCreateReferenceSpace");
        XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_VIEW,
                                         {{0, 0, 0, 1}, {0, 0, 0}}};
        if (!create || XR_FAILED(create(scene_session, &ci, &view))) return;
    }
    XrPosef head;
    XrSpaceLocationFlags flags = 0;
    if (!stage_pose(view, last_predicted_time, &head, &flags)) return;
    XrVector3f p = pose_mul(pose_inv(f->pose_in_stage), head).position;  // floor plane: x, y; its normal: z
    XrVector2f rect[4], *v = f->boundary;
    uint32_t n = f->boundary_count;
    if (!n) {
        const XrRect2Df b = f->bounds;
        rect[0] = (XrVector2f){b.offset.x, b.offset.y};
        rect[1] = (XrVector2f){b.offset.x + b.extent.width, b.offset.y};
        rect[2] = (XrVector2f){b.offset.x + b.extent.width, b.offset.y + b.extent.height};
        rect[3] = (XrVector2f){b.offset.x, b.offset.y + b.extent.height};
        v = rect;
        n = 4;
    }
    int inside = 0;
    for (uint32_t a = 0, b = n - 1; a < n; b = a++)
        if ((v[a].y > p.y) != (v[b].y > p.y) && p.x < (v[b].x - v[a].x) * (p.y - v[a].y) / (v[b].y - v[a].y) + v[a].x)
            inside = !inside;
    LOG("scene: head in STAGE %.2f,%.2f,%.2f (flags 0x%llx), floor-local %.2f,%.2f: %s the floor outline (%u points)",
        head.position.x, head.position.y, head.position.z, (unsigned long long)flags, p.x, p.y,
        inside ? "inside" : "OUTSIDE", n);
    ++logged;
}
static XRAPI_ATTR XrResult XRAPI_CALL emu_discover_spaces(XrSession session, const XrSpaceDiscoveryInfoMETA *info,
                                                         XrAsyncRequestIdFB *request) {
    if (!info || !request || (info->filterCount && !info->filters)) return XR_ERROR_VALIDATION_FAILURE;
    pthread_mutex_lock(&scene_lock);
    scene_session = session;
    build_room();
    if (event_free() < 2) { pthread_mutex_unlock(&scene_lock); return XR_ERROR_LIMIT_REACHED; }
    scene_query *q = new_query();
    if (!q) { pthread_mutex_unlock(&scene_lock); return XR_ERROR_OUT_OF_MEMORY; }
    for (int i = 0; i < entity_count; ++i) {
        scene_entity *e = &entities[i];
        if (!e->erased && meta_filter_matches(info, e))
            q->results[q->count++] = (XrSpaceQueryResultFB){e->handle, e->uuid};
    }
    *request = q->id;
    XrEventDataSpaceDiscoveryResultsAvailableMETA avail = {XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_RESULTS_AVAILABLE_META, NULL, *request};
    XrEventDataSpaceDiscoveryCompleteMETA done = {XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_COMPLETE_META, NULL, *request, XR_SUCCESS};
    push_event(&avail, sizeof(avail)); push_event(&done, sizeof(done));
    LOG("scene: discovery request=%llu results=%u", (unsigned long long)*request, q->count);
    log_head_in_room();
    pthread_mutex_unlock(&scene_lock);
    return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL emu_retrieve_discovery(XrSession session, XrAsyncRequestIdFB request,
                                                           XrSpaceDiscoveryResultsMETA *results) {
    (void)session;
    if (!results) return XR_ERROR_VALIDATION_FAILURE;
    pthread_mutex_lock(&scene_lock);
    scene_query *q = find_query(request);
    XrResult r = XR_SUCCESS;
    if (!q) r = XR_ERROR_VALIDATION_FAILURE;
    else {
        results->resultCountOutput = q->count;
        if (results->resultCapacityInput && results->resultCapacityInput < q->count) r = XR_ERROR_SIZE_INSUFFICIENT;
        else if (results->resultCapacityInput) {
            if (!results->results) r = XR_ERROR_VALIDATION_FAILURE;
            else for (uint32_t i = 0; i < q->count; ++i)
                results->results[i] = (XrSpaceDiscoveryResultMETA){q->results[i].space, q->results[i].uuid};
        }
    }
    pthread_mutex_unlock(&scene_lock);
    return r;
}
static XRAPI_ATTR XrResult XRAPI_CALL emu_save_spaces(XrSession session, const XrSpacesSaveInfoMETA *info,
                                                     XrAsyncRequestIdFB *request) {
    (void)session;
    if (!info || !request || !info->spaceCount || !info->spaces) return XR_ERROR_VALIDATION_FAILURE;
    pthread_mutex_lock(&scene_lock);
    if (!event_free()) { pthread_mutex_unlock(&scene_lock); return XR_ERROR_LIMIT_REACHED; }
    XrResult r = persist_spaces(info->spaceCount, info->spaces, 0, NULL, 0);
    *request = next_request++;
    XrEventDataSpacesSaveResultMETA done = {XR_TYPE_EVENT_DATA_SPACES_SAVE_RESULT_META, NULL, *request, r};
    push_event(&done, sizeof(done));
    pthread_mutex_unlock(&scene_lock);
    return XR_SUCCESS;
}
// XR_META_boundary_visibility: the Frame shows no guardian over an app anyway, so a request always succeeds; the
// change event (once per new state, like Meta's runtime) is what MR games wait for before they start.
static XrBoundaryVisibilityMETA boundary_visibility = XR_BOUNDARY_VISIBILITY_NOT_SUPPRESSED_META;
static XRAPI_ATTR XrResult XRAPI_CALL emu_request_boundary_visibility(XrSession session, XrBoundaryVisibilityMETA v) {
    (void)session;
    if (v != XR_BOUNDARY_VISIBILITY_SUPPRESSED_META && v != XR_BOUNDARY_VISIBILITY_NOT_SUPPRESSED_META)
        return XR_ERROR_VALIDATION_FAILURE;
    pthread_mutex_lock(&scene_lock);
    if (v != boundary_visibility) {
        boundary_visibility = v;
        XrEventDataBoundaryVisibilityChangedMETA e = {XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META, NULL, v};
        push_event(&e, sizeof(e));
        LOG("scene: boundary %s", v == XR_BOUNDARY_VISIBILITY_SUPPRESSED_META ? "suppressed" : "shown");
    }
    pthread_mutex_unlock(&scene_lock);
    return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL emu_erase_spaces(XrSession session, const XrSpacesEraseInfoMETA *info,
                                                      XrAsyncRequestIdFB *request) {
    (void)session;
    if (!info || !request || (info->spaceCount && !info->spaces) || (info->uuidCount && !info->uuids))
        return XR_ERROR_VALIDATION_FAILURE;
    pthread_mutex_lock(&scene_lock);
    if (!event_free()) { pthread_mutex_unlock(&scene_lock); return XR_ERROR_LIMIT_REACHED; }
    XrResult r = persist_spaces(info->spaceCount, info->spaces, info->uuidCount, info->uuids, 1);
    *request = next_request++;
    XrEventDataSpacesEraseResultMETA done = {XR_TYPE_EVENT_DATA_SPACES_ERASE_RESULT_META, NULL, *request, r};
    push_event(&done, sizeof(done));
    pthread_mutex_unlock(&scene_lock);
    return XR_SUCCESS;
}
