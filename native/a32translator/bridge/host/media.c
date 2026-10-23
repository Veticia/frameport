/* NDK media (libmediandk) functions that gen_stubs.py can't generate (imports.txt "!" lines): host memory the guest
 * must read or write.
 *
 * - Strings and buffers an AMediaFormat owns (getString, getBuffer, toString) get guest copies, freed with the format
 *   (not guest_cstr's cache: a deleted format's memory comes back for another string).
 * - Codec buffers (getInputBuffer/getOutputBuffer) lie in the host's heap, above 4 GiB. The guest gets a mirror per
 *   (codec, input/output, index) in its own memory: an input mirror goes to the codec's buffer at queueInputBuffer
 *   (the range queued), an output buffer comes into its mirror at getOutputBuffer (the range dequeueOutputBuffer
 *   reported, else all of it). Unity's video player fills input buffers with AMediaExtractor_readSampleData, which
 *   then writes straight into the mirror (guest memory is host memory). Mirrors go with AMediaCodec_delete. */
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <stddef.h>
#include <stdlib.h>
#include "imports_internal.h"

#define W(f) extern __typeof__(f) f __attribute__((weak))
W(AMediaFormat_delete); W(AMediaFormat_toString); W(AMediaFormat_getString); W(AMediaFormat_getBuffer);
W(AMediaCodec_delete); W(AMediaCodec_getInputBuffer); W(AMediaCodec_queueInputBuffer);
W(AMediaCodec_dequeueOutputBuffer); W(AMediaCodec_getOutputBuffer);
#undef W

/* the guest's AMediaCodecBufferInfo (ARM EABI: int64_t 8-byte aligned) has the host's layout: passed through */
_Static_assert(sizeof(AMediaCodecBufferInfo) == 24 && offsetof(AMediaCodecBufferInfo, presentationTimeUs) == 8 &&
               offsetof(AMediaCodecBufferInfo, flags) == 16, "AMediaCodecBufferInfo layout");

/* ---------------------------------------------------------------- AMediaFormat */

typedef struct fcopy {
    struct fcopy *next;
    AMediaFormat *format;
    const void *host; /* the format's own memory this copies */
    uint32_t size;
    uint32_t g;
} fcopy;

static pthread_mutex_t fcopy_lock = PTHREAD_MUTEX_INITIALIZER;
static fcopy *fcopies;

/* a guest copy of n bytes of format f's memory p (the earlier copy if it still matches) */
static uint32_t format_copy(AMediaFormat *f, const void *p, size_t n)
{
    if (in_guest_reach(p, n)) return p2g(p);
    pthread_mutex_lock(&fcopy_lock);
    for (fcopy *k = fcopies; k; k = k->next)
        if (k->format == f && k->host == p && k->size == n && !memcmp(g2p(k->g), p, n)) {
            uint32_t g = k->g;
            pthread_mutex_unlock(&fcopy_lock);
            return g;
        }
    pthread_mutex_unlock(&fcopy_lock);
    uint32_t g = guest_malloc(n ? (uint32_t)n : 1);
    fcopy *k = malloc(sizeof *k);
    if (!g || !k) {
        free(k);
        return g;
    }
    memcpy(g2p(g), p, n);
    k->format = f, k->host = p, k->size = (uint32_t)n, k->g = g;
    pthread_mutex_lock(&fcopy_lock);
    k->next = fcopies;
    fcopies = k;
    pthread_mutex_unlock(&fcopy_lock);
    return g;
}

static void format_forget(AMediaFormat *f)
{
    fcopy *gone = NULL;
    pthread_mutex_lock(&fcopy_lock);
    for (fcopy **pp = &fcopies; *pp;) {
        fcopy *k = *pp;
        if (k->format == f) {
            *pp = k->next;
            k->next = gone;
            gone = k;
        } else {
            pp = &k->next;
        }
    }
    pthread_mutex_unlock(&fcopy_lock);
    while (gone) {
        fcopy *k = gone;
        gone = k->next;
        guest_free(k->g);
        free(k);
    }
}

void imp_AMediaFormat_delete(a32_cpu *c, argreader *R)
{
    AMediaFormat *f = h2ptr(rd32(R));
    if (!f || !AMediaFormat_delete) {
        ret32(c, (uint32_t)AMEDIA_ERROR_INVALID_OBJECT);
        return;
    }
    format_forget(f);
    ret32(c, (uint32_t)AMediaFormat_delete(f));
}

