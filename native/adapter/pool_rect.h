// First-fit edge packer. Removing a live rectangle immediately frees its space.
#pragma once
#include <stdint.h>
typedef struct {
    int x, y, w, h;
} PoolRect;
static int pool_place(const PoolRect *used, int n, int side, int w, int h, PoolRect *out) {
    if (w <= 0 || h <= 0 || w > side || h > side) return 0;
    for (int yi = -1; yi < n; ++yi)
        for (int xi = -1; xi < n; ++xi) {
            int x = xi < 0 ? 0 : used[xi].x + used[xi].w;
            int y = yi < 0 ? 0 : used[yi].y + used[yi].h;
            if (x > side - w || y > side - h) continue;
            int fits = 1;
            for (int i = 0; i < n; ++i)
                if (x < used[i].x + used[i].w && x + w > used[i].x && y < used[i].y + used[i].h && y + h > used[i].y) {
                    fits = 0;
                    break;
                }
            if (fits) {
                *out = (PoolRect){x, y, w, h};
                return 1;
            }
        }
    return 0;
}
