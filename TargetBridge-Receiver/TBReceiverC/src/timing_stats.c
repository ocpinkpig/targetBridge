#include "timing_stats.h"

#include <stdlib.h>
#include <string.h>

void tb_timing_stats_add(struct tb_timing_stats *s, uint64_t value) {
    if (!s) return;
    if (s->stored < TB_TIMING_STATS_MAX_SAMPLES) {
        s->samples[s->stored++] = value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
    }
    s->count += 1;
    s->sum += value;
    if (value > s->max) s->max = value;
}

void tb_timing_stats_add_elapsed(struct tb_timing_stats *s,
                                 uint64_t start,
                                 uint64_t now) {
    if (start == 0 || start > now) return;
    tb_timing_stats_add(s, now - start);
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

int tb_timing_stats_take_summary(struct tb_timing_stats *s,
                                 struct tb_timing_summary *out) {
    if (!s || !out || s->count == 0) return 0;

    qsort(s->samples, s->stored, sizeof(s->samples[0]), tb_compare_u32);
    out->count = s->count;
    out->avg = s->sum / s->count;
    out->p50 = tb_percentile(s->samples, s->stored, 50);
    out->p95 = tb_percentile(s->samples, s->stored, 95);
    out->max = s->max;

    memset(s, 0, sizeof(*s));
    return 1;
}
