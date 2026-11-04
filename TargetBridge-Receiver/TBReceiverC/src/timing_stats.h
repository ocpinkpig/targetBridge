/* timing_stats.h — a one-second window of timings for the latency log.
 *
 * Used for the receiver-control cursor round trip (the Receiver stamps each
 * move with its own monotonic clock and the Sender echoes the stamp back, so
 * no clock sync between the two Macs is needed), for how long input waits
 * before it is sent, for prediction error, and for per-frame decode/upload/
 * present cost. Values are plain integers; the callers use microseconds
 * (or points for prediction error). */

#ifndef TB_TIMING_STATS_H
#define TB_TIMING_STATS_H

#include <stddef.h>
#include <stdint.h>

#define TB_TIMING_STATS_MAX_SAMPLES 1024

struct tb_timing_stats {
    uint32_t samples[TB_TIMING_STATS_MAX_SAMPLES];
    size_t   stored;   /* samples kept for percentiles (capped) */
    uint64_t count;    /* all samples, including ones past the cap */
    uint64_t sum;
    uint64_t max;
};

struct tb_timing_summary {
    uint64_t count;
    uint64_t avg;
    uint64_t p50;
    uint64_t p95;
    uint64_t max;
};

void tb_timing_stats_add(struct tb_timing_stats *s, uint64_t value);

/* Records now - start. Ignores a missing start (0) and a start in the future
 * (e.g. a stamp sent before a receiver restart). */
void tb_timing_stats_add_elapsed(struct tb_timing_stats *s,
                                 uint64_t start,
                                 uint64_t now);

/* Summarizes and resets the window. Returns 0 when there were no samples. */
int tb_timing_stats_take_summary(struct tb_timing_stats *s,
                                 struct tb_timing_summary *out);

#endif
