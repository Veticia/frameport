// SPDX-License-Identifier: GPL-3.0-only
// Turnip's bin (GMEM tile) size for a foveated pass, as Mesa's freedreno Vulkan driver picks it (GitHub #394).
//
// Turnip reads the density map once per bin, at the bin's centre, on a grid that stays anchored at 0 in map space
// when density-map offsets move the bins (tu_calc_frag_area / tu_bin_offset: a screen bin centred at s samples the map
// at s - offset). So the full-density area is the set of bins whose map-space centre falls inside Valve's sharp texels,
// not the texels themselves: its centre is off the texel centre by up to half a bin in each axis (In Death at
// 2768x2768: 192x224 bins, -80,+32 px). The centring layer (centre.h) needs the bin size to put that set on the eye.
//
// Vulkan doesn't report it (Turnip has no VK_QCOM_tile_properties), so this follows Mesa's own code: SteamOS 0.5.x
// ships Mesa 26.3.0-devel (git-e07916b45f, a Valve build: logged as the GL renderer). That commit isn't public; the
// functions below follow upstream main of the 26.3 development window (82b868960e, 2026-09-23), where they didn't
// change in substance between 26.2's branch point and 27.0's:
//   tu_render_pass_gmem_config() (tu_pass.cc): GMEM split between the pass's attachments -> gmem_pixels
//   tu_tiling_config_update_tile_layout() (tu_util.cc): the tile size with the fewest bins for that budget
//   fdl6_lrz_get_max_fdm_extra_size() (freedreno_lrz_layout.h): a depth image with the density-map-offset flag caps
//   the tile size so LRZ fast clears still cover it
//   tu_choose_gmem_layout() (tu_clear_blit.cc): passes with a density map use TU_GMEM_LAYOUT_AVOID_CCU
//   fd6_calc_gmem_cache_offsets() (fd6_gmem_cache.h) + freedreno_devices.py: the GPU's numbers (Adreno 750 only)
// Anything this doesn't model (MSRTSS, custom resolves, unknown formats, other GPUs) returns 0: the layer then keeps
// the texel centre (the behaviour before), and the per-game settings fdm_bin_w/fdm_bin_h can give the size by hand.

typedef struct {
    uint32_t gmem_bytes;                   // GMEM size (KGSL/MSM report it to Turnip)
    uint32_t usable_avoid_ccu;             // GMEM left for attachments with TU_GMEM_LAYOUT_AVOID_CCU
    uint32_t tile_align_w, tile_align_h;   // fd_dev_info tile_align_w/h
    uint32_t tile_max_w, tile_max_h;       // fd_dev_info tile_max_w/h
    uint32_t gmem_align_w, gmem_align_h;   // fd_dev_info gmem_align_w/h (A6xxGPUInfo: 16, 4)
    uint32_t lrz_fc_size;                  // fd_lrzfc_layout<A7XX>::FC_SIZE
} tb_gpu;

// Adreno 750 (FD750, a7xx gen3): 3 MiB GMEM; gen3 keeps 6 x 0xc000 for the VPC attribute buffer at the end of GMEM
// and the colour CCU (6 CCUs x 64 KiB / 8 in GMEM mode) below it.
static const tb_gpu TB_ADRENO_750 = {3u << 20, (3u << 20) - 6 * 0xc000 - 6 * 64 * 1024 / 8, 96, 32, 2016, 2032,
                                     16, 4, 1024};

// One attachment of the pass as Turnip sees it
typedef struct {
    uint32_t cpp;          // bytes per pixel = format block size x samples (D32S8: 4 x samples, stencil separate)
    uint32_t samples;
    int d32s8;             // VK_FORMAT_D32_SFLOAT_S8_UINT
    int gmem;              // used as a colour or depth/stencil attachment (input-only and resolve targets aren't)
    uint32_t first, last;  // subpass range
} tb_att;

#define TB_MAX_ATTS 16

typedef struct { uint32_t gmem_pixels, tile_align_w, min_cpp; } tb_pass;

