#include "timing_stats.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

int main(void) {
    struct tb_timing_stats s;
    struct tb_timing_summary sum;
    memset(&s, 0, sizeof(s));

    /* No samples: nothing to report. */
    CHECK(tb_timing_stats_take_summary(&s, &sum) == 0);

    /* Round trips of 1..100 ms. */
    for (uint64_t ms = 1; ms <= 100; ms++) {
        tb_timing_stats_add_elapsed(&s, 1000000, 1000000 + ms * 1000);
    }
    CHECK(tb_timing_stats_take_summary(&s, &sum) == 1);
    CHECK(sum.count == 100);
    CHECK(sum.avg == 50500);
    CHECK(sum.p50 == 50000);
    CHECK(sum.p95 == 95000);
    CHECK(sum.max == 100000);

    /* Taking a summary resets the window. */
    CHECK(tb_timing_stats_take_summary(&s, &sum) == 0);

    /* A start in the future or a missing start is ignored. */
    tb_timing_stats_add_elapsed(&s, 2000, 1000);
    tb_timing_stats_add_elapsed(&s, 0, 1000);
    CHECK(tb_timing_stats_take_summary(&s, &sum) == 0);

    /* Plain values (e.g. prediction error), including zero. */
    tb_timing_stats_add(&s, 0);
    tb_timing_stats_add(&s, 4);
    CHECK(tb_timing_stats_take_summary(&s, &sum) == 1);
    CHECK(sum.count == 2 && sum.avg == 2 && sum.max == 4 && sum.p50 == 0);

    /* Past the sample cap, count/avg/max stay exact. */
    for (int i = 0; i < TB_TIMING_STATS_MAX_SAMPLES + 10; i++) {
        tb_timing_stats_add(&s, 2000);
    }
    tb_timing_stats_add(&s, 9000);
    CHECK(tb_timing_stats_take_summary(&s, &sum) == 1);
    CHECK(sum.count == TB_TIMING_STATS_MAX_SAMPLES + 11);
    CHECK(sum.max == 9000);
    CHECK(sum.p50 == 2000);

    printf("timing stats: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
