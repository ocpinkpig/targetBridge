#include "cursor_latency.h"

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
    struct tb_cursor_latency l;
    struct tb_cursor_latency_summary s;
    memset(&l, 0, sizeof(l));

    /* No samples: nothing to report. */
    CHECK(tb_cursor_latency_take_summary(&l, &s) == 0);

    /* Round trips of 1..100 ms. */
    for (uint64_t ms = 1; ms <= 100; ms++) {
        tb_cursor_latency_record(&l, 1000000, 1000000 + ms * 1000);
    }
    CHECK(tb_cursor_latency_take_summary(&l, &s) == 1);
    CHECK(s.count == 100);
    CHECK(s.avg_us == 50500);
    CHECK(s.p50_us == 50000);
    CHECK(s.p95_us == 95000);
    CHECK(s.max_us == 100000);

    /* Taking a summary resets the window. */
    CHECK(tb_cursor_latency_take_summary(&l, &s) == 0);

    /* A stamp from the future or a missing stamp is ignored. */
    tb_cursor_latency_record(&l, 2000, 1000);
    tb_cursor_latency_record(&l, 0, 1000);
    CHECK(tb_cursor_latency_take_summary(&l, &s) == 0);

    /* Past the sample cap, count/avg/max stay exact. */
    for (int i = 0; i < TB_CURSOR_LATENCY_MAX_SAMPLES + 10; i++) {
        tb_cursor_latency_record(&l, 1, 1 + 2000);
    }
    tb_cursor_latency_record(&l, 1, 1 + 9000);
    CHECK(tb_cursor_latency_take_summary(&l, &s) == 1);
    CHECK(s.count == TB_CURSOR_LATENCY_MAX_SAMPLES + 11);
    CHECK(s.max_us == 9000);
    CHECK(s.p50_us == 2000);

    printf("cursor_latency: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
