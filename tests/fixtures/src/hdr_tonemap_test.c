// SPDX-License-Identifier: GPL-3.0-only
// HDR to SDR tone mapping of the software Main 10 path (native/hevc/hdr_tonemap.h).
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hdr_tonemap.h"

static int code_for_nits(float nits) { return (int)lroundf(64 + 876 * fp_tm_pq_oetf(nits)); }
static int sdr_code(float rel) { return (int)lroundf(16 + 219 * powf(rel, 1 / 2.4f)); }
static int near(int a, int b, int tolerance) { return abs(a - b) <= tolerance; }

static void frame(const struct fp_tonemap *tm, int w, int h, int y, int u, int v, uint8_t *out_y, uint8_t *out_u,
                  uint8_t *out_v) {
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint16_t *ly = malloc(sizeof(uint16_t) * w * h), *lu = malloc(sizeof(uint16_t) * cw * ch),
             *lv = malloc(sizeof(uint16_t) * cw * ch);
    for (int i = 0; i < w * h; i++) ly[i] = (uint16_t)y;
    for (int i = 0; i < cw * ch; i++) { lu[i] = (uint16_t)u; lv[i] = (uint16_t)v; }
    const uint8_t *src[3] = {(const uint8_t *)ly, (const uint8_t *)lu, (const uint8_t *)lv};
    int ss[3] = {w * 2, cw * 2, cw * 2}, ds[3] = {w, cw, cw};
    uint8_t *dst[3] = {out_y, out_u, out_v};
    fp_tonemap_rows(tm, w, h, 0, ch, src, ss, dst, ds);
    free(ly); free(lu); free(lv);
}

int main(void) {
    static struct fp_tonemap tm;
    fp_tonemap_init(&tm, FP_TM_PQ, 0, 1, 1, 1000);
    assert(tm.peak == 1000);
    assert(tm.luma8[64] == 16 && tm.luma8[0] == 16);                    // black (and below it)
    for (int c = 1; c < 1024; c++) assert(tm.luma8[c] >= tm.luma8[c - 1]);  // never darker for a brighter input
    assert(near(tm.luma8[code_for_nits(50)], sdr_code(50 / 203.0f), 1));     // below the knee: the same level
    int white = tm.luma8[code_for_nits(203)];
    assert(white >= 200 && white <= 235);                                   // reference white, rolled off a little
    assert(tm.luma8[code_for_nits(1000)] == 235);                            // the content's peak = SDR white
    assert(tm.luma8[940] == 235);                                            // above the peak: clipped to white

    // content mastered no brighter than reference white: levels are kept exactly
    fp_tonemap_init(&tm, FP_TM_PQ, 0, 1, 1, 203);
    assert(tm.peak == 400);  // (kept at 400 or above: MaxCLL that low is a mislabelled stream)
    assert(fp_tonemap_peak(FP_TM_PQ, 0) == 1000 && fp_tonemap_peak(FP_TM_PQ, 4000) == 4000);
    assert(fp_tonemap_peak(FP_TM_PQ, 20000) == 10000 && fp_tonemap_peak(FP_TM_HLG, 4000) == 1000);

    // HLG: 75 % signal is reference white on a 1000 cd/m² display (BT.2408), like PQ's 203 cd/m²
    fp_tonemap_init(&tm, FP_TM_HLG, 0, 1, 1, 0);
    for (int c = 1; c < 1024; c++) assert(tm.luma8[c] >= tm.luma8[c - 1]);
    struct fp_tonemap pq;
    fp_tonemap_init(&pq, FP_TM_PQ, 0, 1, 1, 1000);
    assert(near(tm.luma8[(int)lroundf(64 + 876 * 0.75f)], pq.luma8[code_for_nits(203)], 2));

    // grey stays grey and keeps its level, whatever the frame size (odd sizes repeat the last row/column)
    static uint8_t y8[9 * 7], u8[5 * 4], v8[5 * 4];
    for (int size = 0; size < 3; size++) {
        int w = (int[]){4, 3, 9}[size], h = (int[]){2, 3, 7}[size];
        memset(y8, 0xee, sizeof y8); memset(u8, 0xee, sizeof u8); memset(v8, 0xee, sizeof v8);
        int code = code_for_nits(100);
        frame(&pq, w, h, code, 512, 512, y8, u8, v8);
        for (int i = 0; i < w * h; i++) assert(near(y8[i], pq.luma8[code], 1));
        for (int i = 0; i < ((w + 1) / 2) * ((h + 1) / 2); i++) assert(near(u8[i], 128, 1) && near(v8[i], 128, 1));
    }

    // a BT.2020 red beyond BT.709 (R' = 0.58, G' = B' = 0.3) stays red (V well above, U below neutral) and keeps
    // its brightness: moved toward grey, not clipped (clipping its negative green gave luma 66)
    {
        const float kr = 0.2627f, kb = 0.0593f, r = 0.58f, g = 0.3f, b = 0.3f;
        float ye = kr * r + (1 - kr - kb) * g + kb * b, cb = (b - ye) / 1.8814f, cr = (r - ye) / 1.4746f;
        frame(&pq, 2, 2, (int)lroundf(64 + 876 * ye), (int)lroundf(512 + 896 * cb), (int)lroundf(512 + 896 * cr),
              y8, u8, v8);
        assert(v8[0] > 128 + 30 && u8[0] < 128 && y8[0] > 110);
    }
    return 0;
}
