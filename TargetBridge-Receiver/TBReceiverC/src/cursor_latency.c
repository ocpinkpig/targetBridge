#include "cursor_latency.h"

#include <stdlib.h>
#include <string.h>

void tb_cursor_latency_record(struct tb_cursor_latency *l,
                              uint64_t echoed_us,
                              uint64_t now_us) {
    if (!l || echoed_us == 0 || echoed_us > now_us) return;
    uint64_t rtt = now_us - echoed_us;
    if (l->stored < TB_CURSOR_LATENCY_MAX_SAMPLES) {
        l->samples_us[l->stored++] = rtt > UINT32_MAX ? UINT32_MAX : (uint32_t)rtt;
    }
    l->count += 1;
    l->sum_us += rtt;
    if (rtt > l->max_us) l->max_us = rtt;
}

static int tb_compare_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile over the sorted samples. */
static uint64_t tb_percentile(const uint32_t *sorted, size_t n, unsigned pct) {
    size_t rank = (n * pct + 99) / 100;
    if (rank == 0) rank = 1;
    return sorted[rank - 1];
}

int tb_cursor_latency_take_summary(struct tb_cursor_latency *l,
                                   struct tb_cursor_latency_summary *out) {
    if (!l || !out || l->count == 0) return 0;

    qsort(l->samples_us, l->stored, sizeof(l->samples_us[0]), tb_compare_u32);
    out->count = l->count;
    out->avg_us = l->sum_us / l->count;
    out->p50_us = tb_percentile(l->samples_us, l->stored, 50);
    out->p95_us = tb_percentile(l->samples_us, l->stored, 95);
    out->max_us = l->max_us;

    memset(l, 0, sizeof(*l));
    return 1;
}
