/* The harness's fake libmediandk (fakemedia.c) and the guest test (libbtest4.c) share these. */
#ifndef FAKEMEDIA_H
#define FAKEMEDIA_H

#include <stdint.h>

#define FAKE_NBUF 4
#define FAKE_BUFSZ 8192
#define FAKE_PATTERN1(k) ((uint8_t)((k) * 13 + 1))
#define FAKE_PATTERN2(k) ((uint8_t)((k) * 7 + 3))

struct fake_media_state {
    int buffers_high;          /* the codec's buffers lie above 4 GiB */
    int configured;            /* configure got the guest's format (width 640) and no window/crypto */
    int queued_guest_data;     /* queueInputBuffer calls that carried the guest's bytes */
    int queued_extractor_data; /* ... the extractor's bytes (readSampleData into the guest's buffer) */
    int formats_deleted, codecs_deleted;
    int64_t last_timeout;
    int fd;
    int64_t fd_offset, fd_length, seek_pos;
    int seek_mode;
};
extern struct fake_media_state fake_media;

#endif
