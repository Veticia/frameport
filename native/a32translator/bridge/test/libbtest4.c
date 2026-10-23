/* Guest test library (armv7): NDK media the way Unity's video player uses it (dlopen libmediandk.so, dlsym every
 * function, extractor -> decoder input buffers, output buffers read back). The host side is test/fakemedia.c. */
#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <string.h>
#include "fakemedia.h"

/* the functions Unity 2017.4's libunity.so looks up: all must resolve, or Unity drops NDK video playback */
static const char *const unity_names[] = {
    "AMediaCodec_configure", "AMediaCodec_createCodecByName", "AMediaCodec_createDecoderByType",
    "AMediaCodec_createEncoderByType", "AMediaCodec_delete", "AMediaCodec_dequeueInputBuffer",
    "AMediaCodec_dequeueOutputBuffer", "AMediaCodec_flush", "AMediaCodec_getInputBuffer",
    "AMediaCodec_getOutputBuffer", "AMediaCodec_getOutputFormat", "AMediaCodec_queueInputBuffer",
    "AMediaCodec_releaseOutputBuffer", "AMediaCodec_releaseOutputBufferAtTime", "AMediaCodec_start",
    "AMediaCodec_stop", "AMediaExtractor_advance", "AMediaExtractor_delete", "AMediaExtractor_getSampleFlags",
    "AMediaExtractor_getSampleTime", "AMediaExtractor_getSampleTrackIndex", "AMediaExtractor_getTrackCount",
    "AMediaExtractor_getTrackFormat", "AMediaExtractor_new", "AMediaExtractor_readSampleData",
    "AMediaExtractor_seekTo", "AMediaExtractor_selectTrack", "AMediaExtractor_setDataSource",
    "AMediaExtractor_setDataSourceFd", "AMediaExtractor_unselectTrack", "AMediaFormat_delete",
    "AMediaFormat_getBuffer", "AMediaFormat_getFloat", "AMediaFormat_getInt32", "AMediaFormat_getInt64",
    "AMediaFormat_getString", "AMediaFormat_new", "AMediaFormat_setBuffer", "AMediaFormat_setFloat",
    "AMediaFormat_setInt32", "AMediaFormat_setInt64", "AMediaFormat_setString", "AMediaFormat_toString",
};

/* ... and the data symbols (const char *AMEDIAFORMAT_KEY_*) it looks up the same way */
static const char *const unity_keys[][2] = {
    {"AMEDIAFORMAT_KEY_AAC_PROFILE", "aac-profile"}, {"AMEDIAFORMAT_KEY_BIT_RATE", "bitrate"},
    {"AMEDIAFORMAT_KEY_CHANNEL_COUNT", "channel-count"}, {"AMEDIAFORMAT_KEY_CHANNEL_MASK", "channel-mask"},
    {"AMEDIAFORMAT_KEY_COLOR_FORMAT", "color-format"}, {"AMEDIAFORMAT_KEY_DURATION", "durationUs"},
    {"AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL", "flac-compression-level"},
    {"AMEDIAFORMAT_KEY_FRAME_RATE", "frame-rate"}, {"AMEDIAFORMAT_KEY_HEIGHT", "height"},
    {"AMEDIAFORMAT_KEY_I_FRAME_INTERVAL", "i-frame-interval"}, {"AMEDIAFORMAT_KEY_IS_ADTS", "is-adts"},
    {"AMEDIAFORMAT_KEY_IS_AUTOSELECT", "is-autoselect"}, {"AMEDIAFORMAT_KEY_IS_DEFAULT", "is-default"},
    {"AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE", "is-forced-subtitle"}, {"AMEDIAFORMAT_KEY_LANGUAGE", "language"},
    {"AMEDIAFORMAT_KEY_MAX_HEIGHT", "max-height"}, {"AMEDIAFORMAT_KEY_MAX_INPUT_SIZE", "max-input-size"},
    {"AMEDIAFORMAT_KEY_MAX_WIDTH", "max-width"}, {"AMEDIAFORMAT_KEY_MIME", "mime"},
    {"AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP", "push-blank-buffers-on-shutdown"},
    {"AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER", "repeat-previous-frame-after"},
    {"AMEDIAFORMAT_KEY_ROTATION", "rotation-degrees"}, {"AMEDIAFORMAT_KEY_SAMPLE_RATE", "sample-rate"},
    {"AMEDIAFORMAT_KEY_STRIDE", "stride"}, {"AMEDIAFORMAT_KEY_WIDTH", "width"},
};

static void *lib;

/* the key variable's string ("" if the symbol is missing) */
static const char *key(const char *sym)
{
    const char *const *p = dlsym(lib, sym);
    return p && *p ? *p : "";
}
#define FN(name) ((__typeof__(&name))dlsym(lib, #name))

