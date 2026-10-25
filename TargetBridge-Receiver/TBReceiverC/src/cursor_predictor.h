/* cursor_predictor.h — receiver-side cursor prediction for receiverMaster.
 *
 * The Sender moves its cursor by exactly the dx/dy the Receiver sends (in
 * Sender display points), so the Receiver can show the result immediately
 * instead of waiting a network round trip:
 *
 *   predicted = last confirmed Sender position + every move sent since,
 *
 * applied one move at a time with the same edge clamp the Sender uses.
 * "Confirmed" uses the move stamps: a cursor packet echoing stamp E reflects
 * every move up to and including E, so those leave the pending list and the
 * packet's position becomes the new base. Any mismatch (the Sender crossed
 * onto another display, dropped a move, a local mouse moved) is corrected by
 * the next echoed packet.
 *
 * Pure C with no SDL/Cocoa, so it is unit-tested on its own. */

#ifndef TB_CURSOR_PREDICTOR_H
#define TB_CURSOR_PREDICTOR_H

#include <stddef.h>
#include <stdint.h>

#define TB_CURSOR_PREDICTOR_MAX_PENDING 512
/* A move unconfirmed this long will never be (e.g. the Sender left overlay
 * mode); stop predicting with it. */
#define TB_CURSOR_PREDICTOR_MAX_AGE_US 500000

struct tb_cursor_move {
    uint64_t t;
    int      dx;
    int      dy;
};

struct tb_cursor_predictor {
    /* Last confirmed Sender cursor, in Sender display points. */
    double auth_x;
    double auth_y;
    int    auth_visible;
    int    have_auth;
    /* Sender display size in points (0 = unknown, e.g. an older Sender,
     * which disables prediction) and the stream's pixel grid. */
    double point_w;
    double point_h;
    int    stream_w;
    int    stream_h;
    /* Moves sent but not confirmed yet, oldest first. */
    struct tb_cursor_move pending[TB_CURSOR_PREDICTOR_MAX_PENDING];
    size_t head;
    size_t count;
};

void tb_cursor_predictor_reset(struct tb_cursor_predictor *p);

/* Forgets unconfirmed moves (e.g. control left receiverMaster) but keeps the
 * confirmed position and the learnt display size. */
void tb_cursor_predictor_clear_pending(struct tb_cursor_predictor *p);

/* Records a move or drag delta just sent to the Sender with stamp t. */
void tb_cursor_predictor_on_move(struct tb_cursor_predictor *p,
                                 uint64_t t, int dx, int dy);

/* Applies a cursor packet from the Sender. echo == 0: the packet answers no
 * particular move. point_w/point_h <= 0: not sent (older Sender).
 * Returns 1 and sets *error_tenths_pt to how far the prediction for that
 * state was off (in 0.1 pt) when the packet allowed measuring it. */
int tb_cursor_predictor_on_cursor(struct tb_cursor_predictor *p,
                                  int x_px, int y_px,
                                  int stream_w, int stream_h,
                                  int visible,
                                  double point_w, double point_h,
                                  uint64_t echo,
                                  uint64_t *error_tenths_pt);

/* Predicted cursor on the stream's pixel grid. Returns 0 when there is
 * nothing to predict (no unconfirmed moves, or prediction unavailable); the
 * caller then shows the Sender's latest packet as is. */
int tb_cursor_predictor_position(struct tb_cursor_predictor *p,
                                 uint64_t now_us,
                                 int *x_px, int *y_px, int *visible);

#endif
