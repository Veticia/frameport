/* A fake libmediandk for the harness: formats, a "decoder" that XORs its input into its output buffers and an
 * extractor that writes a known pattern. Everything it hands out lies above 4 GiB, like a real codec's buffers in
 * the host's heap, so the guest only ever sees the bridge's mirrors and copies. */
#define _GNU_SOURCE
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "fakemedia.h"

struct fake_media_state fake_media;

/* bump allocator in a mapping well above 4 GiB */
static void *hi_alloc(size_t n)
{
    static uint8_t *base, *cur, *end;
    if (!base) {
        base = mmap((void *)0x6000000000ull, 16 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) abort();
        cur = base, end = base + (16 << 20);
    }
    n = (n + 15) & ~(size_t)15;
    if (cur + n > end) abort();
    void *p = cur;
    cur += n;
    memset(p, 0, n);
    return p;
}

/* ---------------------------------------------------------------- AMediaFormat */

enum { T_I32, T_I64, T_FLOAT, T_STR, T_BUF };

struct AMediaFormat {
    struct {
        char name[32];
        int type;
        int64_t i;
        float f;
        char *s;
        void *b;
        size_t n;
    } e[16];
    int n;
    char *text;
};

static AMediaFormat *fmt_new(void) { return hi_alloc(sizeof(AMediaFormat)); }

static int fmt_slot(AMediaFormat *f, const char *name, int type, int create)
{
    for (int k = 0; k < f->n; k++)
        if (!strcmp(f->e[k].name, name)) return f->e[k].type == type || create ? k : -1;
    if (!create || f->n == 16) return -1;
    snprintf(f->e[f->n].name, sizeof f->e[0].name, "%s", name);
    f->e[f->n].type = type;
    return f->n++;
}

AMediaFormat *AMediaFormat_new(void) { return fmt_new(); }
media_status_t AMediaFormat_delete(AMediaFormat *f)
{
    fake_media.formats_deleted++;
    return AMEDIA_OK;
}

const char *AMediaFormat_toString(AMediaFormat *f)
{
    f->text = hi_alloc(512);
    int o = 0;
    for (int k = 0; k < f->n && o < 400; k++)
        o += snprintf(f->text + o, 512 - (size_t)o, "%s%s", k ? ", " : "",
                      f->e[k].type == T_STR ? f->e[k].s : f->e[k].name);
    return f->text;
}

void AMediaFormat_setInt32(AMediaFormat *f, const char *name, int32_t v)
{
    int k = fmt_slot(f, name, T_I32, 1);
    if (k >= 0) f->e[k].type = T_I32, f->e[k].i = v;
}

void AMediaFormat_setInt64(AMediaFormat *f, const char *name, int64_t v)
{
    int k = fmt_slot(f, name, T_I64, 1);
    if (k >= 0) f->e[k].type = T_I64, f->e[k].i = v;
}

void AMediaFormat_setFloat(AMediaFormat *f, const char *name, float v)
{
    int k = fmt_slot(f, name, T_FLOAT, 1);
    if (k >= 0) f->e[k].type = T_FLOAT, f->e[k].f = v;
}

void AMediaFormat_setString(AMediaFormat *f, const char *name, const char *v)
{
    int k = fmt_slot(f, name, T_STR, 1);
    if (k < 0) return;
    f->e[k].type = T_STR;
    f->e[k].s = hi_alloc(strlen(v) + 1);
    strcpy(f->e[k].s, v);
}

void AMediaFormat_setBuffer(AMediaFormat *f, const char *name, const void *data, size_t n)
{
    int k = fmt_slot(f, name, T_BUF, 1);
    if (k < 0) return;
    f->e[k].type = T_BUF;
    f->e[k].b = hi_alloc(n);
    memcpy(f->e[k].b, data, n);
    f->e[k].n = n;
}

bool AMediaFormat_getInt32(AMediaFormat *f, const char *name, int32_t *out)
{
    int k = fmt_slot(f, name, T_I32, 0);
    if (k >= 0) *out = (int32_t)f->e[k].i;
    return k >= 0;
}

bool AMediaFormat_getInt64(AMediaFormat *f, const char *name, int64_t *out)
{
    int k = fmt_slot(f, name, T_I64, 0);
    if (k >= 0) *out = f->e[k].i;
    return k >= 0;
}

bool AMediaFormat_getFloat(AMediaFormat *f, const char *name, float *out)
{
    int k = fmt_slot(f, name, T_FLOAT, 0);
    if (k >= 0) *out = f->e[k].f;
    return k >= 0;
}

bool AMediaFormat_getString(AMediaFormat *f, const char *name, const char **out)
{
    int k = fmt_slot(f, name, T_STR, 0);
    if (k >= 0) *out = f->e[k].s;
    return k >= 0;
}

bool AMediaFormat_getBuffer(AMediaFormat *f, const char *name, void **data, size_t *n)
{
    int k = fmt_slot(f, name, T_BUF, 0);
    if (k >= 0) *data = f->e[k].b, *n = f->e[k].n;
    return k >= 0;
}