int media_test(void)
{
    int bits = 0;
    lib = dlopen("libmediandk.so", RTLD_NOW);
    if (!lib) return -1;
    int all = 1;
    for (unsigned k = 0; k < sizeof unity_names / sizeof *unity_names; k++) all &= dlsym(lib, unity_names[k]) != NULL;
    bits |= all;
    int keys = 1;
    for (unsigned k = 0; k < sizeof unity_keys / sizeof *unity_keys; k++)
        keys &= !strcmp(key(unity_keys[k][0]), unity_keys[k][1]);
    if (keys) bits |= 64;

    /* formats: values both ways, a string and a buffer the host owns */
    AMediaFormat *fmt = FN(AMediaFormat_new)();
    static const uint8_t csd[5] = {0, 0, 0, 1, 0x67};
    FN(AMediaFormat_setString)(fmt, key("AMEDIAFORMAT_KEY_MIME"), "video/avc"); /* names from the key symbols, as Unity does */
    FN(AMediaFormat_setInt32)(fmt, key("AMEDIAFORMAT_KEY_WIDTH"), 640);
    FN(AMediaFormat_setInt64)(fmt, "durationUs", 1ll << 40);
    FN(AMediaFormat_setFloat)(fmt, "frame-rate", 29.5f);
    FN(AMediaFormat_setBuffer)(fmt, "csd-0", csd, sizeof csd);
    const char *mime = NULL;
    void *data = NULL;
    size_t n = 0;
    int32_t w = 0;
    int64_t d = 0;
    float fr = 0;
    const char *text = FN(AMediaFormat_toString)(fmt);
    if (fmt && FN(AMediaFormat_getString)(fmt, "mime", &mime) && mime && !strcmp(mime, "video/avc") &&
        FN(AMediaFormat_getBuffer)(fmt, "csd-0", &data, &n) && n == sizeof csd && !memcmp(data, csd, n) &&
        FN(AMediaFormat_getInt32)(fmt, "width", &w) && w == 640 && FN(AMediaFormat_getInt64)(fmt, "durationUs", &d) &&
        d == 1ll << 40 && FN(AMediaFormat_getFloat)(fmt, "frame-rate", &fr) && fr == 29.5f && text &&
        strstr(text, "video/avc") && !FN(AMediaFormat_getString)(fmt, "missing", &mime))
        bits |= 2;

    /* decoder input: the guest writes its buffer, the codec gets the bytes */
    AMediaCodec *codec = FN(AMediaCodec_createDecoderByType)("video/avc");
    size_t cap = 0;
    if (codec && FN(AMediaCodec_configure)(codec, fmt, NULL, NULL, 0) == AMEDIA_OK &&
        FN(AMediaCodec_start)(codec) == AMEDIA_OK) {
        ssize_t i = FN(AMediaCodec_dequeueInputBuffer)(codec, 10000);
        uint8_t *buf = i >= 0 ? FN(AMediaCodec_getInputBuffer)(codec, (size_t)i, &cap) : NULL;
        if (buf && cap == FAKE_BUFSZ) {
            for (int k = 0; k < 1000; k++) buf[16 + k] = FAKE_PATTERN1(k);
            if (FN(AMediaCodec_queueInputBuffer)(codec, (size_t)i, 16, 1000, 0x100000001ull, 4) == AMEDIA_OK)
                bits |= 4;
        }
    }

    /* decoder output: the buffer info and the bytes the codec wrote */
    AMediaCodecBufferInfo info;
    memset(&info, 0, sizeof info);
    ssize_t o = codec ? FN(AMediaCodec_dequeueOutputBuffer)(codec, &info, 10000) : -1;
    uint8_t *out = o >= 0 ? FN(AMediaCodec_getOutputBuffer)(codec, (size_t)o, &cap) : NULL;
    if (out && cap == FAKE_BUFSZ && info.offset == 8 && info.size == 1000 && info.presentationTimeUs == 0x100000001ll &&
        info.flags == 4) {
        int same = 1;
        for (int k = 0; k < 1000; k++) same &= out[8 + k] == (FAKE_PATTERN1(k) ^ 0x5a);
        if (same && FN(AMediaCodec_releaseOutputBuffer)(codec, (size_t)o, 0) == AMEDIA_OK) bits |= 8;
    }

    /* extractor: 64-bit arguments, readSampleData straight into an input buffer, queued again */
    AMediaExtractor *ex = FN(AMediaExtractor_new)();
    if (ex && FN(AMediaExtractor_setDataSourceFd)(ex, 5, 1ll << 33, 77) == AMEDIA_OK &&
        FN(AMediaExtractor_seekTo)(ex, 5000000000ll, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC) == AMEDIA_OK &&
        FN(AMediaExtractor_getSampleTime)(ex) == 0x123456789ll && codec) {
        ssize_t i = FN(AMediaCodec_dequeueInputBuffer)(codec, -1);
        uint8_t *buf = i >= 0 ? FN(AMediaCodec_getInputBuffer)(codec, (size_t)i, &cap) : NULL;
        ssize_t got = buf ? FN(AMediaExtractor_readSampleData)(ex, buf, cap) : -1;
        if (got == 100 && FN(AMediaCodec_queueInputBuffer)(codec, (size_t)i, 0, (size_t)got, 0, 0) == AMEDIA_OK)
            bits |= 16;
        FN(AMediaExtractor_delete)(ex);
    }

    /* formats the codec makes, deletes */
    AMediaFormat *of = codec ? FN(AMediaCodec_getOutputFormat)(codec) : NULL;
    if (of && FN(AMediaFormat_getInt32)(of, "width", &w) && w == 1920 && FN(AMediaFormat_delete)(of) == AMEDIA_OK &&
        FN(AMediaFormat_delete)(fmt) == AMEDIA_OK && FN(AMediaCodec_delete)(codec) == AMEDIA_OK)
        bits |= 32;
    return bits;
}
