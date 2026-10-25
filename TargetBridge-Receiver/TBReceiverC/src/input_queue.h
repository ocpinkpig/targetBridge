#ifndef TB_INPUT_QUEUE_H
#define TB_INPUT_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#define TB_INPUT_QUEUE_CAPACITY 128

enum tb_input_event_kind {
    TB_INPUT_EVENT_NONE = 0,
    TB_INPUT_EVENT_MOVE,
    TB_INPUT_EVENT_LEFT_DRAG,
    TB_INPUT_EVENT_RIGHT_DRAG,
    TB_INPUT_EVENT_OTHER_DRAG,
    TB_INPUT_EVENT_SCROLL,
    TB_INPUT_EVENT_LEFT_DOWN,
    TB_INPUT_EVENT_LEFT_UP,
    TB_INPUT_EVENT_RIGHT_DOWN,
    TB_INPUT_EVENT_RIGHT_UP,
    TB_INPUT_EVENT_OTHER_DOWN,
    TB_INPUT_EVENT_OTHER_UP,
    TB_INPUT_EVENT_KEY_DOWN,
    TB_INPUT_EVENT_KEY_UP,
    TB_INPUT_EVENT_SWITCH_PREV_TARGET,
    TB_INPUT_EVENT_SWITCH_NEXT_TARGET,
    TB_INPUT_EVENT_SWITCH_PREV_SPACE,
    TB_INPUT_EVENT_SWITCH_NEXT_SPACE,
    TB_INPUT_EVENT_DEACTIVATE_CONTROL
};

struct tb_input_event {
    enum tb_input_event_kind kind;
    int dx;
    int dy;
    int scroll_x;
    int scroll_y;
    uint16_t key_code;
    int click_count;
    /* SDL_GetTicks() time of the SDL event (motion only; 0 = unknown), for
     * the input-wait latency figure. */
    uint32_t timestamp_ms;
};

struct tb_input_queue {
    struct tb_input_event events[TB_INPUT_QUEUE_CAPACITY];
    size_t head;
    size_t count;
};

/* Returns 1 when the event was queued, 0 when a lossy motion event was
 * discarded, and -1 when overload forced a safe control deactivation. */
int tb_input_queue_push(struct tb_input_queue *queue,
                        const struct tb_input_event *event);
int tb_input_queue_pop(struct tb_input_queue *queue,
                       struct tb_input_event *event);

#endif
