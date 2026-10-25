#import "tb_cursor_layer.h"
#import "tb_gesture_bridge.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <stdlib.h>
#include <string.h>

/* Transparent view above SDL's video surface that never takes input. */
@interface TBCursorOverlayView : NSView
@end

@implementation TBCursorOverlayView
- (NSView *)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return NO;
}
@end

static TBCursorOverlayView *g_view;
static CALayer *g_layer;
static int g_enabled = -1;

/* What the layer currently shows, to skip no-op window-server commits. */
static int g_shown;
static double g_x = -1, g_y = -1;
static int g_type = -1, g_large = -1;
static CGFloat g_scale;
static NSSize g_view_size;

int tb_cursor_layer_enabled(void) {
    if (g_enabled < 0) {
        const char *env = getenv("TB_RECEIVER_CURSOR_LAYER");
        g_enabled = !(env && strcmp(env, "0") == 0);
        fprintf(stderr, "[disp] cursor layer %s\n", g_enabled ? "on" : "off (SDL-drawn cursor)");
    }
    return g_enabled;
}

static NSCursor *tb_private_cursor(NSString *name) {
    SEL sel = NSSelectorFromString(name);
    if (![NSCursor respondsToSelector:sel]) return nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    id cursor = [NSCursor performSelector:sel];
#pragma clang diagnostic pop
    return [cursor isKindOfClass:[NSCursor class]] ? cursor : nil;
}

/* The Sender's cursor types (see its standardCursorPngs table). */
static NSCursor *tb_cursor_for_type(int type) {
    NSCursor *cursor = nil;
    switch (type) {
    case 1: cursor = [NSCursor IBeamCursor]; break;
    case 2: cursor = [NSCursor pointingHandCursor]; break;
    case 3:
        cursor = tb_private_cursor(@"_windowResizeEastWestCursor");
        if (!cursor) cursor = [NSCursor resizeLeftRightCursor];
        break;
    case 4:
        cursor = tb_private_cursor(@"_windowResizeNorthSouthCursor");
        if (!cursor) cursor = [NSCursor resizeUpDownCursor];
        break;
    case 5: cursor = [NSCursor openHandCursor]; break;
    case 6: cursor = [NSCursor crosshairCursor]; break;
    case 7: cursor = tb_private_cursor(@"_windowResizeNorthWestSouthEastCursor"); break;
    case 8: cursor = tb_private_cursor(@"_windowResizeNorthEastSouthWestCursor"); break;
    default: break;
    }
    return cursor ? cursor : [NSCursor arrowCursor];
}

static void tb_cursor_layer_reset_cache(void) {
    g_shown = 0;
    g_x = g_y = -1;
    g_type = g_large = -1;
    g_scale = 0;
    g_view_size = NSZeroSize;
}

/* Attaches to the content window, or re-stacks above SDL's own subviews
 * (its Metal view) if something was added on top since. */
static BOOL tb_cursor_layer_attach(void) {
    NSWindow *window = tb_receiver_content_window();
    NSView *content = window.contentView;
    if (!content) return NO;

    if (g_view && g_view.superview == content) {
        if (content.subviews.lastObject != g_view) {
            [g_view removeFromSuperview];
            [content addSubview:g_view positioned:NSWindowAbove relativeTo:nil];
        }
        return YES;
    }

    [g_view removeFromSuperview];
    g_view = [[TBCursorOverlayView alloc] initWithFrame:content.bounds];
    g_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    g_view.wantsLayer = YES;

    g_layer = [CALayer layer];
    g_layer.hidden = YES;
    g_layer.contentsGravity = kCAGravityResize;
    /* Native cursors get their shadow from the window server; NSCursor
     * images do not include it. */
    g_layer.shadowOpacity = 0.35f;
    g_layer.shadowRadius = 1.5;
    g_layer.shadowOffset = CGSizeMake(0, -1.5);
    /* Never animate: an implicit 0.25 s animation would add the very lag
     * this layer exists to remove. */
    g_layer.actions = @{
        @"position": [NSNull null],
        @"bounds": [NSNull null],
        @"anchorPoint": [NSNull null],
        @"contents": [NSNull null],
        @"hidden": [NSNull null],
    };
    [g_view.layer addSublayer:g_layer];
    [content addSubview:g_view positioned:NSWindowAbove relativeTo:nil];
    tb_cursor_layer_reset_cache();
    return YES;
}

void tb_cursor_layer_update(int visible, double x, double y, int type, int large) {
    if (!tb_cursor_layer_enabled()) return;
    if (!visible && (!g_layer || !g_shown)) return;
    if (!tb_cursor_layer_attach()) return;

    const NSSize size = g_view.bounds.size;
    const CGFloat scale = g_view.window.backingScaleFactor > 0 ? g_view.window.backingScaleFactor : 1.0;
    const BOOL shape_changed = type != g_type || large != g_large || scale != g_scale;
    const BOOL moved = x != g_x || y != g_y || !NSEqualSizes(size, g_view_size);
    if (!shape_changed && !moved && visible == g_shown) return;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (shape_changed) {
        NSCursor *cursor = tb_cursor_for_type(type);
        NSImage *image = cursor.image;
        const NSSize image_size = image.size;
        const CGFloat factor = large ? 2.0 : 1.0;
        if (image_size.width > 0 && image_size.height > 0) {
            g_layer.contents = [image layerContentsForContentsScale:scale * factor];
            g_layer.contentsScale = scale;
            g_layer.bounds = CGRectMake(0, 0, image_size.width * factor, image_size.height * factor);
            /* The hot spot is measured from the image's top-left; the view is
             * not flipped, so anchor y counts from the bottom. */
            const NSPoint hot = cursor.hotSpot;
            g_layer.anchorPoint = CGPointMake(hot.x / image_size.width,
                                              1.0 - hot.y / image_size.height);
        }
    }
    if (visible) {
        g_layer.position = CGPointMake(x * size.width, (1.0 - y) * size.height);
    }
    g_layer.hidden = !visible;
    [CATransaction commit];
    /* Push the change to the window server now; this loop is not driven by
     * NSApplication's run loop, which would otherwise commit it later. */
    [CATransaction flush];

    g_shown = visible;
    g_x = x;
    g_y = y;
    g_type = type;
    g_large = large;
    g_scale = scale;
    g_view_size = size;
}

void tb_cursor_layer_destroy(void) {
    [g_view removeFromSuperview];
    g_view = nil;
    g_layer = nil;
    tb_cursor_layer_reset_cache();
}