// tu_render_pass_gmem_config() for TU_GMEM_LAYOUT_AVOID_CCU. 0: impossible (Turnip renders in sysmem then).
static int tb_pass_config(const tb_gpu *g, const tb_att *atts, uint32_t n, tb_pass *out) {
    if (!g || !n || n > TB_MAX_ATTS) return 0;
    uint32_t block_align_shift = 3, tile_align_w = g->tile_align_w;
    const uint32_t gmem_align = (1u << block_align_shift) * tile_align_w * g->tile_align_h;
    struct { uint32_t cpp, first, last; } allocs[2 * TB_MAX_ATTS];
    uint32_t nalloc = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const tb_att *a = &atts[i];
        if (!a->gmem) continue;
        int cpp1 = a->cpp == 1;
        for (int part = 0; part < (a->d32s8 ? 2 : 1); ++part) {
            uint32_t cpp = part ? a->samples : a->cpp;
            // tu_gmem_alloc(): reuse an allocation free during this subpass range, the same cpp or the closest larger
            int found = -1;
            for (uint32_t k = 0; k < nalloc; ++k) {
                if (!(allocs[k].first > a->last || allocs[k].last < a->first)) continue;
                if (allocs[k].cpp == cpp) { found = (int)k; break; }
                if (allocs[k].cpp > cpp && (found < 0 || allocs[found].cpp > allocs[k].cpp)) found = (int)k;
            }
            if (found >= 0) {
                if (a->first < allocs[found].first) allocs[found].first = a->first;
                if (a->last > allocs[found].last) allocs[found].last = a->last;
            } else {
                allocs[nalloc].cpp = cpp, allocs[nalloc].first = a->first, allocs[nalloc].last = a->last;
                ++nalloc;
            }
        }
        if (a->d32s8) cpp1 = a->samples == 1;
        if (cpp1 && tile_align_w % 64 != 0) tile_align_w *= 2, block_align_shift -= 1;
    }
    uint32_t cpp_total = 0, min_cpp = UINT32_MAX;
    for (uint32_t k = 0; k < nalloc; ++k) {
        cpp_total += allocs[k].cpp;
        if (allocs[k].cpp < min_cpp) min_cpp = allocs[k].cpp;
    }
    out->tile_align_w = tile_align_w, out->min_cpp = min_cpp;
    if (!cpp_total) {
        out->gmem_pixels = 1024 * 1024;
        return 1;
    }
    uint32_t gmem_blocks = g->usable_avoid_ccu / gmem_align, pixels = UINT32_MAX;
    for (uint32_t k = 0; k < nalloc; ++k) {
        uint32_t align = allocs[k].cpp >> block_align_shift;
        if (align < 1) align = 1;
        uint32_t nblocks = (uint32_t)((uint64_t)gmem_blocks * allocs[k].cpp / cpp_total) & ~(align - 1);
        if (nblocks < align) nblocks = align;
        if (nblocks > gmem_blocks) return 0;
        gmem_blocks -= nblocks;
        cpp_total -= allocs[k].cpp;
        uint32_t p = nblocks * gmem_align / allocs[k].cpp;
        if (p < pixels) pixels = p;
    }
    out->gmem_pixels = pixels;
    return 1;
}

static uint32_t tb_align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// fdl6_lrz_get_max_fdm_extra_size() (a7xx path): the largest tile a depth image with the density-map-offset flag
// allows, so its LRZ fast-clear buffer still covers it.
static int tb_lrz_covered(const tb_gpu *g, uint32_t w, uint32_t h, uint32_t samples, uint32_t layers) {
    switch (samples) {  // fdl6_lrz_get_super_sampled_size()
    case 8: h *= 2; /* fallthrough */
    case 4: w *= 2; /* fallthrough */
    case 2: h *= 2; break;
    default: break;
    }
    uint32_t pitch = tb_align((w + 7) / 8, 32), height = tb_align((h + 7) / 8, 32);
    uint32_t layer_size = pitch * height * 2;
    uint32_t full = tb_align(layer_size >> 7, 512) / 8 * layers;
    return full <= g->lrz_fc_size;
}
static void tb_lrz_constraint(const tb_gpu *g, uint32_t w, uint32_t h, uint32_t samples, uint32_t layers,
                              uint32_t *max_w, uint32_t *max_h) {
    *max_w = g->tile_max_w, *max_h = g->tile_max_h;
    if (!tb_lrz_covered(g, w, h, samples, layers)) return;
    uint32_t extra = g->tile_max_w < g->tile_max_h ? g->tile_max_w : g->tile_max_h;
    uint32_t step = g->gmem_align_w < g->gmem_align_h ? g->gmem_align_w : g->gmem_align_h;
    uint32_t min_extra = step > 192 ? step : 192;  // MIN_TILE_SIZE_FOR_FDM_OFFSET
    while (extra > min_extra) {
        if (tb_lrz_covered(g, w + extra, h + extra, samples, layers)) {
            *max_w = extra / g->gmem_align_w * g->gmem_align_w;
            *max_h = extra / g->gmem_align_h * g->gmem_align_h;
            return;
        }
        extra -= step;
    }
}