void imp_AMediaFormat_toString(a32_cpu *c, argreader *R)
{
    AMediaFormat *f = h2ptr(rd32(R));
    const char *s = f && AMediaFormat_toString ? AMediaFormat_toString(f) : NULL;
    ret32(c, s ? format_copy(f, s, strlen(s) + 1) : 0);
}

void imp_AMediaFormat_getString(a32_cpu *c, argreader *R)
{
    AMediaFormat *f = h2ptr(rd32(R));
    const char *name = g2p(rd32(R));
    uint32_t out = rd32(R);
    const char *s = NULL;
    bool ok = f && AMediaFormat_getString && AMediaFormat_getString(f, name, &s) && s;
    if (ok && out) st32(out, format_copy(f, s, strlen(s) + 1));
    ret32(c, ok);
}

void imp_AMediaFormat_getBuffer(a32_cpu *c, argreader *R)
{
    AMediaFormat *f = h2ptr(rd32(R));
    const char *name = g2p(rd32(R));
    uint32_t out_data = rd32(R), out_size = rd32(R);
    void *d = NULL;
    size_t n = 0;
    bool ok = f && AMediaFormat_getBuffer && AMediaFormat_getBuffer(f, name, &d, &n);
    if (ok) {
        if (out_data) st32(out_data, d ? format_copy(f, d, n) : 0);
        if (out_size) st32(out_size, (uint32_t)n);
    }
    ret32(c, ok);
}

/* ---------------------------------------------------------------- AMediaCodec buffers */

typedef struct mirror {
    struct mirror *next;
    AMediaCodec *codec;
    int output;
    size_t index;
    uint8_t *host;        /* the codec's buffer as last handed out */
    size_t cap;           /* its size */
    uint32_t g, gcap;     /* the guest's copy and its size (0: none yet) */
    int32_t off, size;    /* output: the range dequeueOutputBuffer reported (size -1: unknown) */
} mirror;

static pthread_mutex_t mirror_lock = PTHREAD_MUTEX_INITIALIZER;
static mirror *mirrors;

/* the entry for (codec, output, index), created if missing; mirror_lock held */
static mirror *mirror_find(AMediaCodec *codec, int output, size_t index, int create)
{
    for (mirror *m = mirrors; m; m = m->next)
        if (m->codec == codec && m->output == output && m->index == index) return m;
    if (!create) return NULL;
    mirror *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->codec = codec, m->output = output, m->index = index, m->size = -1;
    m->next = mirrors;
    mirrors = m;
    return m;
}

/* the guest's view of codec buffer p (cap bytes): p itself when the guest can reach it, else its mirror (made, or
 * remade when the codec's buffer grew); *off, *size: the output range dequeueOutputBuffer reported (size -1: none) */
static uint32_t mirror_of(AMediaCodec *codec, int output, size_t index, uint8_t *p, size_t cap, int32_t *off,
                          int32_t *size)
{
    *off = 0, *size = -1;
    int reach = in_guest_reach(p, cap);
    if (!reach && cap > 0xffffffffu) return 0;
    uint32_t old = 0, g = 0;
    pthread_mutex_lock(&mirror_lock);
    mirror *m = mirror_find(codec, output, index, !reach);
    if (m) {
        m->host = p, m->cap = cap;
        *off = m->off, *size = m->size;
        if (reach) { /* a buffer in reach needs no mirror (queueInputBuffer sees that from host) */
        } else if (m->g && m->gcap >= cap) {
            g = m->g;
        } else {
            old = m->g;
            m->g = 0, m->gcap = 0;
        }
    }
    pthread_mutex_unlock(&mirror_lock);
    if (reach) return p2g(p);
    if (!m) return 0;
    if (old) guest_free(old); /* guest calls: outside the lock */
    if (g) return g;
    g = guest_malloc(cap ? (uint32_t)cap : 1);
    if (!g) return 0;
    pthread_mutex_lock(&mirror_lock);
    m = mirror_find(codec, output, index, 1);
    uint32_t extra = 0;
    if (m && !m->g) { /* (another thread could have made one meanwhile: then that one stays) */
        m->g = g, m->gcap = (uint32_t)(cap ? cap : 1);
    } else if (m) {
        extra = g;
        g = m->g;
    }
    pthread_mutex_unlock(&mirror_lock);
    if (extra) guest_free(extra);
    return m ? g : 0;
}

