#include "cursor_predictor.h"

#include <math.h>
#include <string.h>

void tb_cursor_predictor_reset(struct tb_cursor_predictor *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
}

void tb_cursor_predictor_clear_pending(struct tb_cursor_predictor *p) {
    if (!p) return;
    p->head = 0;
    p->count = 0;
}

static struct tb_cursor_move *tb_pending_at(struct tb_cursor_predictor *p, size_t i) {
    return &p->pending[(p->head + i) % TB_CURSOR_PREDICTOR_MAX_PENDING];
}

static void tb_pending_drop_oldest(struct tb_cursor_predictor *p) {
    if (p->count == 0) return;
    p->head = (p->head + 1) % TB_CURSOR_PREDICTOR_MAX_PENDING;
    p->count--;
}

void tb_cursor_predictor_on_move(struct tb_cursor_predictor *p,
                                 uint64_t t, int dx, int dy) {
    if (!p || t == 0) return;
    if (p->count == TB_CURSOR_PREDICTOR_MAX_PENDING) tb_pending_drop_oldest(p);
    struct tb_cursor_move *m = tb_pending_at(p, p->count);
    m->t = t;
    m->dx = dx;
    m->dy = dy;
    p->count++;
}

static int tb_predictor_ready(const struct tb_cursor_predictor *p) {
    return p->have_auth && p->point_w > 0 && p->point_h > 0 &&
           p->stream_w > 0 && p->stream_h > 0;
}

/* Matches the Sender: a move that would leave the display is held at its
 * last point/pixel (frame.maxX - 1). Crossing onto a neighbouring Sender
 * display shows up as an echoed visible=false instead. */
static double tb_clamp(double v, double size) {
    if (v < 0) return 0;
    if (v > size - 1) return size - 1;
    return v;
}

/* Base position plus pending moves, optionally only those with t <= upto. */
static void tb_predictor_replay(const struct tb_cursor_predictor *p,
                                uint64_t upto,
                                double *x, double *y) {
    double px = p->auth_x;
    double py = p->auth_y;
    for (size_t i = 0; i < p->count; i++) {
        const struct tb_cursor_move *m =
            &p->pending[(p->head + i) % TB_CURSOR_PREDICTOR_MAX_PENDING];
        if (upto && m->t > upto) break;
        px = tb_clamp(px + m->dx, p->point_w);
        py = tb_clamp(py + m->dy, p->point_h);
    }
    *x = px;
    *y = py;
}

int tb_cursor_predictor_on_cursor(struct tb_cursor_predictor *p,
                                  int x_px, int y_px,
                                  int stream_w, int stream_h,
                                  int visible,
                                  double point_w, double point_h,
                                  uint64_t echo,
                                  uint64_t *error_tenths_pt) {
    if (!p) return 0;
    if (point_w > 0 && point_h > 0) {
        p->point_w = point_w;
        p->point_h = point_h;
    }
    if (stream_w > 0 && stream_h > 0) {
        p->stream_w = stream_w;
        p->stream_h = stream_h;
    }
    if (p->point_w <= 0 || p->point_h <= 0 || p->stream_w <= 0 || p->stream_h <= 0) {
        return 0;  /* older Sender: nothing to predict with */
    }

    const double x_pt = (double)x_px * p->point_w / (double)p->stream_w;
    const double y_pt = (double)y_px * p->point_h / (double)p->stream_h;

    if (echo == 0) {
        /* A timer packet: while moves are unconfirmed we cannot tell which
         * of them it includes, so keep the base and take only visibility. */
        if (p->count == 0) {
            p->auth_x = x_pt;
            p->auth_y = y_pt;
            p->have_auth = 1;
        }
        p->auth_visible = visible;
        return 0;
    }

    int measured = 0;
    if (p->have_auth && error_tenths_pt) {
        double ex, ey;
        tb_predictor_replay(p, echo, &ex, &ey);
        /* The packet's position was rounded down onto the stream grid. */
        const double grid_x = p->point_w / (double)p->stream_w;
        const double grid_y = p->point_h / (double)p->stream_h;
        double dx = fabs(ex - x_pt);
        double dy = fabs(ey - y_pt);
        dx = dx > grid_x ? dx - grid_x : 0;
        dy = dy > grid_y ? dy - grid_y : 0;
        *error_tenths_pt = (uint64_t)llround(sqrt(dx * dx + dy * dy) * 10.0);
        measured = 1;
    }

    while (p->count > 0 && tb_pending_at(p, 0)->t <= echo) tb_pending_drop_oldest(p);
    p->auth_x = x_pt;
    p->auth_y = y_pt;
    p->auth_visible = visible;
    p->have_auth = 1;
    return measured;
}

int tb_cursor_predictor_position(struct tb_cursor_predictor *p,
                                 uint64_t now_us,
                                 int *x_px, int *y_px, int *visible) {
    if (!p) return 0;
    while (p->count > 0 &&
           now_us > tb_pending_at(p, 0)->t + TB_CURSOR_PREDICTOR_MAX_AGE_US) {
        tb_pending_drop_oldest(p);
    }
    if (p->count == 0 || !tb_predictor_ready(p)) return 0;

    double x_pt, y_pt;
    tb_predictor_replay(p, 0, &x_pt, &y_pt);
    if (x_px) *x_px = (int)(x_pt * (double)p->stream_w / p->point_w);
    if (y_px) *y_px = (int)(y_pt * (double)p->stream_h / p->point_h);
    if (visible) *visible = p->auth_visible;
    return 1;
}
