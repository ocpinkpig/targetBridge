/* decode_thread.h — runs the video decoder on its own thread.
 *
 * At 5K a VideoToolbox decode took ~30 ms on the Intel receiver. Done on the
 * event-loop thread, it held up mouse input and the cursor for 100 ms or
 * more per socket read. Here the loop only copies frame packets into a
 * queue and shows the newest decoded frame (tb_dec_present_latest); the
 * decoding, which must stay in order, happens on this worker.
 *
 * Back-pressure: tb_decode_thread_wants_frames() turns false while a couple
 * of frames are waiting, so the loop stops reading the socket and TCP slows
 * the Sender, which then drops frames before encoding instead of queueing
 * them. */

#ifndef TB_DECODE_THREAD_H
#define TB_DECODE_THREAD_H

#include <stddef.h>
#include <stdint.h>

#include "timing_stats.h"

struct tb_decoder;
struct tb_decode_thread;

/* Frames allowed to wait before the loop should stop reading the socket. */
#define TB_DECODE_QUEUE_TARGET_FRAMES 2
/* Hard cap on queued jobs; queueing past it blocks until one finishes. */
#define TB_DECODE_QUEUE_CAPACITY 32

struct tb_decode_thread *tb_decode_thread_create(struct tb_decoder *dec);
void tb_decode_thread_destroy(struct tb_decode_thread *t);

/* Queue parameter sets / one AVCC frame. The bytes are copied. */
void tb_decode_thread_queue_params(struct tb_decode_thread *t, const uint8_t *payload, size_t len);
void tb_decode_thread_queue_frame(struct tb_decode_thread *t, const uint8_t *payload, size_t len);

/* Session ended: drop queued work and the undisplayed frame, then reset the
 * decoder (on the worker, after anything it is decoding right now). */
void tb_decode_thread_reset(struct tb_decode_thread *t);

/* 1 while fewer than TB_DECODE_QUEUE_TARGET_FRAMES frames are waiting. */
int  tb_decode_thread_wants_frames(struct tb_decode_thread *t);

/* Read and reset per-frame decode time (µs). Returns 0 with no samples. */
int  tb_decode_thread_take_decode_stats(struct tb_decode_thread *t,
                                        struct tb_timing_summary *out);

#endif
