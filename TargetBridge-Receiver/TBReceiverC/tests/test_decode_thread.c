/* decode_thread.c against stand-in decoder functions: no FFmpeg needed. */
#include "decode_thread.h"
#include "decoder.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

/* ---- stand-in decoder ------------------------------------------------ */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log[4096];        /* e.g. "P1 F1 F2 R " */
static int g_decode_ms;         /* simulated decode time */
static pthread_t g_main;
static int g_ran_off_main = 1;  /* every job ran off the caller's thread */
static int g_drop_latest_calls;

static void record(const char *what, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&g_lock);
    if (pthread_equal(pthread_self(), g_main)) g_ran_off_main = 0;
    char entry[32];
    snprintf(entry, sizeof(entry), "%s%.*s ", what, (int)len, data ? (const char *)data : "");
    strncat(g_log, entry, sizeof(g_log) - strlen(g_log) - 1);
    pthread_mutex_unlock(&g_lock);
}

int tb_dec_set_param_sets(struct tb_decoder *d, const uint8_t *payload, size_t len) {
    (void)d;
    record("P", payload, len);
    return 0;
}

int tb_dec_feed_frame(struct tb_decoder *d, const uint8_t *avcc, size_t len) {
    (void)d;
    if (g_decode_ms) usleep((useconds_t)g_decode_ms * 1000);
    record("F", avcc, len);
    return 0;
}

void tb_dec_reset(struct tb_decoder *d) {
    (void)d;
    record("R", NULL, 0);
}

void tb_dec_drop_latest(struct tb_decoder *d) {
    (void)d;
    pthread_mutex_lock(&g_lock);
    g_drop_latest_calls++;
    pthread_mutex_unlock(&g_lock);
}

/* ---- helpers --------------------------------------------------------- */

static void reset_log(void) {
    pthread_mutex_lock(&g_lock);
    g_log[0] = '\0';
    pthread_mutex_unlock(&g_lock);
}

static int log_equals(const char *expected) {
    pthread_mutex_lock(&g_lock);
    const int same = strcmp(g_log, expected) == 0;
    if (!same) fprintf(stderr, "  log was \"%s\", expected \"%s\"\n", g_log, expected);
    pthread_mutex_unlock(&g_lock);
    return same;
}

/* Waits up to 2 s for the worker to go idle with wants_frames true. */
static void wait_idle(struct tb_decode_thread *t, const char *expected_log) {
    for (int i = 0; i < 400; i++) {
        pthread_mutex_lock(&g_lock);
        const int done = strcmp(g_log, expected_log) == 0;
        pthread_mutex_unlock(&g_lock);
        if (done && tb_decode_thread_wants_frames(t)) return;
        usleep(5000);
    }
}

int main(void) {
    g_main = pthread_self();
    struct tb_decode_thread *t = tb_decode_thread_create((struct tb_decoder *)0x1);
    CHECK(t != NULL);

    /* Jobs run in order, off the caller's thread, on copies of the bytes. */
    uint8_t buf[2] = { 'a', 0 };
    tb_decode_thread_queue_params(t, buf, 1);
    buf[0] = '1';
    tb_decode_thread_queue_frame(t, buf, 1);
    buf[0] = '2';
    tb_decode_thread_queue_frame(t, buf, 1);
    buf[0] = 'X';  /* caller reuses its buffer straight away */
    wait_idle(t, "Pa F1 F2 ");
    CHECK(log_equals("Pa F1 F2 "));
    CHECK(g_ran_off_main);

    struct tb_timing_summary s;
    CHECK(tb_decode_thread_take_decode_stats(t, &s) == 1);
    CHECK(s.count == 2);
    CHECK(tb_decode_thread_take_decode_stats(t, &s) == 0);

    /* Back-pressure: with slow decodes, two waiting frames stop reads. */
    reset_log();
    g_decode_ms = 60;
    CHECK(tb_decode_thread_wants_frames(t));
    tb_decode_thread_queue_frame(t, (const uint8_t *)"3", 1);
    tb_decode_thread_queue_frame(t, (const uint8_t *)"4", 1);
    CHECK(!tb_decode_thread_wants_frames(t));
    wait_idle(t, "F3 F4 ");
    CHECK(tb_decode_thread_wants_frames(t));
    CHECK(log_equals("F3 F4 "));

    /* Reset drops frames still queued (not the one being decoded), drops
     * the undisplayed frame at once, then resets the decoder in order. */
    reset_log();
    g_decode_ms = 80;
    tb_decode_thread_queue_frame(t, (const uint8_t *)"5", 1);
    usleep(20000);  /* the worker is now decoding 5 */
    tb_decode_thread_queue_frame(t, (const uint8_t *)"6", 1);
    tb_decode_thread_queue_frame(t, (const uint8_t *)"7", 1);
    const int drops_before = g_drop_latest_calls;
    tb_decode_thread_reset(t);
    CHECK(g_drop_latest_calls == drops_before + 1);
    wait_idle(t, "F5 R ");
    CHECK(log_equals("F5 R "));
    CHECK(tb_decode_thread_wants_frames(t));  /* back-pressure count recovered */

    /* Destroy with work pending returns promptly and frees it. */
    g_decode_ms = 0;
    for (int i = 0; i < 10; i++) tb_decode_thread_queue_frame(t, (const uint8_t *)"z", 1);
    tb_decode_thread_destroy(t);

    printf("decode thread: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
