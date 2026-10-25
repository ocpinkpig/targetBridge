#ifndef TB_GESTURE_BRIDGE_H
#define TB_GESTURE_BRIDGE_H

#include <stdint.h>

typedef void (*tb_gesture_space_switch_callback)(int direction, void *context);

void tb_gesture_bridge_install(tb_gesture_space_switch_callback callback, void *context);
void tb_gesture_bridge_set_active(int active);

/* Returns 1 if the given SDL_Window's Cocoa window is on the active macOS
 * Space (or can't be determined), 0 if it is on a different Space. */
int tb_window_on_active_space(void *sdl_window);

/* Keep the fullscreen monitor surface above Notification Center while a
 * session is active, restoring the normal window level on Stop/disconnect. */
void tb_receiver_set_monitor_shield(int active);

/* Return the native pixel dimensions of the screen that hosts the receiver
 * window. This avoids treating a windowed SDL drawable as the panel size. */
int tb_receiver_content_display_pixels(uint32_t *width, uint32_t *height);

#ifdef __OBJC__
@class NSWindow;
/* The receiver's SDL content window: the largest visible window. Found via
 * NSApp rather than SDL_GetWindowWMInfo, which fails on the Intel build. */
NSWindow *tb_receiver_content_window(void);
#endif

#endif
