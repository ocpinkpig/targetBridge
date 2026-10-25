/* cursor_latency.h — round-trip timing for the receiver-control cursor.
 *
 * The Receiver stamps each move it sends with its own monotonic clock; the
 * Sender echoes that stamp in the cursor packet it sends back. The difference
 * on arrival is the full mouse → Sender → overlay-position round trip, with
 * no clock sync between the two Macs. */

#ifndef TB_CURSOR_LATENCY_H
#define TB_CURSOR_LATENCY_H

#include <stddef.h>
#include <stdint.h>

#define TB_CURSOR_LATENCY_MAX_SAMPLES 1024

struct tb_cursor_latency {
    uint32_t samples_us[TB_CURSOR_LATENCY_MAX_SAMPLES];
    size_t   stored;   /* samples kept for percentiles (capped) */
    uint64_t count;    /* all samples, including ones past the cap */
    uint64_t sum_us;
    uint64_t max_us;
};

struct tb_cursor_latency_summary {
    uint64_t count;
    uint64_t avg_us;
    uint64_t p50_us;
    uint64_t p95_us;
    uint64_t max_us;
};

/* Records now - echoed stamp. Ignores a stamp from the future (e.g. one sent
 * before a receiver restart). */
void tb_cursor_latency_record(struct tb_cursor_latency *l,
                              uint64_t echoed_us,
                              uint64_t now_us);

/* Summarizes and resets the window. Returns 0 when there were no samples. */
int tb_cursor_latency_take_summary(struct tb_cursor_latency *l,
                                   struct tb_cursor_latency_summary *out);

#endif
