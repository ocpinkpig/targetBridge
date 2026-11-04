#include "decode_thread.h"
#include "decoder.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum tb_decode_job_kind {
    TB_DECODE_JOB_PARAMS,
    TB_DECODE_JOB_FRAME,
    TB_DECODE_JOB_RESET
};

struct tb_decode_job {
    enum tb_decode_job_kind kind;
    uint8_t *data;
    size_t   len;
};

struct tb_decode_thread {
    struct tb_decoder *dec;
    pthread_t          thread;
    int                started;

    pthread_mutex_t    lock;
    pthread_cond_t     has_work;
    pthread_cond_t     has_space;
    struct tb_decode_job jobs[TB_DECODE_QUEUE_CAPACITY];
    size_t             head;
    size_t             count;
    size_t             frames_waiting;
    int                stopping;
    struct tb_timing_stats decode_us;
    /* TB_RECEIVER_DECODE_DELAY_MS: extra time per decode, to reproduce a
     * slow receiver (e.g. 5K on an Intel Mac) on a fast one. Off by default. */
    unsigned           simulated_delay_ms;
};

static uint64_t tb_decode_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void *tb_decode_thread_main(void *arg) {
    struct tb_decode_thread *t = (struct tb_decode_thread *)arg;
    for (;;) {
        pthread_mutex_lock(&t->lock);
        while (t->count == 0 && !t->stopping) pthread_cond_wait(&t->has_work, &t->lock);
        if (t->stopping) {
            pthread_mutex_unlock(&t->lock);
            return NULL;
        }
        struct tb_decode_job job = t->jobs[t->head];
        t->head = (t->head + 1) % TB_DECODE_QUEUE_CAPACITY;
        t->count--;
        pthread_mutex_unlock(&t->lock);

        uint64_t started = 0;
        switch (job.kind) {
        case TB_DECODE_JOB_PARAMS:
            tb_dec_set_param_sets(t->dec, job.data, job.len);
            break;
        case TB_DECODE_JOB_FRAME:
            started = tb_decode_now_us();
            tb_dec_feed_frame(t->dec, job.data, job.len);
            if (t->simulated_delay_ms) usleep((useconds_t)t->simulated_delay_ms * 1000);
            break;
        case TB_DECODE_JOB_RESET:
            tb_dec_reset(t->dec);
            break;
        }
        free(job.data);

        pthread_mutex_lock(&t->lock);
        if (job.kind == TB_DECODE_JOB_FRAME) {
            /* Counted until decoded, not just until dequeued: a frame being
             * decoded still holds the loop's back-pressure. */
            t->frames_waiting--;
            tb_timing_stats_add_elapsed(&t->decode_us, started, tb_decode_now_us());
        }
        pthread_cond_broadcast(&t->has_space);
        pthread_mutex_unlock(&t->lock);
    }
}

struct tb_decode_thread *tb_decode_thread_create(struct tb_decoder *dec) {
    struct tb_decode_thread *t = (struct tb_decode_thread *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->dec = dec;
    const char *delay = getenv("TB_RECEIVER_DECODE_DELAY_MS");
    if (delay && atoi(delay) > 0) {
        t->simulated_delay_ms = (unsigned)atoi(delay);
        fprintf(stderr, "[dec] simulating %u ms extra per decode\n", t->simulated_delay_ms);
    }
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->has_work, NULL);
    pthread_cond_init(&t->has_space, NULL);
    if (pthread_create(&t->thread, NULL, tb_decode_thread_main, t) != 0) {
        fprintf(stderr, "[dec] could not start decode thread\n");
        tb_decode_thread_destroy(t);
        return NULL;
    }
    t->started = 1;
    return t;
}

static void tb_decode_thread_clear_locked(struct tb_decode_thread *t) {
    while (t->count > 0) {
        free(t->jobs[t->head].data);
        t->head = (t->head + 1) % TB_DECODE_QUEUE_CAPACITY;
        t->count--;
    }
}

void tb_decode_thread_destroy(struct tb_decode_thread *t) {
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    t->stopping = 1;
    pthread_cond_broadcast(&t->has_work);
    pthread_mutex_unlock(&t->lock);
    if (t->started) pthread_join(t->thread, NULL);
    tb_decode_thread_clear_locked(t);
    pthread_cond_destroy(&t->has_work);
    pthread_cond_destroy(&t->has_space);
    pthread_mutex_destroy(&t->lock);
    free(t);
}

static void tb_decode_thread_push(struct tb_decode_thread *t,
                                  enum tb_decode_job_kind kind,
                                  const uint8_t *payload, size_t len) {
    uint8_t *copy = NULL;
    if (payload && len > 0) {
        copy = (uint8_t *)malloc(len);
        if (!copy) {
            fprintf(stderr, "[dec] out of memory queueing %zu bytes\n", len);
            return;
        }
        memcpy(copy, payload, len);
    }

    pthread_mutex_lock(&t->lock);
    /* Encoded frames are never dropped (later ones reference them), so a
     * full queue waits. The loop's back-pressure keeps this rare. */
    while (t->count == TB_DECODE_QUEUE_CAPACITY && !t->stopping) {
        pthread_cond_wait(&t->has_space, &t->lock);
    }
    if (t->stopping) {
        pthread_mutex_unlock(&t->lock);
        free(copy);
        return;
    }
    struct tb_decode_job *job = &t->jobs[(t->head + t->count) % TB_DECODE_QUEUE_CAPACITY];
    job->kind = kind;
    job->data = copy;
    job->len = len;
    t->count++;
    if (kind == TB_DECODE_JOB_FRAME) t->frames_waiting++;
    pthread_cond_signal(&t->has_work);
    pthread_mutex_unlock(&t->lock);
}

void tb_decode_thread_queue_params(struct tb_decode_thread *t, const uint8_t *payload, size_t len) {
    if (!t) return;
    tb_decode_thread_push(t, TB_DECODE_JOB_PARAMS, payload, len);
}

void tb_decode_thread_queue_frame(struct tb_decode_thread *t, const uint8_t *payload, size_t len) {
    if (!t) return;
    tb_decode_thread_push(t, TB_DECODE_JOB_FRAME, payload, len);
}

void tb_decode_thread_reset(struct tb_decode_thread *t) {
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    /* Queued frames belong to the ended session; a frame being decoded
     * right now finishes and is then dropped by the reset job. */
    size_t dropped_frames = 0;
    for (size_t i = 0; i < t->count; i++) {
        if (t->jobs[(t->head + i) % TB_DECODE_QUEUE_CAPACITY].kind == TB_DECODE_JOB_FRAME) {
            dropped_frames++;
        }
    }
    tb_decode_thread_clear_locked(t);
    t->frames_waiting -= dropped_frames;
    pthread_mutex_unlock(&t->lock);
    tb_dec_drop_latest(t->dec);
    tb_decode_thread_push(t, TB_DECODE_JOB_RESET, NULL, 0);
}

int tb_decode_thread_wants_frames(struct tb_decode_thread *t) {
    if (!t) return 1;
    pthread_mutex_lock(&t->lock);
    const int wants = t->frames_waiting < TB_DECODE_QUEUE_TARGET_FRAMES;
    pthread_mutex_unlock(&t->lock);
    return wants;
}

int tb_decode_thread_take_decode_stats(struct tb_decode_thread *t,
                                       struct tb_timing_summary *out) {
    if (!t) return 0;
    pthread_mutex_lock(&t->lock);
    const int have = tb_timing_stats_take_summary(&t->decode_us, out);
    pthread_mutex_unlock(&t->lock);
    return have;
}
