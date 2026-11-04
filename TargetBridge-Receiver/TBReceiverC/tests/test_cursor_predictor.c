#include "cursor_predictor.h"

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

/* A 2560x1440-point Sender display streamed at 5120x2880 (2 px per point). */
#define SW 5120
#define SH 2880
#define PW 2560.0
#define PH 1440.0

static struct tb_cursor_predictor p;

static int cursor(int x, int y, int visible, uint64_t echo, uint64_t *err) {
    return tb_cursor_predictor_on_cursor(&p, x, y, SW, SH, visible, PW, PH, echo, err);
}

static void test_older_sender_disables_prediction(void) {
    tb_cursor_predictor_reset(&p);
    /* No point size in the packet. */
    tb_cursor_predictor_on_cursor(&p, 100, 100, SW, SH, 1, 0, 0, 0, NULL);
    tb_cursor_predictor_on_move(&p, 1000, 5, 5);
    int x, y, v;
    CHECK(tb_cursor_predictor_position(&p, 1000, &x, &y, &v) == 0);
}

static void test_moves_apply_on_top_of_confirmed_position(void) {
    tb_cursor_predictor_reset(&p);
    CHECK(cursor(2000, 1000, 1, 0, NULL) == 0);  /* base: (1000, 500) pt */

    int x, y, v;
    /* Nothing pending: show the packet as is. */
    CHECK(tb_cursor_predictor_position(&p, 1000, &x, &y, &v) == 0);

    tb_cursor_predictor_on_move(&p, 1000, 10, -5);
    tb_cursor_predictor_on_move(&p, 2000, 3, 2);
    CHECK(tb_cursor_predictor_position(&p, 2000, &x, &y, &v) == 1);
    CHECK(x == 2026 && y == 994 && v == 1);       /* (1013, 497) pt */

    /* The Sender confirms the first move: prediction stays put. */
    uint64_t err = 99;
    CHECK(cursor(2020, 990, 1, 1000, &err) == 1);
    CHECK(err == 0);
    CHECK(p.count == 1);
    CHECK(tb_cursor_predictor_position(&p, 2000, &x, &y, &v) == 1);
    CHECK(x == 2026 && y == 994);

    /* Confirms the second: nothing left to predict. */
    CHECK(cursor(2026, 994, 1, 2000, &err) == 1);
    CHECK(err == 0);
    CHECK(tb_cursor_predictor_position(&p, 2000, &x, &y, &v) == 0);
}

static void test_edge_clamp_matches_sender(void) {
    tb_cursor_predictor_reset(&p);
    cursor(5100, 10, 1, 0, NULL);                 /* (2550, 5) pt */
    tb_cursor_predictor_on_move(&p, 1000, 30, 0); /* pushes past the right edge */
    tb_cursor_predictor_on_move(&p, 2000, -4, -20);
    int x, y, v;
    CHECK(tb_cursor_predictor_position(&p, 2000, &x, &y, &v) == 1);
    /* Held at 2559 pt, then 4 back; y held at 0. */
    CHECK(x == 2 * 2555 && y == 0 && v == 1);
}

static void test_timer_packet_keeps_base_while_moves_pending(void) {
    tb_cursor_predictor_reset(&p);
    cursor(1000, 1000, 1, 0, NULL);               /* (500, 500) pt */
    tb_cursor_predictor_on_move(&p, 1000, 10, 0);
    /* A timer packet with some unknown subset of moves applied. */
    cursor(1010, 1000, 1, 0, NULL);
    int x, y, v;
    CHECK(tb_cursor_predictor_position(&p, 1000, &x, &y, &v) == 1);
    CHECK(x == 1020);                             /* not 1010 + 20 */

    /* It still carries visibility: the cursor left onto another display. */
    cursor(0, 0, 0, 0, NULL);
    CHECK(tb_cursor_predictor_position(&p, 1000, &x, &y, &v) == 1);
    CHECK(v == 0);
}

static void test_mismatch_is_measured_and_corrected(void) {
    tb_cursor_predictor_reset(&p);
    cursor(1000, 1000, 1, 0, NULL);               /* (500, 500) pt */
    tb_cursor_predictor_on_move(&p, 1000, 10, 0);
    tb_cursor_predictor_on_move(&p, 2000, 10, 0);
    /* The Sender ended the first move at 506 pt, not 510. */
    uint64_t err = 0;
    CHECK(cursor(1012, 1000, 1, 1000, &err) == 1);
    CHECK(err == 35);                             /* 4 pt off, minus the 0.5 pt grid */
    int x, y, v;
    CHECK(tb_cursor_predictor_position(&p, 2000, &x, &y, &v) == 1);
    CHECK(x == 1032);                             /* rebased: 506 + 10 pt */
}

static void test_unconfirmed_moves_expire(void) {
    tb_cursor_predictor_reset(&p);
    cursor(1000, 1000, 1, 0, NULL);
    tb_cursor_predictor_on_move(&p, 1000, 10, 0);
    int x, y, v;
    CHECK(tb_cursor_predictor_position(&p, 1000 + TB_CURSOR_PREDICTOR_MAX_AGE_US, &x, &y, &v) == 1);
    CHECK(tb_cursor_predictor_position(&p, 1001 + TB_CURSOR_PREDICTOR_MAX_AGE_US, &x, &y, &v) == 0);
    CHECK(p.count == 0);
}

static void test_echo_drops_moves_the_sender_skipped(void) {
    tb_cursor_predictor_reset(&p);
    cursor(1000, 1000, 1, 0, NULL);
    tb_cursor_predictor_on_move(&p, 1000, 10, 0);
    tb_cursor_predictor_on_move(&p, 2000, 10, 0);
    tb_cursor_predictor_on_move(&p, 3000, 10, 0);
    /* Echo of the last move confirms (and drops) everything before it. */
    cursor(1060, 1000, 1, 3000, NULL);
    CHECK(p.count == 0);
}

static void test_pending_overflow_keeps_newest(void) {
    tb_cursor_predictor_reset(&p);
    cursor(0, 0, 1, 0, NULL);
    for (uint64_t i = 1; i <= TB_CURSOR_PREDICTOR_MAX_PENDING + 5; i++) {
        tb_cursor_predictor_on_move(&p, i, 1, 0);
    }
    CHECK(p.count == TB_CURSOR_PREDICTOR_MAX_PENDING);
    CHECK(p.pending[p.head].t == 6);
}

int main(void) {
    test_older_sender_disables_prediction();
    test_moves_apply_on_top_of_confirmed_position();
    test_edge_clamp_matches_sender();
    test_timer_packet_keeps_base_while_moves_pending();
    test_mismatch_is_measured_and_corrected();
    test_unconfirmed_moves_expire();
    test_echo_drops_moves_the_sender_skipped();
    test_pending_overflow_keeps_newest();

    printf("cursor predictor: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
