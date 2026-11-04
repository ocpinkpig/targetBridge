/* tb_cursor_layer.h — the overlay cursor as its own Core Animation layer.
 *
 * Moving a layer is a property change the window server composites on its
 * own, so the cursor no longer waits for (or forces) a full redraw of the
 * video frame. That matters most at 5K, where each redraw is expensive and
 * the event loop is busy decoding. Uses the Receiver Mac's native cursor
 * images, keyed by the Sender's cursor type. */

#ifndef TB_CURSOR_LAYER_H
#define TB_CURSOR_LAYER_H

/* Returns 1 when the layer is available. Off when TB_RECEIVER_CURSOR_LAYER=0
 * (the SDL-drawn cursor is used instead, e.g. to compare the two). */
int tb_cursor_layer_enabled(void);

/* Shows or hides the cursor. x/y are fractions of the video area (0..1,
 * top-left origin) and match how the frame is stretched over the window.
 * type is the Sender's cursor type; large selects the 2x cursor. Cheap to
 * call often: nothing is sent to the window server when nothing changed. */
void tb_cursor_layer_update(int visible, double x, double y, int type, int large);

void tb_cursor_layer_destroy(void);

#endif
