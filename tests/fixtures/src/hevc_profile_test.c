// SPDX-License-Identifier: GPL-3.0-only
// HEVC profile detection that keeps Main 10 off Iris's NV12 path (native/hevc/hevc_profile.h).
// Byte strings are from x265 encodes: Annex B VPS + SPS starts, and the start of an MP4 hvcC record.
#include <assert.h>
#include "hevc_profile.h"

int main(void) {
    // VPS (type 32, carries a profile too: must be skipped), then the SPS (type 33)
    static const uint8_t main8[] = {0, 0, 0, 1, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x02, 0x60,
                                    0, 0, 0, 1, 0x42, 0x01, 0x01, 0x01, 0x60, 0, 0, 3, 0, 0x90, 0};
    static const uint8_t main10[] = {0, 0, 1, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60,
                                     0, 0, 1, 0x42, 0x01, 0x01, 0x02, 0x20, 0, 0, 3, 0, 0x90, 0};
    static const uint8_t hvcc_main8[] = {0x01, 0x01, 0x60, 0, 0, 0, 0x90};
    static const uint8_t hvcc_main10[] = {0x01, 0x02, 0x20, 0, 0, 0, 0x90};
    static const uint8_t slice_only[] = {0, 0, 0, 1, 0x26, 0x01, 0xaf, 0x08, 0x42, 0x01, 0x02};
    static const uint8_t short_sps[] = {0, 0, 1, 0x42, 0x01, 0x01};

    assert(hevc_profile(main8, sizeof main8) == 1);
    assert(hevc_profile(main10, sizeof main10) == 2);
    assert(hevc_profile(hvcc_main8, sizeof hvcc_main8) == 1);
    assert(hevc_profile(hvcc_main10, sizeof hvcc_main10) == 2);
    assert(hevc_profile(slice_only, sizeof slice_only) == -1);  // no parameter sets in this access unit
    assert(hevc_profile(short_sps, sizeof short_sps) == -1);    // truncated: no profile byte
    assert(hevc_profile(main8, 0) == -1);

    assert(!hevc_needs_software(1));   // Main: Iris
    assert(!hevc_needs_software(3));   // Main Still Picture: Iris
    assert(!hevc_needs_software(-1));  // unknown: try Iris as before
    assert(hevc_needs_software(2));    // Main 10: software
    assert(hevc_needs_software(4));    // range extensions (4:2:2, 4:4:4, 12-bit): software
    return 0;
}
