// SPDX-License-Identifier: GPL-3.0-only
// HEVC profile of a stream's parameter sets, to keep streams Iris can't decode
// into NV12 on the software path (frameport_hevc.cpp). Pure, so the host test
// (tests/fixtures/src/hevc_profile_test.c) can check it.
#pragma once
#include <stddef.h>
#include <stdint.h>

// general_profile_idc of the first SPS (1 Main, 2 Main 10, 3 Main Still
// Picture, 4+ range extensions) in Annex B data or an hvcC record, or -1.
// The field precedes any possible emulation-prevention byte.
static inline int hevc_profile(const uint8_t *data, size_t size) {
    if (size >= 2 && data[0] == 1) return data[1] & 0x1f;  // hvcC: configurationVersion, then profile space/tier/idc
    for (size_t i = 0; i + 7 <= size; i++)
        if (!data[i] && !data[i + 1] && data[i + 2] == 1 && ((data[i + 3] >> 1) & 0x3f) == 33)
            return data[i + 6] & 0x1f;
    return -1;
}

// Iris decodes 8-bit Main (and Main Still Picture) into NV12; a stream with no
// profile found keeps trying it.
static inline int hevc_needs_software(int profile) {
    return profile > 1 && profile != 3;
}