/* ---------------------------------------------------------------- AMediaCodec */

struct AMediaCodec {
    uint8_t *in[FAKE_NBUF], *out[FAKE_NBUF];
    int in_busy[FAKE_NBUF];
    struct {
        int pending;
        AMediaCodecBufferInfo info;
    } outq[FAKE_NBUF];
};

AMediaCodec *AMediaCodec_createDecoderByType(const char *mime)
{
    if (strcmp(mime, "video/avc")) return NULL;
    AMediaCodec *c = hi_alloc(sizeof *c);
    for (int k = 0; k < FAKE_NBUF; k++) {
        c->in[k] = hi_alloc(FAKE_BUFSZ);
        c->out[k] = hi_alloc(FAKE_BUFSZ);
    }
    fake_media.buffers_high = (uintptr_t)c->in[0] >= 0x100000000ull;
    return c;
}

media_status_t AMediaCodec_configure(AMediaCodec *c, const AMediaFormat *f, ANativeWindow *w, AMediaCrypto *crypto,
                                     uint32_t flags)
{
    int32_t width = 0;
    fake_media.configured = f && AMediaFormat_getInt32((AMediaFormat *)f, "width", &width) && width == 640 && !w &&
                            !crypto;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_start(AMediaCodec *c) { return AMEDIA_OK; }

media_status_t AMediaCodec_delete(AMediaCodec *c)
{
    fake_media.codecs_deleted++;
    return AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c, int64_t timeout)
{
    fake_media.last_timeout = timeout;
    for (int k = 0; k < FAKE_NBUF; k++)
        if (!c->in_busy[k]) {
            c->in_busy[k] = 1;
            return k;
        }
    return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
}

uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *c, size_t idx, size_t *size)
{
    if (idx >= FAKE_NBUF) return NULL;
    *size = FAKE_BUFSZ;
    return c->in[idx];
}

/* the queued bytes must be what the guest wrote: pattern 1 (the guest's own) or 2 (the fake extractor's) */
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c, size_t idx, off_t off, size_t size, uint64_t time,
                                            uint32_t flags)
{
    if (idx >= FAKE_NBUF || off < 0 || (size_t)off + size > FAKE_BUFSZ) return AMEDIA_ERROR_INVALID_PARAMETER;
    int p1 = 1, p2 = 1;
    for (size_t k = 0; k < size; k++) {
        p1 &= c->in[idx][off + k] == FAKE_PATTERN1(k);
        p2 &= c->in[idx][off + k] == FAKE_PATTERN2(k);
    }
    if (size && p1) fake_media.queued_guest_data++;
    if (size && p2) fake_media.queued_extractor_data++;
    c->in_busy[idx] = 0;
    for (size_t k = 0; k < size; k++) c->out[idx][8 + k] = c->in[idx][off + k] ^ 0x5a;
    c->outq[idx].pending = 1;
    c->outq[idx].info.offset = 8;
    c->outq[idx].info.size = (int32_t)size;
    c->outq[idx].info.presentationTimeUs = (int64_t)time;
    c->outq[idx].info.flags = flags;
    return AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c, AMediaCodecBufferInfo *info, int64_t timeout)
{
    for (int k = 0; k < FAKE_NBUF; k++)
        if (c->outq[k].pending) {
            c->outq[k].pending = 0;
            *info = c->outq[k].info;
            return k;
        }
    return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
}

uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *c, size_t idx, size_t *size)
{
    if (idx >= FAKE_NBUF) return NULL;
    *size = FAKE_BUFSZ;
    return c->out[idx];
}

media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *c, size_t idx, bool render)
{
    return idx < FAKE_NBUF ? AMEDIA_OK : AMEDIA_ERROR_INVALID_PARAMETER;
}

AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *c)
{
    AMediaFormat *f = fmt_new();
    AMediaFormat_setInt32(f, "width", 1920);
    return f;
}

/* ---------------------------------------------------------------- AMediaExtractor */

struct AMediaExtractor {
    int unused;
};

AMediaExtractor *AMediaExtractor_new(void) { return hi_alloc(sizeof(AMediaExtractor)); }
media_status_t AMediaExtractor_delete(AMediaExtractor *e) { return AMEDIA_OK; }

media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor *e, int fd, off64_t offset, off64_t length)
{
    fake_media.fd = fd, fake_media.fd_offset = offset, fake_media.fd_length = length;
    return AMEDIA_OK;
}

media_status_t AMediaExtractor_seekTo(AMediaExtractor *e, int64_t pos, SeekMode mode)
{
    fake_media.seek_pos = pos, fake_media.seek_mode = (int)mode;
    return AMEDIA_OK;
}

int64_t AMediaExtractor_getSampleTime(AMediaExtractor *e) { return 0x123456789ll; }

ssize_t AMediaExtractor_readSampleData(AMediaExtractor *e, uint8_t *buf, size_t cap)
{
    if (cap < 100) return -1;
    for (int k = 0; k < 100; k++) buf[k] = FAKE_PATTERN2(k);
    return 100;
}
