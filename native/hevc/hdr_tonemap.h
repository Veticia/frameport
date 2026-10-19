// SPDX-License-Identifier: GPL-3.0-only
// HDR to SDR for the software Main 10 path (frameport_hevc.cpp): every output of the component is 8-bit BT.709
// SDR, so PQ (HDR10) and HLG pictures are tone mapped and BT.2020 colours are mapped into BT.709 while the 10-bit
// planes are narrowed. Pure C, so the host test (tests/fixtures/src/hdr_tonemap_test.c) can check it.
//
// Brightness follows ITU-R BT.2390's EETF in the PQ domain: HDR reference white (203 cd/m², BT.2408) becomes SDR
// white, darker tones keep their level and highlights up to the content's peak roll off instead of clipping.
// Colour is converted per 2x2 block (one chroma sample): BT.2020 R'G'B' to linear light, scaled by the tone curve
// at the block's luminance (hue kept), BT.2020 to BT.709 primaries, pulled toward the grey of the same luminance
// where it falls outside BT.709 or above white, encoded for a BT.1886 display.
// Each pixel's luma keeps its own detail and is moved by its block's change, so luma and chroma agree.
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

enum { FP_TM_NONE = 0, FP_TM_PQ = 1, FP_TM_HLG = 2 };

#define FP_TM_WHITE 203.0f   // cd/m²: HDR reference white, shown as SDR white
#define FP_TM_STEPS 4095     // the tone curve table, over sqrt(L / peak)
#define FP_TM_ENC 65535      // the display encoding table, over SDR linear light

struct fp_tonemap {
    int transfer, full_range, matrix2020, primaries2020;
    float peak;                        // cd/m² the content reaches (PQ), 1000 for HLG
    float luma[1024];               // 10-bit Y' -> SDR Y' (0..1), the per-pixel curve
    int16_t luma8[1024];            // the same as 8-bit limited-range code values
    float chan[1024];               // E' (0..1 in 1023 steps) -> linear cd/m² / peak
    float ratio[FP_TM_STEPS + 1];   // sqrt(L / peak) -> SDR linear (white = 1) per unit of L / peak
    uint8_t enc[FP_TM_ENC + 1];     // SDR linear (0..1) -> E' * 219 for a BT.1886 display (gamma 2.4)
};

static inline float fp_tm_pq_eotf(float e) {  // PQ signal -> cd/m²
    const float m1 = 2610.0f / 16384, m2 = 2523.0f / 4096 * 128, c1 = 3424.0f / 4096, c2 = 2413.0f / 4096 * 32,
                c3 = 2392.0f / 4096 * 32;
    if (e <= 0) return 0;
    float p = powf(e, 1 / m2), n = p - c1;
    return n <= 0 ? 0 : 10000 * powf(n / (c2 - c3 * p), 1 / m1);
}

static inline float fp_tm_pq_oetf(float l) {  // cd/m² -> PQ signal
    const float m1 = 2610.0f / 16384, m2 = 2523.0f / 4096 * 128, c1 = 3424.0f / 4096, c2 = 2413.0f / 4096 * 32,
                c3 = 2392.0f / 4096 * 32;
    if (l <= 0) return 0;
    float y = powf(l / 10000, m1);
    return powf((c1 + c2 * y) / (1 + c3 * y), m2);
}

static inline float fp_tm_hlg_linear(float e) {  // HLG signal -> display light / peak (OOTF with system gamma 1.2)
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    if (e <= 0) return 0;
    float s = e <= 0.5f ? e * e / 3 : (expf((e - c) / a) + b) / 12;
    return powf(s, 1.2f);
}

// BT.2390 EETF: content luminance (cd/m², up to `peak`) -> SDR luminance relative to white (0..1)
static inline float fp_tm_eetf(float l, float peak) {
    if (peak <= FP_TM_WHITE) return fminf(l / FP_TM_WHITE, 1);
    float w = fp_tm_pq_oetf(peak), e1 = fp_tm_pq_oetf(l) / w, max_lum = fp_tm_pq_oetf(FP_TM_WHITE) / w;
    float ks = fmaxf(1.5f * max_lum - 0.5f, 0), e2 = e1;
    if (e1 >= 1) e2 = max_lum;
    else if (e1 > ks) {
        float t = (e1 - ks) / (1 - ks), t2 = t * t, t3 = t2 * t;
        e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * max_lum;
    }
    return fminf(fp_tm_pq_eotf(e2 * w) / FP_TM_WHITE, 1);
}

static inline float fp_tm_signal_nits(const struct fp_tonemap *tm, float e) {
    return tm->transfer == FP_TM_PQ ? fp_tm_pq_eotf(e) : tm->peak * fp_tm_hlg_linear(e);
}

/** The peak the tables use: PQ content's MaxCLL or mastering peak (0 = unknown -> 1000, kept within 400..10000),
 *  1000 cd/m² for HLG. */