void imp_AMediaCodec_getInputBuffer(a32_cpu *c, argreader *R)
{
    AMediaCodec *codec = h2ptr(rd32(R));
    size_t index = rd32(R);
    uint32_t out_size = rd32(R);
    size_t cap = 0;
    uint8_t *p = codec && AMediaCodec_getInputBuffer ? AMediaCodec_getInputBuffer(codec, index, &cap) : NULL;
    if (out_size) st32(out_size, p ? (uint32_t)cap : 0);
    int32_t off, size;
    ret32(c, p ? mirror_of(codec, 0, index, p, cap, &off, &size) : 0);
}

void imp_AMediaCodec_queueInputBuffer(a32_cpu *c, argreader *R)
{
    AMediaCodec *codec = h2ptr(rd32(R));
    size_t index = rd32(R);
    uint32_t off = rd32(R), size = rd32(R);
    uint64_t time = rd64(R);
    uint32_t flags = rd32(R);
    if (!codec || !AMediaCodec_queueInputBuffer) {
        ret32(c, (uint32_t)AMEDIA_ERROR_INVALID_OBJECT);
        return;
    }
    pthread_mutex_lock(&mirror_lock);
    mirror *m = mirror_find(codec, 0, index, 0);
    if (m && m->g && m->host && !in_guest_reach(m->host, m->cap) && size && (uint64_t)off + size <= m->cap)
        memcpy(m->host + off, (uint8_t *)g2p(m->g) + off, size);
    else if (m && m->g && size && (uint64_t)off + size > m->cap)
        bridge_log(ANDROID_LOG_WARN, "AMediaCodec_queueInputBuffer: %u bytes at %u past buffer %zu (%zu bytes)", size,
                   off, index, m->cap);
    pthread_mutex_unlock(&mirror_lock);
    ret32(c, (uint32_t)AMediaCodec_queueInputBuffer(codec, index, (off_t)off, size, time, flags));
}

void imp_AMediaCodec_dequeueOutputBuffer(a32_cpu *c, argreader *R)
{
    AMediaCodec *codec = h2ptr(rd32(R));
    AMediaCodecBufferInfo *info = g2p(rd32(R));
    int64_t timeout = (int64_t)rd64(R);
    AMediaCodecBufferInfo tmp;
    if (!codec || !AMediaCodec_dequeueOutputBuffer) {
        ret32(c, (uint32_t)AMEDIACODEC_INFO_TRY_AGAIN_LATER);
        return;
    }
    ssize_t r = AMediaCodec_dequeueOutputBuffer(codec, info ? info : &tmp, timeout);
    if (r >= 0) {
        AMediaCodecBufferInfo *i = info ? info : &tmp;
        pthread_mutex_lock(&mirror_lock);
        mirror *m = mirror_find(codec, 1, (size_t)r, 1);
        if (m) m->off = i->offset, m->size = i->size;
        pthread_mutex_unlock(&mirror_lock);
    }
    ret32(c, (uint32_t)r);
}

void imp_AMediaCodec_getOutputBuffer(a32_cpu *c, argreader *R)
{
    AMediaCodec *codec = h2ptr(rd32(R));
    size_t index = rd32(R);
    uint32_t out_size = rd32(R);
    size_t cap = 0;
    uint8_t *p = codec && AMediaCodec_getOutputBuffer ? AMediaCodec_getOutputBuffer(codec, index, &cap) : NULL;
    if (out_size) st32(out_size, p ? (uint32_t)cap : 0);
    if (!p) {
        ret32(c, 0);
        return;
    }
    int32_t off, size;
    uint32_t g = mirror_of(codec, 1, index, p, cap, &off, &size);
    if (g && g != p2g(p)) {
        if (size < 0 || off < 0 || (uint64_t)off + (uint32_t)size > cap) off = 0, size = (int32_t)cap;
        memcpy((uint8_t *)g2p(g) + off, p + off, (size_t)size);
    }
    ret32(c, g);
}

void imp_AMediaCodec_delete(a32_cpu *c, argreader *R)
{
    AMediaCodec *codec = h2ptr(rd32(R));
    if (!codec || !AMediaCodec_delete) {
        ret32(c, (uint32_t)AMEDIA_ERROR_INVALID_OBJECT);
        return;
    }
    mirror *gone = NULL;
    pthread_mutex_lock(&mirror_lock);
    for (mirror **pp = &mirrors; *pp;) {
        mirror *m = *pp;
        if (m->codec == codec) {
            *pp = m->next;
            m->next = gone;
            gone = m;
        } else {
            pp = &m->next;
        }
    }
    pthread_mutex_unlock(&mirror_lock);
    while (gone) {
        mirror *m = gone;
        gone = m->next;
        guest_free(m->g);
        free(m);
    }
    ret32(c, (uint32_t)AMediaCodec_delete(codec));
}
