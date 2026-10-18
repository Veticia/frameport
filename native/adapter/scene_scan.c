// SPDX-License-Identifier: GPL-3.0-only
// Included by scene_emu.c; all scan state is protected by scene_lock.
static int scan_path(char *path, size_t size, const char *kind) {
    char anchor[4096];
    if (!anchor_path(anchor, sizeof(anchor))) return 0;
    char *slash = strrchr(anchor, '/');
    if (!slash) return 0;
    *slash = 0;
    int n = snprintf(path, size, "%s/framebridge-scan-%s", anchor, kind);
    return n > 0 && (size_t)n < size;
}
static void clean_scan_files(void) {
    const char *kinds[] = {"request", "ack", "result"};
    char path[4096];
    for (unsigned i = 0; i < 3; ++i) if (scan_path(path, sizeof(path), kinds[i])) unlink(path);
}
static int start_scan(XrAsyncRequestIdFB id) {
    char path[4096], tmp[4128];
    if (!scan_path(path, sizeof(path), "request")) return 0;
    clean_scan_files();
    snprintf(tmp, sizeof(tmp), "%s.tmp-XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) { LOG("scene: scan request write failed (%d), using the stored room", errno); return 0; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return 0; }
    int ok = fprintf(f, "request %llu %llu\n", (unsigned long long)id, (unsigned long long)time(NULL)) > 0;
    if (fflush(f) || fsync(fd)) ok = 0;
    if (fclose(f)) ok = 0;
    if (ok && rename(tmp, path)) ok = 0;
    if (!ok) { unlink(tmp); LOG("scene: scan request write failed (%d), using the stored room", errno); return 0; }
    scan_started_at = scene_now_ns();
    scan_poll_at = 0;
    scan_waiting = 1;
    scan_acked = 0;
    LOG("scene: scan requested from FramePort (request %llu)", (unsigned long long)id);
    return 1;
}
static int scan_message(const char *kind, char *status, char *reason, size_t size) {
    char path[4096], line[512];
    struct stat st;
    if (!scan_path(path, sizeof(path), kind) || stat(path, &st)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    unsigned long long id;
    int used = 0;
    if (!ok || sscanf(line, "%15s %llu %n", status, &id, &used) < 2 || id != pending_capture.requestId) return 0;
    snprintf(reason, size, "%s", line + used);
    reason[strcspn(reason, "\r\n")] = 0;
    return 1;
}
static int reload_scanned_room(void) {
    scene_entity *old = malloc(sizeof(entities));
    if (!old) return 0;
    memcpy(old, entities, sizeof(entities));
    int old_count = entity_count, old_room_count = room_entity_count, old_walls = wall_count, old_file = room_from_file;
    uint64_t old_universe = room_universe;
    unsigned old_generation = scene_generation;
    char old_source[sizeof(room_source)];
    memcpy(old_source, room_source, sizeof(room_source));
    memset(entities, 0, sizeof(entities));
    entity_count = room_entity_count = wall_count = room_from_file = 0;
    room_universe = 0;
    scene_generation = (scene_generation + 1) & 255;
    if (!load_room_file()) {
        reset_room();
        memcpy(entities, old, sizeof(entities));
        entity_count = old_count; room_entity_count = old_room_count; wall_count = old_walls; room_from_file = old_file;
        room_universe = old_universe; scene_generation = old_generation;
        memcpy(room_source, old_source, sizeof(room_source));
        free(old);
        return 0;
    }
    for (int i = 0; i < old_count; ++i) {
        free(old[i].boundary); free(old[i].vertices); free(old[i].indices);
    }
    free(old);
    load_anchors();
    return 1;
}
static void poll_scan(uint64_t now) {
    if (now < scan_poll_at) return;
    scan_poll_at = now + 250000000ull;
    char status[16], reason[512];
    if (!scan_acked && scan_message("ack", status, reason, sizeof(reason)) && !strcmp(status, "ack")) {
        scan_acked = 1;
        scan_started_at = now;  // allow the scanner a full six minutes after acknowledging
        LOG("scene: scan started by FramePort");
    }
    if (scan_message("result", status, reason, sizeof(reason)) &&
        (!strcmp(status, "ok") || !strcmp(status, "failed"))) {
        int ok = !strcmp(status, "ok") && reload_scanned_room();
        pending_capture_at = now;
        pending_capture.result = ok ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
        if (ok) LOG("scene: scanned room loaded (%d entities)", entity_count);
        else LOG("scene: scan failed (%s)", !strcmp(status, "failed") ? reason : "could not load scanned room");
    } else if (!scan_acked && now - scan_started_at >= 5000000000ull) {
        LOG("scene: no scanner answered, using the stored room");
    } else if (scan_acked && now - scan_started_at >= 360000000000ull) {
        pending_capture_at = now;
        pending_capture.result = XR_ERROR_RUNTIME_FAILURE;
        LOG("scene: scan failed (timeout)");
    } else return;
    scan_waiting = 0;
    clean_scan_files();
}
