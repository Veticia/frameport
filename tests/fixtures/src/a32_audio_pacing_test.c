/* a32 bridge audio pacing (native/a32translator/bridge/host/audio_pacing.h, GitHub #439): a simulated device that
 * takes 960 frames every 20 ms (AudioFlinger's bursts at 48 kHz) from a 150 ms stream buffer, fed by a guest that
 * queues 10 ms buffers. Paced, the reads come one per 10 ms (never two back to back) and the device never runs dry;
 * stalls, underruns and clock drift both ways re-anchor without bursts or underruns. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "audio_pacing.h"

#define RATE 48000
#define MS 1000000LL

typedef struct {
    int64_t written, read, cap;
    int xruns;
    int64_t next_burst, burst_ns; /* device clock: burst period in our ns */
    int started;
} device;

/* advance the device to time t */
static void device_run(device *d, int64_t t)
{
    while (d->next_burst <= t) {
        if (!d->started) { /* AudioFlinger starts a track ~90 ms late with one big read (seen on the Frame) */
            d->started = 1;
            d->read += d->written - d->read < 2880 ? d->written - d->read : 2880;
            d->next_burst += d->burst_ns;
            continue;
        }
        if (d->written - d->read < 960) {
            d->xruns++;
            d->read = d->written; /* plays what there is, then silence */
        } else {
            d->read += 960;
        }
        d->next_burst += d->burst_ns;
    }
}

typedef struct {
    int reads, xruns, min_gap_ms_x10, max_gap_ms_x10;
    int64_t last_read;
} result;

/* the guest writes `n` 480-frame buffers; stall_at: buffer index after which the guest pauses for stall_ms */
static result simulate_buf(int buf, int64_t burst_ns, int n, int stall_at, int stall_ms, int underrun_once)
{
    device d = {0, 0, 3848, 0, 90 * MS, burst_ns, 0}; /* capacity 3848: the Frame's legacy AAudio stream */
    a32_pace p;
    a32_pace_init(&p, RATE);
    result r = {0, 0, 1000000, 0, -1};
    int64_t t = 0;
    for (int k = 0; k < n; k++) {
        if (k == stall_at) t += stall_ms * MS;
        if (underrun_once && k == n / 2) d.xruns++; /* a reported underrun: the pacer starts over with a pre-roll */
        for (;;) {
            device_run(&d, t);
            a32_pace_step s = a32_pace_before(&p, t, d.written - d.read, d.xruns);
            d.written += s.silence;
            if (s.wait_ns <= 0) break;
            t += s.wait_ns;
        }
        /* blocking write: what fits, then wait for room */
        for (int64_t left = buf; left > 0;) {
            int64_t room = d.cap - (d.written - d.read);
            if (room <= 0) {
                t = d.next_burst;
                device_run(&d, t);
                continue;
            }
            int64_t n = left < room ? left : room;
            d.written += n;
            left -= n;
        }
        a32_pace_after(&p, buf);
        if (k > 0 && r.last_read >= 0 && k != stall_at && !(underrun_once && k == n / 2)) {
            int g = (int)((t - r.last_read) * 10 / MS);
            if (g < r.min_gap_ms_x10) r.min_gap_ms_x10 = g;
            if (g > r.max_gap_ms_x10 && k > 20) r.max_gap_ms_x10 = g; /* the device starts ~90 ms late: a wait */
        }
        r.last_read = t;
        r.reads++;
        t += 50000; /* the guest's callback refills and re-enqueues: 50 us */
    }
    r.xruns = d.xruns - (underrun_once ? 1 : 0);
    return r;
}

static result simulate(int64_t burst_ns, int n, int stall_at, int stall_ms, int underrun_once)
{
    return simulate_buf(480, burst_ns, n, stall_at, stall_ms, underrun_once);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* steady state: one read every 10 ms, no underruns */
    result r = simulate(20 * MS, 3000, -1, 0, 0);
    printf("steady: gaps %d..%d (x0.1 ms) xruns %d\n", r.min_gap_ms_x10, r.max_gap_ms_x10, r.xruns);
    assert(r.xruns == 0);
    assert(r.min_gap_ms_x10 >= 95 && r.max_gap_ms_x10 <= 105);

    /* the device's clock 0.5 % slow / fast: corrections, never an underrun, never a burst of reads */
    r = simulate(20 * MS * 1005 / 1000, 6000, -1, 0, 0);
    printf("slow device: gaps %d..%d xruns %d\n", r.min_gap_ms_x10, r.max_gap_ms_x10, r.xruns);
    assert(r.xruns == 0 && r.min_gap_ms_x10 >= 80);
    r = simulate(20 * MS * 995 / 1000, 6000, -1, 0, 0);
    printf("fast device: gaps %d..%d xruns %d\n", r.min_gap_ms_x10, r.max_gap_ms_x10, r.xruns);
    assert(r.xruns == 0 && r.min_gap_ms_x10 >= 80);

    /* the guest stalls for 300 ms: one underrun at most while it stalls, then no burst to catch up */
    r = simulate(20 * MS, 1000, 500, 300, 0);
    printf("stall: gaps %d..%d xruns %d\n", r.min_gap_ms_x10, r.max_gap_ms_x10, r.xruns);
    assert(r.xruns <= 15 && r.min_gap_ms_x10 >= 50);

    /* a reported underrun: a new pre-roll, then paced again */
    r = simulate(20 * MS, 1000, -1, 0, 1);
    printf("underrun: gaps %d..%d xruns %d\n", r.min_gap_ms_x10, r.max_gap_ms_x10, r.xruns);
    assert(r.xruns == 0 && r.min_gap_ms_x10 >= 95 && r.max_gap_ms_x10 <= 105);

    /* large buffers (85 ms; engines other than FMOD): one read per buffer duration, no underruns */
    r = simulate_buf(4096, 20 * MS, 500, -1, 0, 0);
    printf("4096-frame buffers: gaps %d..%d xruns %d\n", r.min_gap_ms_x10, r.max_gap_ms_x10, r.xruns);
    assert(r.xruns == 0 && r.min_gap_ms_x10 >= 750 && r.max_gap_ms_x10 <= 1050);

    /* restart (stream started again): pre-roll again */
    a32_pace p;
    a32_pace_init(&p, RATE);
    a32_pace_step s = a32_pace_before(&p, 1 * MS, 0, 0);
    assert(s.silence == RATE * 40 / 1000 && s.wait_ns == 0);
    a32_pace_after(&p, 480);
    s = a32_pace_before(&p, 1 * MS + 100000, 1920 + 480, 0);
    assert(llabs(s.wait_ns - (10 * MS - 100000)) < 100000 && s.silence == 0);
    a32_pace_restart(&p);
    s = a32_pace_before(&p, 50 * MS, 600, 0);
    assert(s.silence == 1920 - 600 && s.wait_ns == 0);
    puts("ok");
    return 0;
}