// tu_tiling_config_update_tile_layout() for a pass with a density map. max_w/max_h: the framebuffer's constraint
// (~0u: none). 0: no GMEM tiling possible.
static int tb_tile_size(const tb_gpu *g, const tb_pass *p, uint32_t fb_w, uint32_t fb_h, uint32_t layers,
                        uint32_t max_w_c, uint32_t max_h_c, uint32_t *tile_w, uint32_t *tile_h) {
    if (!g || !p || !p->gmem_pixels || !fb_w || !fb_h || !layers || !p->tile_align_w) return 0;
    uint32_t align_w = p->tile_align_w, align_h = g->tile_align_h;
    if (layers > 1 && p->min_cpp && p->min_cpp != UINT32_MAX) {
        uint32_t stride = align_h * align_w * p->min_cpp;
        if (stride % 4096) {  // the layer stride must be a multiple of 4 KiB: grow tile_align_h
            uint32_t tz = 0;
            while (!((stride >> tz) & 1)) ++tz;
            align_h <<= 12 - tz;
        }
    }
    uint32_t max_w = g->tile_max_w, max_h = g->tile_max_h;
    if (tb_align(fb_w, align_w) < max_w) max_w = tb_align(fb_w, align_w);
    if (max_w_c < max_w) max_w = max_w_c;
    if (tb_align(fb_h, align_h) < max_h) max_h = tb_align(fb_h, align_h);
    if (max_h_c < max_h) max_h = max_h_c;
    uint32_t best = UINT32_MAX, bw = UINT32_MAX, bh = UINT32_MAX;
    int possible = 0;
    for (uint32_t w = align_w; w <= max_w; w += align_w) {
        uint32_t h = p->gmem_pixels / (w * layers);
        if (h > max_h) h = max_h;
        h = h / align_h * align_h;
        if (!h) continue;
        uint32_t penalty = (w > h * 2 || h > w * 2) ? 1000 : 0;  // FDM wants roughly square bins
        uint32_t cw = (fb_w + w - 1) / w, ch = (fb_h + h - 1) / h;
        h = tb_align((fb_h + ch - 1) / ch, align_h);  // spread the rows evenly
        uint32_t total = cw * ch + penalty;
        int d_new = (int)w - (int)h, d_old = (int)bw - (int)bh;
        if (total < best || (total == best && abs(d_new) < abs(d_old))) {
            possible = 1, best = total, bw = w, bh = h;
        }
    }
    if (!possible) return 0;
    *tile_w = bw, *tile_h = bh;
    return 1;
}

// Where the bins Turnip renders at full density are centred along one axis, in map-space pixels: the bins whose
// centre (bin / 2 + j * bin) lies in the sharp texels [lo, hi). Negative: no bin centre falls inside.
static float tb_bins_centre(uint32_t lo, uint32_t hi, uint32_t bin) {
    if (!bin || hi <= lo) return -1.0f;
    uint32_t half = bin / 2;
    uint32_t jmin = lo > half ? (lo - half + bin - 1) / bin : 0;
    if (hi <= half) return -1.0f;
    uint32_t jmax = (hi - 1 - half) / bin;
    if (jmax < jmin) return -1.0f;
    return (float)(jmin * bin + (jmax + 1) * bin) / 2.0f;
}

// Bytes per pixel of the formats render targets use (vk_format_get_blocksize); 0: not modelled.
static uint32_t tb_format_size(VkFormat f) {
    switch ((int)f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_SRGB: case VK_FORMAT_S8_UINT:
        return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return 4;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 0;
    }
}

// An attachment for tb_pass_config from its format and sample count; 0: format not modelled.
static int tb_att_of(VkFormat format, uint32_t samples, tb_att *a) {
    memset(a, 0, sizeof(*a));
    if (!samples) samples = 1;
    a->samples = samples;
    if (format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        a->d32s8 = 1, a->cpp = 4 * samples;
        return 1;
    }
    uint32_t size = tb_format_size(format);
    a->cpp = size * samples;
    return size != 0;
}