static inline float fp_tonemap_peak(int transfer, float peak) {
    return transfer == FP_TM_HLG || !(peak > 0) ? 1000 : fminf(fmaxf(peak, 400), 10000);
}

/** Build the tables. transfer FP_TM_PQ/FP_TM_HLG; peak in cd/m² (PQ: MaxCLL or the mastering display's maximum,
 *  0 = unknown -> 1000; HLG always uses 1000). */
static inline void fp_tonemap_init(struct fp_tonemap *tm, int transfer, int full_range, int matrix2020,
                                   int primaries2020, float peak) {
    tm->transfer = transfer;
    tm->full_range = full_range;
    tm->matrix2020 = matrix2020;
    tm->primaries2020 = primaries2020;
    tm->peak = fp_tonemap_peak(transfer, peak);
    for (int i = 0; i < 1024; i++) tm->chan[i] = fp_tm_signal_nits(tm, i / 1023.0f) / tm->peak;
    for (int code = 0; code < 1024; code++) {
        float e = full_range ? code / 1023.0f : (code - 64) / 876.0f;
        e = fminf(fmaxf(e, 0), 1);
        tm->luma[code] = powf(fp_tm_eetf(fp_tm_signal_nits(tm, e), tm->peak), 1 / 2.4f);
        tm->luma8[code] = (int16_t)(16 + 219 * tm->luma[code] + 0.5f);
    }
    for (int i = 0; i <= FP_TM_STEPS; i++) {
        float s = (float)i / FP_TM_STEPS, rel = s * s;  // L / peak
        tm->ratio[i] = i ? fp_tm_eetf(rel * tm->peak, tm->peak) / rel : tm->peak / FP_TM_WHITE;
    }
    for (int i = 0; i <= FP_TM_ENC; i++) tm->enc[i] = (uint8_t)(219 * powf((float)i / FP_TM_ENC, 1 / 2.4f) + 0.5f);
}

static inline float fp_tm_clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static inline int fp_tm_index(float x01, int steps) { return (int)(fp_tm_clamp01(x01) * steps + 0.5f); }
static inline uint8_t fp_tm_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

#define FP_TM_CHUNK 128  // blocks per pass: the passes alternate plain arithmetic (NEON) and table lookups

/** Tone map chroma rows [crow0, crow1) of a 10-bit 4:2:0 picture (and the luma rows they cover) into 8-bit 4:2:0
 *  limited range BT.709. Strides are in bytes. Rows are independent, so callers may split a picture into bands. */
