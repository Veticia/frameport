/* libmediandk's data symbols: the AMEDIAFORMAT_KEY_* string pointers. Unity's video player looks them up with dlsym
 * like its functions and drops NDK playback if one is missing. The host's libmediandk only compares key names by
 * content, so guest strings with AOSP's values (frameworks/av media/ndk/NdkMediaFormat.cpp) work with its formats.
 * gen_stubs.py exports every AMEDIAFORMAT_KEY_ defined here. The set: what Unity 2017.4's libunity.so references. */
#define KEY(name, value) const char *AMEDIAFORMAT_KEY_##name = value

KEY(AAC_PROFILE, "aac-profile");
KEY(BIT_RATE, "bitrate");
KEY(CHANNEL_COUNT, "channel-count");
KEY(CHANNEL_MASK, "channel-mask");
KEY(COLOR_FORMAT, "color-format");
KEY(DURATION, "durationUs");
KEY(FLAC_COMPRESSION_LEVEL, "flac-compression-level");
KEY(FRAME_RATE, "frame-rate");
KEY(HEIGHT, "height");
KEY(I_FRAME_INTERVAL, "i-frame-interval");
KEY(IS_ADTS, "is-adts");
KEY(IS_AUTOSELECT, "is-autoselect");
KEY(IS_DEFAULT, "is-default");
KEY(IS_FORCED_SUBTITLE, "is-forced-subtitle");
KEY(LANGUAGE, "language");
KEY(MAX_HEIGHT, "max-height");
KEY(MAX_INPUT_SIZE, "max-input-size");
KEY(MAX_WIDTH, "max-width");
KEY(MIME, "mime");
KEY(PUSH_BLANK_BUFFERS_ON_STOP, "push-blank-buffers-on-shutdown");
KEY(REPEAT_PREVIOUS_FRAME_AFTER, "repeat-previous-frame-after");
KEY(ROTATION, "rotation-degrees");
KEY(SAMPLE_RATE, "sample-rate");
KEY(STRIDE, "stride");
KEY(WIDTH, "width");
