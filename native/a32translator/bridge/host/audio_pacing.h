/* Real-time pacing of the guest's OpenSL ES buffer queue (guest/opensles.c → host/android.c a32_audio_write).
 *
 * The device (AudioFlinger) takes data from the AAudio stream in bursts (~20 ms at 48 kHz). A blocking write alone
 * therefore releases the guest's buffers in bursts too: two 10 ms buffers back to back, then nothing for 20 ms, and the
 * buffer-queue callbacks follow the same rhythm. Audio engines that keep only two buffers and fill them from their own
 * mixer thread (FMOD: Ghost Giant, GitHub #439) are read before they have refilled them: distorted sound. A real
 * OpenSL ES device reads each buffer when it is due, so this paces the reads on the clock instead: buffer k is read at
 * anchor + k × its duration, with a short silence pre-roll in front so the device always has a cushion. The level
 * (frames written to the stream but not yet read by the device) keeps the timeline honest: an underrun or a level
 * below `low` (the guest stalled) starts again with a new pre-roll; a level that grows far past the cushion (the
 * device fell behind) shifts the timeline back; small clock drift either way is nudged out gradually.
 *
 * host/android.c starts pacing only once the device reads: AudioFlinger starts a legacy track when its buffer is full,
 * so the stream keeps a 40 ms buffer until then and the pre-roll is what fills it (pacing a stream that hasn't
 * started would wait for ever). A32_AUDIO_PACING=0 turns pacing off.
 *
 * Pure logic, no AAudio: tests/fixtures/src/a32_audio_pacing_test.c drives it with a simulated device. */
#ifndef A32_AUDIO_PACING_H
#define A32_AUDIO_PACING_H

#include <stdint.h>

#define A32_PACE_NS 1000000000LL

typedef struct {
    int rate;
    int64_t anchor_ns;
    int started;                     /* 0 = start again (stream started or resumed) */
    int checked;                     /* the timeline was corrected for the next write */
    int64_t frames;                  /* guest frames written since the anchor */
    int64_t preroll, low, slack;     /* frames */
    int64_t stall_ns;
    int xruns;
    /* counters for the trace */
    int anchors, shifts;
    int64_t silence_frames;
} a32_pace;

typedef struct {
    int64_t wait_ns; /* > 0: sleep this long, then ask again */
    int64_t silence; /* frames of silence to write before the guest's data */
} a32_pace_step;

static inline void a32_pace_init(a32_pace *p, int rate)
{
    *p = (a32_pace){0};
    p->rate = rate > 0 ? rate : 48000;
    p->preroll = (int64_t)p->rate * 40 / 1000; /* = the old 40 ms stream buffer: same latency as before */
    p->low = (int64_t)p->rate * 10 / 1000;
    p->slack = (int64_t)p->rate * 20 / 1000;
    p->stall_ns = 100000000LL;
}

static inline void a32_pace_restart(a32_pace *p) { p->started = 0, p->checked = 0; }

static inline int64_t a32_pace_due(const a32_pace *p) { return p->anchor_ns + p->frames * A32_PACE_NS / p->rate; }

/* anchor the timeline so the next guest frame is due at `now` */
static inline void a32_pace_anchor(a32_pace *p, int64_t now)
{
    p->anchor_ns = now - p->frames * A32_PACE_NS / p->rate;
    p->anchors++;
}

/* Before a guest write. now: CLOCK_MONOTONIC ns; level: frames buffered in the stream (< 0 = unknown); xruns: the
 * stream's underrun count. Write `silence` frames of silence first; with wait_ns > 0 sleep that long and call again
 * (the timeline is corrected once per write, the calls after a sleep only wait for the due time). */
static inline a32_pace_step a32_pace_before(a32_pace *p, int64_t now, int64_t level, int xruns)
{
    a32_pace_step s = {0, 0};
    if (p->checked) {
        int64_t wait = a32_pace_due(p) - now;
        s.wait_ns = wait > 0 ? wait : 0;
        return s;
    }
    p->checked = 1;
    int underrun = xruns > p->xruns;
    p->xruns = xruns;
    if (!p->started || underrun) { /* (re)start: build the cushion, then play from here */
        int64_t have = level > 0 ? level : 0;
        s.silence = have < p->preroll ? p->preroll - have : 0;
        p->silence_frames += s.silence;
        int64_t due = p->started ? a32_pace_due(p) : now; /* an underrun while the guest is on time: keep its pace */
        p->frames = 0;
        p->started = 1;
        a32_pace_anchor(p, due > now ? due : now);
        s.wait_ns = due > now ? due - now : 0;
        return s;
    }
    int64_t wait = a32_pace_due(p) - now;
    if (level < 0) { /* no level: the clock alone */
        if (-wait > p->stall_ns) a32_pace_anchor(p, now);
        s.wait_ns = wait > 0 ? wait : 0;
        return s;
    }
    /* the level expected when this buffer is due */
    int64_t proj = wait > 0 ? level - wait * p->rate / A32_PACE_NS : level;
    if (proj < p->low) { /* about to run dry (the guest stalled): rebuild the cushion, play from here */
        s.silence = level < p->preroll ? p->preroll - level : 0;
        p->silence_frames += s.silence;
        a32_pace_anchor(p, now);
        return s;
    }
    if (wait <= 0) { /* late: catch up only while the device is short of data, never in a burst otherwise */
        if (level > p->preroll || -wait > p->stall_ns) a32_pace_anchor(p, now);
        return s;
    }
    int64_t err = proj - p->preroll;
    if (err > p->slack) { /* the device fell behind our clock (or hasn't started yet): wait for it */
        p->anchor_ns += err * A32_PACE_NS / p->rate;
        p->shifts++;
    } else { /* clock drift either way: a gentle nudge (the level jumps by the device's bursts; this averages) */
        p->anchor_ns += err * A32_PACE_NS / p->rate / 64;
    }
    wait = a32_pace_due(p) - now;
    s.wait_ns = wait > 0 ? wait : 0;
    return s;
}

static inline void a32_pace_after(a32_pace *p, int frames)
{
    p->frames += frames;
    p->checked = 0;
}

#endif