static inline void fp_tonemap_rows(const struct fp_tonemap *tm, int width, int height, int crow0, int crow1,
                                   const uint8_t *const src[3], const int src_stride[3],
                                   uint8_t *const dst[3], const int dst_stride[3]) {
    const float kr = tm->matrix2020 ? 0.2627f : 0.2126f, kb = tm->matrix2020 ? 0.0593f : 0.0722f;
    const float kg = 1 - kr - kb, inv_kg = 1 / kg, cr_r = 2 - 2 * kr, cb_b = 2 - 2 * kb;
    const float yscale = tm->full_range ? 1023.0f : 876.0f, yoff = tm->full_range ? 0 : 64;
    const float cscale = tm->full_range ? 1023.0f : 896.0f;
    // BT.2020 -> BT.709 primaries in linear light (ITU-R BT.2087)
    static const float m2020[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f,
                                   -0.0182f, -0.1006f, 1.1187f};
    static const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float *m = tm->primaries2020 ? m2020 : ident;
    const int cw = (width + 1) / 2, pairs = width / 2;
    int c0[FP_TM_CHUNK], c1[FP_TM_CHUNK], c2[FP_TM_CHUNK], c3[FP_TM_CHUNK], ir[FP_TM_CHUNK], ig[FP_TM_CHUNK],
        ib[FP_TM_CHUNK], shift[FP_TM_CHUNK];
    float fr[FP_TM_CHUNK], fg[FP_TM_CHUNK], fb[FP_TM_CHUNK], fk[FP_TM_CHUNK];
    for (int cy = crow0; cy < crow1; cy++) {
        const int y0 = cy * 2, y1 = y0 + 1 < height ? y0 + 1 : y0;
        const uint16_t *l0 = (const uint16_t *)(src[0] + (size_t)y0 * src_stride[0]);
        const uint16_t *l1 = (const uint16_t *)(src[0] + (size_t)y1 * src_stride[0]);
        const uint16_t *us = (const uint16_t *)(src[1] + (size_t)cy * src_stride[1]);
        const uint16_t *vs = (const uint16_t *)(src[2] + (size_t)cy * src_stride[2]);
        uint8_t *d0 = dst[0] + (size_t)y0 * dst_stride[0], *d1 = dst[0] + (size_t)y1 * dst_stride[0];
        uint8_t *du = dst[1] + (size_t)cy * dst_stride[1], *dv = dst[2] + (size_t)cy * dst_stride[2];
        for (int start = 0; start < cw; start += FP_TM_CHUNK) {
            const int n = cw - start < FP_TM_CHUNK ? cw - start : FP_TM_CHUNK;
            const int full = pairs - start < n ? (pairs - start > 0 ? pairs - start : 0) : n;
            // the block's four luma codes (an odd width's last block repeats its column)
            for (int i = 0; i < full; i++) {
                const int x = (start + i) * 2;
                c0[i] = l0[x] & 1023; c1[i] = l0[x + 1] & 1023; c2[i] = l1[x] & 1023; c3[i] = l1[x + 1] & 1023;
            }
            for (int i = full; i < n; i++) {
                const int x = (start + i) * 2;
                c0[i] = c1[i] = l0[x] & 1023; c2[i] = c3[i] = l1[x] & 1023;
            }
            // block R'G'B' (0..1) -> table indices
            for (int i = 0; i < n; i++) {
                float ye = ((c0[i] + c1[i] + c2[i] + c3[i]) * 0.25f - yoff) / yscale;
                float cb = ((us[start + i] & 1023) - 512) / cscale, cr = ((vs[start + i] & 1023) - 512) / cscale;
                float r = ye + cr_r * cr, b = ye + cb_b * cb, g = (ye - kr * r - kb * b) * inv_kg;
                ir[i] = fp_tm_index(r, 1023); ig[i] = fp_tm_index(g, 1023); ib[i] = fp_tm_index(b, 1023);
            }
            for (int i = 0; i < n; i++) { fr[i] = tm->chan[ir[i]]; fg[i] = tm->chan[ig[i]]; fb[i] = tm->chan[ib[i]]; }
            // linear light / peak -> the tone curve's index at the block's luminance
            for (int i = 0; i < n; i++) ir[i] = fp_tm_index(sqrtf(kr * fr[i] + kg * fg[i] + kb * fb[i]), FP_TM_STEPS);
            for (int i = 0; i < n; i++) fk[i] = tm->ratio[ir[i]];
            // scaled by the tone curve (hue kept), BT.709 primaries; colours outside BT.709 or above white move
            // toward the grey of the same luminance until they fit (clipping a channel would darken them)
            for (int i = 0; i < n; i++) {
                float r = fr[i], g = fg[i], b = fb[i], k = fk[i];
                float R = (m[0] * r + m[1] * g + m[2] * b) * k, G = (m[3] * r + m[4] * g + m[5] * b) * k,
                      B = (m[6] * r + m[7] * g + m[8] * b) * k;
                float y = fp_tm_clamp01(0.2126f * R + 0.7152f * G + 0.0722f * B);
                float lo = R < G ? (R < B ? R : B) : (G < B ? G : B), hi = R > G ? (R > B ? R : B) : (G > B ? G : B);
                float t = 1;
                if (lo < 0) t = y / (y - lo);
                if (hi > 1 && (1 - y) < t * (hi - y)) t = (1 - y) / (hi - y);
                ir[i] = fp_tm_index(y + t * (R - y), FP_TM_ENC);
                ig[i] = fp_tm_index(y + t * (G - y), FP_TM_ENC);
                ib[i] = fp_tm_index(y + t * (B - y), FP_TM_ENC);
            }
            for (int i = 0; i < n; i++) { ir[i] = tm->enc[ir[i]]; ig[i] = tm->enc[ig[i]]; ib[i] = tm->enc[ib[i]]; }
            // chroma, and how much the colour work moved the block's luma from the per-pixel curve
            for (int i = 0; i < n; i++) {
                float er = (float)ir[i], eg = (float)ig[i], eb = (float)ib[i];  // E' * 219
                float ey = 0.2126f * er + 0.7152f * eg + 0.0722f * eb;
                du[start + i] = fp_tm_u8((int)(128.5f + (224.0f / 219 / 1.8556f) * (eb - ey)));
                dv[start + i] = fp_tm_u8((int)(128.5f + (224.0f / 219 / 1.5748f) * (er - ey)));
                shift[i] = (int)(16.5f + ey);
            }
            for (int i = 0; i < n; i++) {
                c0[i] = tm->luma8[c0[i]]; c1[i] = tm->luma8[c1[i]]; c2[i] = tm->luma8[c2[i]]; c3[i] = tm->luma8[c3[i]];
            }
            // each pixel keeps its own curve value, moved with its block
            for (int i = 0; i < full; i++) {
                const int x = (start + i) * 2, sh = shift[i] - ((c0[i] + c1[i] + c2[i] + c3[i] + 2) >> 2);
                d0[x] = fp_tm_u8(c0[i] + sh); d0[x + 1] = fp_tm_u8(c1[i] + sh);
                d1[x] = fp_tm_u8(c2[i] + sh); d1[x + 1] = fp_tm_u8(c3[i] + sh);
            }
            for (int i = full; i < n; i++) {
                const int x = (start + i) * 2, sh = shift[i] - ((c0[i] + c2[i] + 1) >> 1);
                d0[x] = fp_tm_u8(c0[i] + sh); d1[x] = fp_tm_u8(c2[i] + sh);
            }
        }
    }
}
