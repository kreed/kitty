/*
 * wl_client_side_decorations.c
 * Copyright (C) 2021 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "wl_client_side_decorations.h"
#include "linux_desktop_settings.h"

#include "backend_utils.h"
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
// Needed for the BTN_* defines
#ifdef __has_include
#if __has_include(<linux/input.h>)
#include <linux/input.h>
#elif __has_include(<dev/evdev/input.h>)
#include <dev/evdev/input.h>
#endif
#else
#include <linux/input.h>
#endif

#define decs window->wl.decorations
#define debug debug_rendering
#define decoration_touch _glfw.wl.decoration_touch

#define ARGB(a, r, g, b) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))
#define A(x) (((x) >> 24) & 0xff)
#define R(x) (((x) >> 16) & 0xff)
#define G(x) (((x) >> 8) & 0xff)
#define B(x) ((x) & 0xff)
#define SWAP(x, y)              \
    do {                        \
        __typeof__(x) SWAP = x; \
        x = y;                  \
        y = SWAP;               \
    } while (0)

// shadow tile  {{{
typedef float kernel_type;

// The radius the window is rounded to, matching window_mask.slang. The shadow
// silhouette and the controls pop-over both have to use it, or they square off
// the corner the mask carves away.
#define WINDOW_CORNER_RADIUS 12.0

static void
build_blur_kernel(kernel_type *blur_kernel, const size_t size, kernel_type sigma) {
    // 1D Normalized Gaussian
    const kernel_type half = size / (kernel_type)2;
    kernel_type sum = 0;
    for (size_t i = 0; i < size; i++) {
        kernel_type f = (i - half);
        blur_kernel[i] = (kernel_type)exp(-f * f / sigma);
        sum += blur_kernel[i];
    }
    for (size_t i = 0; i < size; i++) blur_kernel[i] /= sum;
}

static void
blur_mask(
    kernel_type *image_data,
    ssize_t width,
    ssize_t height,
    ssize_t kernel_size,
    kernel_type sigma,
    kernel_type *scratch,
    kernel_type *blur_kernel,
    ssize_t margin) {
    (void)margin;
    build_blur_kernel(blur_kernel, kernel_size, sigma);
    const size_t half = kernel_size / 2;

    for (ssize_t y = 0; y < height; y++) {
        kernel_type *s = image_data + y * width, *d = scratch + y * width;
        for (ssize_t x = 0; x < width; x++) {
            kernel_type a = 0;
            for (ssize_t k = 0; k < kernel_size; k++) {
                const ssize_t px = x + k - half;
                if (0 <= px && px < width) a += s[px] * blur_kernel[k];
            }
            d[x] = a;
        }
    }

    for (ssize_t y = 0; y < height; y++) {
        kernel_type *d = image_data + y * width;
        for (ssize_t x = 0; x < width; x++) {
            kernel_type a = 0;
            for (ssize_t k = 0; k < kernel_size; k++) {
                const ssize_t py = y + k - half;
                if (0 <= py && py < height) {
                    kernel_type *s = scratch + py * width;
                    a += s[x] * blur_kernel[k];
                }
            }
            d[x] = a;
        }
    }
}

static kernel_type *
create_shadow_mask(size_t width, size_t height, size_t margin, size_t kernel_size, kernel_type base_alpha, kernel_type sigma, double radius) {
    kernel_type *mask = calloc(2 * width * height + kernel_size, sizeof(kernel_type));
    if (!mask) return NULL;
    for (size_t y = margin; y < height - margin; y++) {
        kernel_type *row = mask + y * width;
        for (size_t x = margin; x < width - margin; x++) {
            double dx = fmax(margin + radius - x - .5, x + .5 - (width - margin - radius));
            double dy = margin + radius - y - .5;
            row[x] = radius > 0 && dx > 0 && dy > 0 ? base_alpha * (kernel_type)fmax(0, fmin(1, radius - hypot(dx, dy) + .5)) : base_alpha;
        }
    }
    blur_mask(mask, width, height, kernel_size, sigma, mask + width * height, (kernel_type *)(mask + 2 * width * height), margin);
    return mask;
}

#define st decs.shadow_tile

static size_t
create_shadow_tile(_GLFWwindow *window) {
    const size_t margin = (size_t)round(decs.metrics.width * decs.for_window_state.fscale);
    if (st.data && st.for_decoration_size == margin) return margin;
    st.for_decoration_size = margin;
    if (!margin) { // absurdly small scale, no shadows
        free(st.data);
        st.data = NULL;
        return 0;
    }
    st.segments = 7;
    st.stride = st.segments * margin;
    st.corner_size = margin * (st.segments - 1) / 2;
    kernel_type *mask = create_shadow_mask(st.stride, st.stride, margin, 2 * margin + 1, (kernel_type)0.46, 32 * margin, !decs.titlebar_hidden ? margin : 0);
    free(st.data);
    st.data = NULL;
    if (!mask) return margin;
    // Fill a local buffer and only publish it to st.data once complete, this
    // also avoids a false positive use-after-free warning from CodeQL
    uint32_t *data = malloc(sizeof(data[0]) * st.stride * st.stride);
    if (data)
        for (size_t y = 0; y < st.stride; y++)
            for (size_t x = 0; x < st.stride; x++) {
                double alpha = mask[y * st.stride + x];
                // The blur has finite buffers. Fade to a rounded outer envelope
                // instead of exposing their rectangular edges on the desktop.
                const double feather = fmax(1, 3 * decs.for_window_state.fscale);
                const bool upper = y < st.stride / 2;
                const double radius = margin * (upper && !decs.titlebar_hidden ? 2. : 1.);
                const double dx = fmax(radius - x - .5, x + .5 - (st.stride - radius));
                const double dy = upper ? radius - y - .5 : y + .5 - (st.stride - radius);
                double distance = fmin(fmin(x + .5, st.stride - x - .5), fmin(y + .5, st.stride - y - .5));
                if (dx > 0 && dy > 0) distance = radius - hypot(dx, dy);
                double coverage = fmax(0, fmin(1, distance / feather));
                alpha *= coverage * coverage * (3 - 2 * coverage);
                data[y * st.stride + x] = ((uint32_t)(alpha * 255)) << 24;
            }
    free(mask);
    st.data = data;
    return margin;
}


// }}}

static bool
window_needs_shadows(_GLFWwindow *w) {
    (void)w;
    // No drop shadow. The shadow is drawn as square tiles ringing the window's
    // bounding box, so it fills the notch the corner mask carves out and the
    // corners read as square against any background dark enough to show it.
    // Rounding the silhouette is not enough: render_shadows() slices only the
    // outermost margin of the tile into the corner surfaces, which is outside
    // the rounded part. The painted frame carries the window edge instead.
    return false;
}

static void
swap_buffers(_GLFWWaylandBufferPair *pair) {
    SWAP(pair->front, pair->back);
    SWAP(pair->data.front, pair->data.back);
}

static size_t
init_buffer_pair(_GLFWWaylandBufferPair *pair, size_t width, size_t height, double scale) {
    memset(pair, 0, sizeof(_GLFWWaylandBufferPair));
    // wl_shm buffers must have non-zero size
    pair->width = MAX(1, (int)round(width * scale));
    pair->height = MAX(1, (int)round(height * scale));
    pair->viewport_width = width;
    pair->viewport_height = height;
    pair->stride = 4 * pair->width;
    pair->size_in_bytes = pair->stride * pair->height;
    return 2 * pair->size_in_bytes;
}

#define all_shadow_surfaces(Q) \
    Q(shadow_left);            \
    Q(shadow_top);             \
    Q(shadow_right);           \
    Q(shadow_bottom);          \
    Q(shadow_upper_left);      \
    Q(shadow_upper_right);     \
    Q(shadow_lower_left);      \
    Q(shadow_lower_right);
#define all_surfaces(Q) \
    Q(titlebar);        \
    all_shadow_surfaces(Q);

static bool
mark_buffer_released(_GLFWwindow *window, struct wl_buffer *q) {
#define Q(which)                          \
    if (decs.which.buffer.a == q) {       \
        decs.which.buffer.a_busy = false; \
        return true;                      \
    }                                     \
    if (decs.which.buffer.b == q) {       \
        decs.which.buffer.b_busy = false; \
        return true;                      \
    }
    all_surfaces(Q);
#undef Q
    return false;
}

static void on_titlebar_buffer_released(_GLFWwindow *window);

static void
buffer_release_event(void *data, struct wl_buffer *buffer) {
    _GLFWwindow *window = _glfwWindowForId((uintptr_t)data);
    if (!window || !mark_buffer_released(window, buffer)) {
        // A buffer that was busy when the buffers it belonged to were freed
        wl_buffer_destroy(buffer);
        return;
    }
    if (buffer == decs.titlebar.buffer.back) on_titlebar_buffer_released(window);
}

static struct wl_buffer_listener handle_buffer_events = {.release = buffer_release_event};

static void
alloc_buffer_pair(uintptr_t window_id, _GLFWWaylandBufferPair *pair, struct wl_shm_pool *pool, uint8_t *data, size_t *offset) {
    pair->data.a = data + *offset;
    pair->a = wl_shm_pool_create_buffer(pool, *offset, pair->width, pair->height, pair->stride, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(pair->a, &handle_buffer_events, (void *)window_id);
    *offset += pair->size_in_bytes;
    pair->data.b = data + *offset;
    pair->b = wl_shm_pool_create_buffer(pool, *offset, pair->width, pair->height, pair->stride, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(pair->b, &handle_buffer_events, (void *)window_id);
    *offset += pair->size_in_bytes;
    pair->front = pair->a;
    pair->back = pair->b;
    pair->data.front = pair->data.a;
    pair->data.back = pair->data.b;
}

void
csd_initialize_metrics(_GLFWwindow *window) {
    decs.metrics.width = 12;
    decs.metrics.top = decs.metrics.width + glfw_current_titlebar_height();
    decs.metrics.visible_titlebar_height = decs.metrics.top - decs.metrics.width;
    decs.metrics.horizontal = 2 * decs.metrics.width;
    decs.metrics.vertical = decs.metrics.width + decs.metrics.top;
}

// All geometry is in logical pixels; coverage is evaluated in buffer pixels.
static uint32_t
coverage_pixel(uint32_t color, double coverage) {
    unsigned a = (unsigned)(255 * fmax(0, fmin(1, coverage)));
    return ARGB(a, R(color) * a / 255, G(color) * a / 255, B(color) * a / 255);
}

static double
segment_distance(double x, double y, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double t = fmax(0, fmin(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)));
    return hypot(x - ax - t * dx, y - ay - t * dy);
}

static uint32_t
blend_colors(uint32_t a, uint32_t b, double t) {
    t = fmax(0, fmin(1, t));
    return ARGB(255u, (unsigned)(R(a) * (1 - t) + R(b) * t), (unsigned)(G(a) * (1 - t) + G(b) * t), (unsigned)(B(a) * (1 - t) + B(b) * t));
}

typedef struct {
    uint32_t bg, fg, button, hovered_button, pressed_button;
} TitlebarColors;

// The pop-over reads as part of the frame, so it takes the frame's own base
// color rather than the titlebar palette. Keep the base colors in step with
// window_frame.slang. Button backgrounds are tints of it, as upstream does
// for its titlebar.
static TitlebarColors
popover_colors(void) {
    const bool dark = glfwGetCurrentSystemColorTheme(true) == GLFW_COLOR_SCHEME_DARK;
    TitlebarColors ans;
    ans.bg = dark ? 0xff17171a : 0xfff0f0f4;
    ans.fg = dark ? 0xffe6e6e6 : 0xff444444;
    static const double dark_tints[3] = {.116, .232, .31}, light_tints[3] = {.09, .187, .277};
    const double *tints = dark ? dark_tints : light_tints;
    const uint32_t tint = dark ? 0xffffffff : 0xff000000;
    ans.button = blend_colors(ans.bg, tint, tints[0]);
    ans.hovered_button = blend_colors(ans.bg, tint, tints[1]);
    ans.pressed_button = blend_colors(ans.bg, tint, tints[2]);
    return ans;
}

typedef struct {
    GLFWTitlebarButton which;
    int left;
} PlacedTitlebarButton;

static bool
is_titlebar_button_available(_GLFWwindow *window, GLFWTitlebarButton which) {
    switch (which) {
        case TITLEBAR_BUTTON_CLOSE: return true;
        case TITLEBAR_BUTTON_MAXIMIZE: return window->wl.wm_capabilities.maximize;
        case TITLEBAR_BUTTON_MINIMIZE: return window->wl.wm_capabilities.minimize;
        case TITLEBAR_BUTTON_NONE:
        case TITLEBAR_BUTTON_COUNT: break;
    }
    return false;
}

// Only x is checked so that a mouse button pressed on a titlebar button
// still activates it when released after the pointer is dragged out of the
// titlebar, vertically.
static GLFWTitlebarButton
titlebar_button_at_x(_GLFWwindow *window, double sx) {
    const int scaled_x = (int)round(decs.for_window_state.fscale * sx);
#define button_at(which, value) \
    if (decs.which.left <= scaled_x && scaled_x < decs.which.left + decs.which.width) return value;
    button_at(close, TITLEBAR_BUTTON_CLOSE);
    button_at(maximize, TITLEBAR_BUTTON_MAXIMIZE);
    button_at(minimize, TITLEBAR_BUTTON_MINIMIZE);
#undef button_at
    return TITLEBAR_BUTTON_NONE;
}

static GLFWTitlebarButton
titlebar_button_at(_GLFWwindow *window, double sx, double sy) {
    if (sy < 0 || sy >= decs.metrics.visible_titlebar_height) return TITLEBAR_BUTTON_NONE;
    return titlebar_button_at_x(window, sx);
}

static void
activate_titlebar_button(_GLFWwindow *window, GLFWTitlebarButton button) {
    switch (button) {
        case TITLEBAR_BUTTON_CLOSE: _glfwInputWindowCloseRequest(window); break;
        case TITLEBAR_BUTTON_MINIMIZE: _glfwPlatformIconifyWindow(window); break;
        case TITLEBAR_BUTTON_MAXIMIZE:
            if (window->wl.current.toplevel_states & TOPLEVEL_STATE_MAXIMIZED) _glfwPlatformRestoreWindow(window);
            else _glfwPlatformMaximizeWindow(window);
            break;
        case TITLEBAR_BUTTON_NONE:
        case TITLEBAR_BUTTON_COUNT: break;
    }
}

static _GLFWCSDSurface
decoration_surface(_GLFWwindow *window, struct wl_surface *surface) {
#define surface_at(which) \
    if (surface && surface == decs.which.surface) return CSD_##which;
    all_surfaces(surface_at);
#undef surface_at
    return CENTRAL_WINDOW;
}

static uint32_t
decoration_resize_edges(_GLFWCSDSurface surface) {
    switch (surface) {
        case CSD_shadow_left: return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
        case CSD_shadow_upper_left: return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
        case CSD_shadow_right: return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
        // The top edge moves the window and the top right corner, under the
        // window controls, does nothing.
        case CSD_shadow_upper_right:
        case CSD_shadow_top: return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
        case CSD_shadow_lower_left: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
        case CSD_shadow_bottom: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
        case CSD_shadow_lower_right: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
        case CENTRAL_WINDOW:
        case CSD_titlebar: return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    }
    return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
}

// Places the buttons from the desktop button layout on the left and right
// sides of the titlebar, returns the number of buttons placed.
static unsigned
layout_titlebar_buttons(_GLFWwindow *window, int width, int slot, int pad, PlacedTitlebarButton *ans, int *side_margin) {
    const GLFWTitlebarButtonLayout *layout = glfw_current_titlebar_button_layout();
    bool visible[TITLEBAR_BUTTON_COUNT] = {0};
    unsigned count = 0;
    for (unsigned side = 0; side < 2; side++) {
        const GLFWTitlebarButton *buttons = side ? layout->right : layout->left;
        const unsigned n = side ? layout->right_count : layout->left_count;
        for (unsigned i = 0; i < n; i++) {
            if (!visible[buttons[i]] && is_titlebar_button_available(window, buttons[i])) {
                visible[buttons[i]] = true;
                count++;
            }
        }
    }
    // Hide buttons that dont fit in narrow windows, close is hidden last
    const unsigned capacity = width > 2 * pad ? (unsigned)((width - 2 * pad) / slot) : 0;
    static const GLFWTitlebarButton hide_order[] = {TITLEBAR_BUTTON_MINIMIZE, TITLEBAR_BUTTON_MAXIMIZE, TITLEBAR_BUTTON_CLOSE};
    for (unsigned i = 0; i < arraysz(hide_order) && count > capacity; i++) {
        if (visible[hide_order[i]]) {
            visible[hide_order[i]] = false;
            count--;
        }
    }
    unsigned used = 0;
    int side_count[2] = {0};
    for (unsigned side = 0; side < 2; side++) {
        const GLFWTitlebarButton *buttons = side ? layout->right : layout->left;
        const unsigned n = side ? layout->right_count : layout->left_count;
        for (unsigned i = 0; i < n; i++)
            if (visible[buttons[i]]) side_count[side]++;
        int left = side ? width - pad - side_count[side] * slot : pad;
        for (unsigned i = 0; i < n; i++) {
            if (visible[buttons[i]]) {
                visible[buttons[i]] = false; // guard against duplicates
                ans[used].which = buttons[i];
                ans[used++].left = left;
                left += slot;
            }
        }
    }
    *side_margin = pad + MAX(side_count[0], side_count[1]) * slot;
    return used;
}

// The buttons the desktop layout asks for and the compositor supports, the
// pop-over holds exactly these.
static unsigned
popover_button_count(_GLFWwindow *window) {
    PlacedTitlebarButton buttons[TITLEBAR_BUTTON_COUNT];
    int margin;
    return layout_titlebar_buttons(window, TITLEBAR_BUTTON_COUNT, 1, 0, buttons, &margin);
}

// The titlebar holds nothing but the window controls, so it is exactly as
// wide as the buttons it draws and is mapped only while revealed.
static unsigned
popover_width(_GLFWwindow *window) {
    return popover_button_count(window) * decs.metrics.visible_titlebar_height;
}

static void
render_titlebar_button(
    uint32_t *pixels,
    int width,
    int height,
    int left,
    int slot,
    double scale,
    GLFWTitlebarButton which,
    bool maximized,
    uint32_t fill,
    uint32_t fg,
    uint32_t bg) {
    for (int y = 0; y < height; y++)
        for (int x = left; x < left + slot; x++) {
            const double lx = (x + .5 - left - slot / 2.) / scale, ly = (y + .5 - height / 2.) / scale;
            // a circle of radius 12 with the icon drawn as lines of width 1
            const double coverage = fmax(0, fmin(1, (12 - hypot(lx, ly)) * scale + .5));
            double d;
            switch (which) {
                case TITLEBAR_BUTTON_CLOSE: d = fmin(segment_distance(lx, ly, -4, -4, 4, 4), segment_distance(lx, ly, -4, 4, 4, -4)); break;
                case TITLEBAR_BUTTON_MINIMIZE: d = segment_distance(lx, ly, -4, 3, 4, 3); break;
                default: {
                    const double bx = fabs(lx) - 3.5, by = fabs(ly) - 3.5;
                    d = (bx > 0 || by > 0) ? hypot(fmax(0, bx), fmax(0, by)) : fmin(-bx, -by);
                    if (maximized && fmin(segment_distance(lx, ly, -1, -5, 5, -5), segment_distance(lx, ly, 5, -5, 5, 1)) < .75) d = 0;
                } break;
            }
            const double ink = (.5 - d) * scale + .5;
            pixels[y * width + x] = blend_colors(bg, blend_colors(fill, fg, ink), coverage);
        }
}

// The pop-over sits over the top right corner window_mask.slang carves away,
// so it rounds that corner to match or it would square it off.
static void
round_top_right_corner(uint32_t *pixels, int width, int height, double radius) {
    if (radius <= 0 || width < 2 || height < 2) return;
    const double cx = width - radius, cy = radius;
    for (int y = 0; y < height && y < cy; y++)
        for (int x = MAX(0, (int)cx); x < width; x++) {
            const double dx = x + .5 - cx, dy = y + .5 - cy;
            if (dx <= 0 || dy >= 0) continue; // outside the corner quadrant
            const double coverage = radius - hypot(dx, dy) + .5;
            if (coverage < 1) pixels[y * width + x] = coverage_pixel(pixels[y * width + x], coverage);
        }
}

static void
render_title_bar(_GLFWwindow *window, bool to_front_buffer) {
    uint32_t *pixels = (uint32_t *)(to_front_buffer ? decs.titlebar.buffer.data.front : decs.titlebar.buffer.data.back);
    const int width = decs.titlebar.buffer.width, height = decs.titlebar.buffer.height;
    if (width <= 0 || height <= 0) return;
    const double scale = decs.for_window_state.fscale;
    const bool maximized = window->wl.current.toplevel_states & TOPLEVEL_STATE_MAXIMIZED;
    const TitlebarColors colors = popover_colors();
    for (int i = 0; i < width * height; i++) pixels[i] = colors.bg;

    // The pop-over carries the window controls and nothing else, so there is
    // no title to draw. It is exactly as wide as the buttons, one square slot
    // each; the buffer is scaled as a whole while a slot is scaled on its own,
    // and round(n * w * scale) can come out below n * round(w * scale), so
    // derive the slot from the width too rather than trusting them to agree.
    PlacedTitlebarButton buttons[TITLEBAR_BUTTON_COUNT];
    int margin;
    const unsigned wanted = popover_button_count(window);
    if (!wanted) return;
    const int slot = MAX(1, MIN(height, width / (int)wanted));
    const unsigned num_buttons = layout_titlebar_buttons(window, width, slot, 0, buttons, &margin);

    decs.minimize.width = decs.maximize.width = decs.close.width = 0;
    for (unsigned i = 0; i < num_buttons; i++) {
        __typeof__(decs.close) *state = buttons[i].which == TITLEBAR_BUTTON_CLOSE      ? &decs.close
                                        : buttons[i].which == TITLEBAR_BUTTON_MAXIMIZE ? &decs.maximize
                                                                                       : &decs.minimize;
        state->left = buttons[i].left;
        state->width = slot;
        uint32_t fill = colors.button;
        if (state->hovered) fill = decs.pressed_button == (int)buttons[i].which ? colors.pressed_button : colors.hovered_button;
        // The geometry of the buttons after this one is not set yet, but
        // buttons do not overlap, so only this button can contain the touch
        if (decoration_touch.active && decoration_touch.window_id == window->id && decoration_touch.button == (int)buttons[i].which &&
            titlebar_button_at(window, decoration_touch.x, decoration_touch.y) == buttons[i].which)
            fill = colors.pressed_button;
        render_titlebar_button(pixels, width, height, buttons[i].left, slot, scale, buttons[i].which, maximized, fill, colors.fg, colors.bg);
    }
    if (!decs.close.width) decs.close.hovered = false;
    if (!decs.maximize.width) decs.maximize.hovered = false;
    if (!decs.minimize.width) decs.minimize.hovered = false;
    // Only round when the window itself is rounded: maximized and fullscreen
    // windows square off their corners, matching kitty_chrome_frame().
    if (!(window->wl.current.toplevel_states & (TOPLEVEL_STATE_MAXIMIZED | TOPLEVEL_STATE_FULLSCREEN)))
        round_top_right_corner(pixels, width, height, WINDOW_CORNER_RADIUS * scale);
}

// Wayland shm buffers are premultiplied, so fading means scaling the color
// along with the alpha.
static void
apply_titlebar_opacity(uint8_t *data, size_t size_in_bytes, double opacity) {
    if (opacity >= 1.0) return;
    const uint32_t scale = (uint32_t)(opacity <= 0 ? 0 : opacity * 255.0 + 0.5);
    for (uint32_t *px = (uint32_t *)data, *end = (uint32_t *)(data + size_in_bytes); px < end; px++) {
        const uint32_t a = (((*px >> 24) & 0xff) * scale) / 255, r = (((*px >> 16) & 0xff) * scale) / 255;
        const uint32_t g = (((*px >> 8) & 0xff) * scale) / 255, b = ((*px & 0xff) * scale) / 255;
        *px = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

static bool
update_title_bar(_GLFWwindow *window) {
    // Never render into a buffer the compositor might still be reading from,
    // instead render when it is released.
    _GLFWWaylandBufferPair *pair = &decs.titlebar.buffer;
    if (pair->back == pair->a ? pair->a_busy : pair->b_busy) {
        pair->has_pending_update = true;
        return false;
    }
    pair->has_pending_update = false;
    render_title_bar(window, false);
    apply_titlebar_opacity(pair->data.back, pair->size_in_bytes, decs.titlebar_opacity);
    swap_buffers(pair);
    return true;
}

static void
render_horizontal_shadow(_GLFWwindow *window, ssize_t scaled_shadow_size, ssize_t src_y_offset, ssize_t y, _GLFWWaylandBufferPair *buf) {
    // left region
    ssize_t src_y = src_y_offset + y;
    const ssize_t src_leftover_corner = st.corner_size - scaled_shadow_size;
    uint32_t *src = st.data + st.stride * src_y + scaled_shadow_size;
    uint32_t *d_start = (uint32_t *)(buf->data.front + y * buf->stride);
    uint32_t *d_end = (uint32_t *)(buf->data.front + (y + 1) * buf->stride);
    uint32_t *left_region_end = d_start + MIN(d_end - d_start, src_leftover_corner);
    memcpy(d_start, src, sizeof(uint32_t) * (left_region_end - d_start));
    // right region
    uint32_t *right_region_start = MAX(d_start, d_end - src_leftover_corner);
    src = st.data + st.stride * (src_y + 1) - st.corner_size;
    memcpy(right_region_start, src, sizeof(uint32_t) * MIN(src_leftover_corner, d_end - right_region_start));
    src = st.data + st.stride * src_y + st.corner_size;
    // middle region
    for (uint32_t *d = left_region_end; d < right_region_start; d += scaled_shadow_size)
        memcpy(d, src, sizeof(uint32_t) * MIN(right_region_start - d, scaled_shadow_size));
}

static void
copy_vertical_region(
    _GLFWwindow *window, ssize_t src_y_start, ssize_t src_y_limit, ssize_t y_start, ssize_t y_limit, ssize_t src_x_offset, _GLFWWaylandBufferPair *buf) {
    for (ssize_t dy = y_start, sy = src_y_start; dy < y_limit && sy < src_y_limit; dy++, sy++)
        memcpy(buf->data.front + dy * buf->stride, st.data + sy * st.stride + src_x_offset, sizeof(uint32_t) * buf->width);
}

static void
render_shadows(_GLFWwindow *window) {
    if (!window_needs_shadows(window)) return;
    const ssize_t scaled_shadow_size = create_shadow_tile(window);
    if (!st.data || !scaled_shadow_size) return; // out of memory
    // upper and lower shadows
    for (ssize_t y = 0; y < scaled_shadow_size; y++) {
        _GLFWWaylandBufferPair *buf = &decs.shadow_upper_left.buffer;
        uint32_t *src = st.data + st.stride * y;
        uint32_t *d = (uint32_t *)(buf->data.front + y * buf->stride);
        memcpy(d, src, sizeof(uint32_t) * scaled_shadow_size);

        buf = &decs.shadow_upper_right.buffer;
        src += st.stride - scaled_shadow_size;
        d = (uint32_t *)(buf->data.front + y * buf->stride);
        memcpy(d, src, sizeof(uint32_t) * scaled_shadow_size);

        const size_t tile_bottom_start = st.stride - scaled_shadow_size;
        buf = &decs.shadow_lower_left.buffer;
        src = st.data + (tile_bottom_start + y) * st.stride;
        d = (uint32_t *)(buf->data.front + y * buf->stride);
        memcpy(d, src, sizeof(uint32_t) * scaled_shadow_size);

        buf = &decs.shadow_lower_right.buffer;
        src += st.stride - scaled_shadow_size;
        d = (uint32_t *)(buf->data.front + y * buf->stride);
        memcpy(d, src, sizeof(uint32_t) * scaled_shadow_size);

        render_horizontal_shadow(window, scaled_shadow_size, 0, y, &decs.shadow_top.buffer);
        render_horizontal_shadow(window, scaled_shadow_size, st.stride - scaled_shadow_size, y, &decs.shadow_bottom.buffer);
    }
    // side shadows
    // top region
    const ssize_t src_leftover_corner = st.corner_size - scaled_shadow_size;
    ssize_t y_start = 0, y_end = decs.shadow_left.buffer.height, top_end = MIN(y_end, src_leftover_corner);
    ssize_t right_src_start = st.stride - scaled_shadow_size;
#define c(src_y_start, src_y_limit, dest_y_start, dest_y_limit)                                                                         \
    {                                                                                                                                   \
        copy_vertical_region(window, src_y_start, src_y_limit, dest_y_start, dest_y_limit, 0, &decs.shadow_left.buffer);                \
        copy_vertical_region(window, src_y_start, src_y_limit, dest_y_start, dest_y_limit, right_src_start, &decs.shadow_right.buffer); \
    }
    c(scaled_shadow_size, st.corner_size, y_start, top_end);
    // bottom region
    ssize_t bottom_start = MAX(0, y_end - src_leftover_corner);
    c(st.stride - st.corner_size, st.stride - scaled_shadow_size, bottom_start, y_end);
    // middle region
    for (ssize_t dest_y = top_end; dest_y < bottom_start; dest_y += scaled_shadow_size)
        c(st.corner_size, st.corner_size + scaled_shadow_size, dest_y, MIN(dest_y + scaled_shadow_size, bottom_start));
#undef c

#define copy(which)                                                                                                 \
    for (uint32_t *src = (uint32_t *)decs.which.buffer.data.front, *dest = (uint32_t *)decs.which.buffer.data.back; \
         src < (uint32_t *)(decs.which.buffer.data.front + decs.which.buffer.size_in_bytes);                        \
         src++, dest++)                                                                                             \
        *dest = (A(*src) / 2) << 24;
    all_shadow_surfaces(copy);
#undef copy
}
#undef st

// The titlebar holds nothing but the window controls, so it is exactly as
// wide as the buttons it draws and is mapped only while revealed.

static bool
window_has_popover(_GLFWwindow *window) {
    return !decs.titlebar_hidden && popover_width(window) > 0;
}

static bool
create_shm_buffers(_GLFWwindow *window) {
    decs.mapping.size = 0;
    const bool has_titlebar = window_has_popover(window);
    // The pop-over overlays the content, so the sides span the content alone.
    const int side_height = window->wl.height;
#define bp(which, width, height) decs.mapping.size += init_buffer_pair(&decs.which.buffer, width, height, decs.for_window_state.fscale);
    if (has_titlebar) bp(titlebar, popover_width(window), decs.metrics.visible_titlebar_height);
    bp(shadow_top, window->wl.width, decs.metrics.width);
    bp(shadow_bottom, window->wl.width, decs.metrics.width);
    bp(shadow_left, decs.metrics.width, side_height);
    bp(shadow_right, decs.metrics.width, side_height);
    bp(shadow_upper_left, decs.metrics.width, decs.metrics.width);
    bp(shadow_upper_right, decs.metrics.width, decs.metrics.width);
    bp(shadow_lower_left, decs.metrics.width, decs.metrics.width);
    bp(shadow_lower_right, decs.metrics.width, decs.metrics.width);
#undef bp

    int fd = createAnonymousFile(decs.mapping.size);
    if (fd < 0) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: Creating a buffer file for %zu B failed: %s", decs.mapping.size, strerror(errno));
        return false;
    }
    decs.mapping.data = mmap(NULL, decs.mapping.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (decs.mapping.data == MAP_FAILED) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: mmap failed: %s", strerror(errno));
        close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(_glfw.wl.shm, fd, decs.mapping.size);
    close(fd);
    size_t offset = 0;
#define Q(which) alloc_buffer_pair(window->id, &decs.which.buffer, pool, decs.mapping.data, &offset)
    if (has_titlebar) Q(titlebar);
    all_shadow_surfaces(Q);
#undef Q
    wl_shm_pool_destroy(pool);
    if (has_titlebar) {
        render_title_bar(window, true);
        apply_titlebar_opacity(decs.titlebar.buffer.data.front, decs.titlebar.buffer.size_in_bytes, decs.titlebar_opacity);
    }
    render_shadows(window);
    debug("Created decoration buffers at scale: %f\n", decs.for_window_state.fscale);
    return true;
}

static void
free_csd_surface(_GLFWWaylandCSDSurface *s) {
    if (decoration_touch.active && decoration_touch.surface == s->surface) memset(&decoration_touch, 0, sizeof(decoration_touch));
    if (s->subsurface) wl_subsurface_destroy(s->subsurface);
    s->subsurface = NULL;
    if (s->wp_viewport) wp_viewport_destroy(s->wp_viewport);
    s->wp_viewport = NULL;
    if (s->surface) wl_surface_destroy(s->surface);
    s->surface = NULL;
}

static void
free_csd_surfaces(_GLFWwindow *window) {
#define Q(which) free_csd_surface(&decs.which)
    all_surfaces(Q);
#undef Q
}

static void
free_csd_buffers(_GLFWwindow *window) {
#define Q(which)                                                                                      \
    {                                                                                                 \
        /* Busy buffers are destroyed when released, as destroying them now  */                       \
        /* causes flicker. Mutter never releases buffers that are not busy. */                        \
        if (decs.which.buffer.a && !decs.which.buffer.a_busy) wl_buffer_destroy(decs.which.buffer.a); \
        if (decs.which.buffer.b && !decs.which.buffer.b_busy) wl_buffer_destroy(decs.which.buffer.b); \
        memset(&decs.which.buffer, 0, sizeof(_GLFWWaylandBufferPair));                                \
    }
    all_surfaces(Q);
#undef Q
    if (decs.mapping.data) munmap(decs.mapping.data, decs.mapping.size);
    decs.mapping.data = NULL;
    decs.mapping.size = 0;
}

static void
position_csd_surface(_GLFWWaylandCSDSurface *s, int x, int y) {
    if (s->surface) {
        wl_surface_set_buffer_scale(s->surface, 1);
        s->x = x;
        s->y = y;
        wl_subsurface_set_position(s->subsurface, s->x, s->y);
    }
}

static void
create_csd_surfaces(_GLFWwindow *window, _GLFWWaylandCSDSurface *s) {
    if (s->surface) wl_surface_destroy(s->surface);
    s->surface = wl_compositor_create_surface(_glfw.wl.compositor);
    wl_surface_set_user_data(s->surface, window);
    if (s->subsurface) wl_subsurface_destroy(s->subsurface);
    s->subsurface = wl_subcompositor_get_subsurface(_glfw.wl.subcompositor, s->surface, window->wl.surface);
    if (_glfw.wl.wp_viewporter) {
        if (s->wp_viewport) wp_viewport_destroy(s->wp_viewport);
        s->wp_viewport = wp_viewporter_get_viewport(_glfw.wl.wp_viewporter, s->surface);
    }
}

#define damage_csd(which, xbuffer)                                                                                                                            \
    if (decs.which.surface) {                                                                                                                                 \
        wl_surface_attach(decs.which.surface, (xbuffer), 0, 0);                                                                                               \
        if (decs.which.wp_viewport) wp_viewport_set_destination(decs.which.wp_viewport, decs.which.buffer.viewport_width, decs.which.buffer.viewport_height); \
        wl_surface_damage(decs.which.surface, 0, 0, decs.which.buffer.width, decs.which.buffer.height);                                                       \
        wl_surface_commit(decs.which.surface);                                                                                                                \
        if (decs.which.buffer.a == (xbuffer)) {                                                                                                               \
            decs.which.buffer.a_busy = true;                                                                                                                  \
        } else {                                                                                                                                              \
            decs.which.buffer.b_busy = true;                                                                                                                  \
        }                                                                                                                                                     \
    }

static bool
window_is_csd_capable(_GLFWwindow *window) {
    return window->decorated && !decs.serverSide && window->wl.xdg.toplevel;
}

bool
csd_should_window_be_decorated(_GLFWwindow *window) {
    return window_is_csd_capable(window) && window->monitor == NULL && (window->wl.current.toplevel_states & TOPLEVEL_STATE_FULLSCREEN) == 0;
}

static bool
ensure_csd_resources(_GLFWwindow *window) {
    // Never create decorations for windows that should not have them, such
    // as fullscreen windows, even if the stored state is stale
    if (!csd_should_window_be_decorated(window)) return false;
    const bool has_titlebar = window_has_popover(window);
    const bool is_focused = window->id == _glfw.focusedWindowId;
    const bool focus_changed = is_focused != decs.for_window_state.focused;
    const double current_scale = _glfwWaylandWindowScale(window);
    const bool size_changed =
        (decs.for_window_state.width != window->wl.width || decs.for_window_state.height != window->wl.height ||
         decs.for_window_state.fscale != current_scale || !decs.mapping.data ||
         (has_titlebar && decs.titlebar.buffer.viewport_height != decs.metrics.visible_titlebar_height));
    // The activated state is ignored as focus is tracked separately via
    // keyboard focus, otherwise every focus change would render the
    // decorations twice, once for keyboard focus and once for the configure
    const uint32_t ignored_states = TOPLEVEL_STATE_ACTIVATED;
    const bool state_changed = (decs.for_window_state.toplevel_states & ~ignored_states) != (window->wl.current.toplevel_states & ~ignored_states);
    const bool titlebar_state_changed = (has_titlebar && !decs.titlebar.surface) || (!has_titlebar && decs.titlebar.surface);
    // The surfaces are destroyed when the decorations are hidden, for example,
    // when going fullscreen, without the state above changing
    const bool surfaces_missing = !decs.shadow_top.surface;
    const bool needs_update = focus_changed || size_changed || titlebar_state_changed || state_changed || surfaces_missing;
    debug(
        "CSD: old.size: %dx%d new.size: %dx%d needs_update: %d size_changed: %d state_changed: %d\n",
        decs.for_window_state.width,
        decs.for_window_state.height,
        window->wl.width,
        window->wl.height,
        needs_update,
        size_changed,
        state_changed);
    if (!needs_update) return false;
    decs.for_window_state.fscale = current_scale; // used in create_shm_buffers
    if (size_changed || titlebar_state_changed) {
        if (titlebar_state_changed) {
            free(decs.shadow_tile.data);
            decs.shadow_tile.data = NULL;
        }
        free_csd_buffers(window);
        if (!create_shm_buffers(window)) return false;
    }

    const int top_y = 0; // the pop-over overlays the content, it reserves nothing

#define setup_surface(which, x, y)                                     \
    if (!decs.which.surface) create_csd_surfaces(window, &decs.which); \
    position_csd_surface(&decs.which, x, y);

    if (has_titlebar) {
        setup_surface(titlebar, window->wl.width - (int)popover_width(window), 0);
    } else {
        free_csd_surface(&decs.titlebar);
        if (decs.focus == CSD_titlebar) {
            decs.focus = CENTRAL_WINDOW;
            decs.dragging = false;
        }
    }
    setup_surface(shadow_top, 0, top_y - decs.metrics.width);
    setup_surface(shadow_bottom, 0, window->wl.height);
    setup_surface(shadow_left, -decs.metrics.width, top_y);
    setup_surface(shadow_right, window->wl.width, decs.shadow_left.y);
    setup_surface(shadow_upper_left, decs.shadow_left.x, decs.shadow_top.y);
    setup_surface(shadow_upper_right, decs.shadow_right.x, decs.shadow_top.y);
    setup_surface(shadow_lower_left, decs.shadow_left.x, decs.shadow_bottom.y);
    setup_surface(shadow_lower_right, decs.shadow_right.x, decs.shadow_bottom.y);

    if (has_titlebar) {
        if (focus_changed || state_changed) update_title_bar(window);
        if (decs.titlebar_opacity > 0) {
            damage_csd(titlebar, decs.titlebar.buffer.front);
        } else if (decs.titlebar.surface) {
            wl_surface_attach(decs.titlebar.surface, NULL, 0, 0);
            wl_surface_commit(decs.titlebar.surface);
        }
    }
#define d(which) damage_csd(which, is_focused ? decs.which.buffer.front : decs.which.buffer.back);
    d(shadow_left);
    d(shadow_right);
    d(shadow_top);
    d(shadow_bottom);
    d(shadow_upper_left);
    d(shadow_upper_right);
    d(shadow_lower_left);
    d(shadow_lower_right);
#undef d

    decs.for_window_state.width = window->wl.width;
    decs.for_window_state.height = window->wl.height;
    decs.for_window_state.focused = is_focused;
    decs.for_window_state.toplevel_states = window->wl.current.toplevel_states;
    return true;
}

void
csd_set_visible(_GLFWwindow *window, bool visible) {
    // When setting to visible will only take effect if window currently has
    // CSD and will also ensure CSD is of correct size and type for current window.
    // When hiding CSD simply destroys all CSD surfaces.
    if (visible) ensure_csd_resources(window);
    else free_csd_surfaces(window);
}

// A fade rather than a pop: the controls appear and disappear on pointer
// proximity, and snapping them in and out at full opacity reads as a glitch.
#define TITLEBAR_FADE_MS 180.0
#define TITLEBAR_FADE_INTERVAL_MS 16

static void
titlebar_fade_tick(id_type timer_id, void *data) {
    _GLFWwindow *window = data;
    const double target = decs.titlebar_revealed ? 1.0 : 0.0;
    const double step = TITLEBAR_FADE_INTERVAL_MS / TITLEBAR_FADE_MS;
    if (decs.titlebar_opacity < target) decs.titlebar_opacity = MIN(target, decs.titlebar_opacity + step);
    else decs.titlebar_opacity = MAX(target, decs.titlebar_opacity - step);

    if (decs.titlebar.surface) {
        if (decs.titlebar_opacity > 0) {
            // csd_change_title() repaints into the back buffer, or, while the
            // compositor still holds it, defers to its release.
            csd_change_title(window);
        } else {
            wl_surface_attach(decs.titlebar.surface, NULL, 0, 0);
            wl_surface_commit(decs.titlebar.surface);
        }
    }
    commit_window_surface_if_safe(window);
    if (decs.titlebar_opacity == target) toggleTimer(&_glfw.wl.eventLoopData, timer_id, 0);
}

static void
start_titlebar_fade(_GLFWwindow *window) {
    if (!decs.titlebar_fade_timer)
        decs.titlebar_fade_timer = addTimer(
            &_glfw.wl.eventLoopData, "kitty-chrome-popover-fade", ms_to_monotonic_t(TITLEBAR_FADE_INTERVAL_MS), 1, true, titlebar_fade_tick, window, NULL);
    else toggleTimer(&_glfw.wl.eventLoopData, decs.titlebar_fade_timer, 1);
}

// Revealing maps the pop-over and hiding unmaps it, rather than creating and
// destroying the surface: a sweep of the pointer across the corner would
// otherwise tear down and rebuild the shm pool and every decoration buffer on
// each crossing. Unmapping also takes the pointer off it, so leaving the window
// directly from the pop-over hides it like any other exit.
bool
csd_set_titlebar_revealed(_GLFWwindow *window, bool revealed) {
    if (decs.titlebar_revealed == revealed) return false;
    decs.titlebar_revealed = revealed;
    if (!window_is_csd_capable(window)) return false;
    if (revealed) ensure_csd_resources(window);
    else if (decs.focus == CSD_titlebar) {
        decs.focus = CENTRAL_WINDOW;
        decs.dragging = false;
    }
    start_titlebar_fade(window);
    return true;
}

// The pop-over lives in the top right of the content. Reveal it while the
// pointer is in that corner, and keep it revealed while the pointer is over the
// pop-over itself, which the compositor reports as a leave of the main surface.
bool
csd_update_titlebar_reveal(_GLFWwindow *window, double x, double y) {
    if (decs.titlebar_hidden || !window_is_csd_capable(window)) return false;
    const double slop = 8.0;
    const double zone_width = popover_width(window) + slop;
    const double zone_height = decs.metrics.visible_titlebar_height + slop;
    const bool inside = x >= window->wl.width - zone_width && y <= zone_height;
    return csd_set_titlebar_revealed(window, inside || decs.focus == CSD_titlebar);
}

// The top edge drags the window. Above the surface that is the shadow strip,
// but the painted frame inside our own surface is part of the same edge, and
// without this the cursor flips between grab and the terminal's own cursor as
// the pointer crosses between them. Kept to the painted frame's thickness so it
// does not eat clicks on the first line of the terminal.
#define TOP_DRAG_BAND 4.0
bool
csd_top_edge_contains(_GLFWwindow *window, double x, double y) {
    if (decs.titlebar_hidden || !window_is_csd_capable(window) || !window->wl.xdg.toplevel) return false;
    if (window->wl.current.toplevel_states & (TOPLEVEL_STATE_FULLSCREEN | TOPLEVEL_STATE_MAXIMIZED)) return false;
    if (y < 0 || y > TOP_DRAG_BAND) return false;
    return x >= 0 && x < (double)window->wl.width - (double)popover_width(window);
}

void
csd_top_edge_set_cursor(_GLFWwindow *window) {
    if (_glfw.wl.cursorPreviousShape != GLFW_GRAB_CURSOR) _glfwWaylandSetCursorShape(GLFW_GRAB_CURSOR, window);
}

bool
csd_top_edge_start_move(_GLFWwindow *window, uint32_t button, uint32_t state) {
    if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED) return false;
    xdg_toplevel_move(window->wl.xdg.toplevel, _glfw.wl.seat, _glfw.wl.pointer_serial);
    return true;
}

// The compositor reports the pointer moving onto the pop-over as leaving the
// main surface, and the matching enter has not arrived yet, so only hide when
// the pointer was heading somewhere other than the pop-over itself.
bool
csd_handle_pointer_left_window(_GLFWwindow *window, double x, double y) {
    if (!decs.titlebar_revealed) return false;
    const double slop = 2.0;
    const double left = (double)window->wl.width - (double)popover_width(window) - slop;
    const double bottom = (double)decs.metrics.visible_titlebar_height + slop;
    if (x >= left && y <= bottom) return false;
    return csd_set_titlebar_revealed(window, false);
}

void
csd_free_all_resources(_GLFWwindow *window) {
    if (decs.titlebar_fade_timer) {
        removeTimer(&_glfw.wl.eventLoopData, decs.titlebar_fade_timer);
        decs.titlebar_fade_timer = 0;
    }
    free_csd_surfaces(window);
    free_csd_buffers(window);
    if (decs.shadow_tile.data) free(decs.shadow_tile.data);
    decs.shadow_tile.data = NULL;
}

bool
csd_change_title(_GLFWwindow *window) {
    if (!window_is_csd_capable(window)) return false;
    if (ensure_csd_resources(window)) return true; // CSD were re-rendered for other reasons
    if (decs.titlebar.surface) {
        update_title_bar(window);
        damage_csd(titlebar, decs.titlebar.buffer.front);
        return true;
    }
    return false;
}

bool
csd_handle_focus_change(_GLFWwindow *window) {
    // Keyboard focus changes are not necessarily accompanied by a configure
    // event, which is what usually causes the decorations to be re-rendered.
    // Returns true if the window surface needs to be committed.
    return ensure_csd_resources(window);
}

static void
on_titlebar_buffer_released(_GLFWwindow *window) {
    if (!decs.titlebar.buffer.has_pending_update || !decs.titlebar.surface || !window_is_csd_capable(window)) return;
    if (update_title_bar(window)) {
        damage_csd(titlebar, decs.titlebar.buffer.front);
        if (!window->wl.waiting_for_swap_to_commit) wl_surface_commit(window->wl.surface);
    }
}

void
csd_set_window_geometry(_GLFWwindow *window, int32_t *width, int32_t *height) {
    bool size_specified_by_compositor = *width > 0 && *height > 0;
    if (!size_specified_by_compositor) {
        *width = window->wl.user_requested_content_size.width;
        *height = window->wl.user_requested_content_size.height;
        if (window->wl.xdg.top_level_bounds.width > 0) *width = MIN(*width, window->wl.xdg.top_level_bounds.width);
        if (window->wl.xdg.top_level_bounds.height > 0) *height = MIN(*height, window->wl.xdg.top_level_bounds.height);
    }
    decs.geometry.x = 0;
    decs.geometry.y = 0;
    decs.geometry.width = *width;
    decs.geometry.height = *height;
}

bool
csd_set_titlebar_color(_GLFWwindow *window, uint32_t color, bool use_system_color) {
    bool use_custom_color = !use_system_color;
    decs.use_custom_titlebar_color = use_custom_color;
    decs.titlebar_color = color;
    return csd_change_title(window);
}

#define x window->wl.allCursorPosX
#define y window->wl.allCursorPosY

void
_glfwWaylandSetCursorShape(GLFWCursorShape shape, _GLFWwindow *window) {
    if (_glfw.wl.wp_cursor_shape_device_v1) {
        wayland_cursor_shape s = glfw_cursor_shape_to_wayland_cursor_shape(shape);
        if (s.which > -1) {
            debug("Changing cursor shape to: %s with serial: %u\n", s.name, _glfw.wl.pointer_enter_serial);
            wp_cursor_shape_device_v1_set_shape(_glfw.wl.wp_cursor_shape_device_v1, _glfw.wl.pointer_enter_serial, (uint32_t)s.which);
            return;
        }
    }

    struct wl_buffer *buffer;
    struct wl_cursor *cursor;
    struct wl_cursor_image *image;
    struct wl_surface *surface = _glfw.wl.cursorSurface;
    const int scale = _glfwWaylandIntegerWindowScale(window);

    struct wl_cursor_theme *theme = glfw_wlc_theme_for_scale(scale);
    if (!theme) return;
    cursor = _glfwLoadCursor(shape, theme);
    if (!cursor || !cursor->images) return;
    image = cursor->images[0];
    if (!image) return;
    if (image->width % scale || image->height % scale) {
        static uint32_t warned_width = 0, warned_height = 0;
        if (warned_width != image->width || warned_height != image->height) {
            _glfwInputError(
                GLFW_PLATFORM_ERROR,
                "WARNING: Cursor image size: %dx%d is not a multiple of window scale: %d. This will"
                " cause some compositors such as GNOME to crash. See https://github.com/kovidgoyal/kitty/issues/4878",
                image->width,
                image->height,
                scale);
            warned_width = image->width;
            warned_height = image->height;
        }
    }

    buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;
    debug("Calling wl_pointer_set_cursor in set_cursor with surface: %p\n", (void *)surface);
    wl_pointer_set_cursor(_glfw.wl.pointer, _glfw.wl.serial, surface, image->hotspot_x / scale, image->hotspot_y / scale);
    wl_surface_set_buffer_scale(surface, scale);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, image->width, image->height);
    wl_surface_commit(surface);
    _glfw.wl.cursorPreviousShape = shape;
}


static bool
update_hovered_button(_GLFWwindow *window) {
    const GLFWTitlebarButton hovered = titlebar_button_at_x(window, x);
#define c(which, button)                       \
    if (hovered == button) {                   \
        if (!decs.which.hovered) {             \
            decs.titlebar_needs_update = true; \
            decs.which.hovered = true;         \
        }                                      \
    } else if (decs.which.hovered) {           \
        decs.titlebar_needs_update = true;     \
        decs.which.hovered = false;            \
    }

    c(minimize, TITLEBAR_BUTTON_MINIMIZE);
    c(maximize, TITLEBAR_BUTTON_MAXIMIZE);
    c(close, TITLEBAR_BUTTON_CLOSE);
#undef c
    return hovered != TITLEBAR_BUTTON_NONE;
}

static bool
has_hovered_button(_GLFWwindow *window) {
    return decs.minimize.hovered || decs.maximize.hovered || decs.close.hovered;
}

static void
handle_pointer_leave(_GLFWwindow *window, struct wl_surface *surface) {
#define c(which)                           \
    if (decs.which.hovered) {              \
        decs.titlebar_needs_update = true; \
        decs.which.hovered = false;        \
    }
    if (surface == decs.titlebar.surface) {
        c(minimize);
        c(maximize);
        c(close);
        // Hide whenever the pointer leaves the pop-over. Where it went cannot be
        // known here, and a fast flick out of the window does not leave a motion
        // event near the edge it crossed, so guessing from the last position
        // misses exactly the case that matters. If it landed back on the content
        // the enter there re-reveals it, which is now just an attach away. The
        // subsurfaces are synchronized, so this needs a commit on the parent.
        if (csd_set_titlebar_revealed(window, false)) commit_window_surface_if_safe(window);
    }
#undef c
    decs.focus = CENTRAL_WINDOW;
    decs.pressed_button = TITLEBAR_BUTTON_NONE;
    decs.dragging = false;
}


static void
handle_pointer_move(_GLFWwindow *window) {
    GLFWCursorShape cursorShape = GLFW_DEFAULT_CURSOR;
    switch (decs.focus) {
        case CENTRAL_WINDOW: break;
        case CSD_titlebar: {
            if (decs.dragging) {
                if (window->wl.xdg.toplevel) xdg_toplevel_move(window->wl.xdg.toplevel, _glfw.wl.seat, _glfw.wl.pointer_serial);
            } else if (update_hovered_button(window)) cursorShape = GLFW_POINTER_CURSOR;
        } break;
        case CSD_shadow_top:
            // The top edge drags the window rather than resizing it, so it
            // shows a grab cursor rather than a resize arrow.
            cursorShape = decs.dragging ? GLFW_GRABBING_CURSOR : GLFW_GRAB_CURSOR;
            if (decs.dragging && window->wl.xdg.toplevel) xdg_toplevel_move(window->wl.xdg.toplevel, _glfw.wl.seat, _glfw.wl.pointer_serial);
            break;
        case CSD_shadow_bottom: cursorShape = GLFW_S_RESIZE_CURSOR; break;
        case CSD_shadow_left: cursorShape = GLFW_W_RESIZE_CURSOR; break;
        case CSD_shadow_right: cursorShape = GLFW_E_RESIZE_CURSOR; break;
        case CSD_shadow_upper_left: cursorShape = GLFW_NW_RESIZE_CURSOR; break;
        // The top right corner sits under the window controls, so it neither
        // resizes nor drags: doing either there fights with the buttons.
        case CSD_shadow_upper_right: break;
        case CSD_shadow_lower_left: cursorShape = GLFW_SW_RESIZE_CURSOR; break;
        case CSD_shadow_lower_right: cursorShape = GLFW_SE_RESIZE_CURSOR; break;
    }
    if (_glfw.wl.cursorPreviousShape != cursorShape) _glfwWaylandSetCursorShape(cursorShape, window);
}

static void
handle_pointer_enter(_GLFWwindow *window, struct wl_surface *surface) {
    decs.focus = decoration_surface(window, surface);
    if (decs.focus != CENTRAL_WINDOW) handle_pointer_move(window);
    else decs.dragging = false;
}

static void
perform_titlebar_action(_GLFWwindow *window, GLFWTitlebarAction action) {
    switch (action) {
        case TITLEBAR_ACTION_MENU:
            if (!window->wl.xdg.toplevel) break;
            if (window->wl.wm_capabilities.window_menu)
                // x and y are relative to the pop-over, which sits at the top
                // right of the content, and the window geometry starts at 0,0.
                xdg_toplevel_show_window_menu(
                    window->wl.xdg.toplevel, _glfw.wl.seat, _glfw.wl.pointer_serial, (int32_t)(window->wl.width - (int)popover_width(window) + x), (int32_t)y);
            else _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland compositor does not support showing window menu");
            break;
        case TITLEBAR_ACTION_MAXIMIZE:
            if (window->wl.wm_capabilities.maximize) {
                if (window->wl.current.toplevel_states & TOPLEVEL_STATE_MAXIMIZED) _glfwPlatformRestoreWindow(window);
                else _glfwPlatformMaximizeWindow(window);
            }
            break;
        case TITLEBAR_ACTION_MINIMIZE:
            if (window->wl.wm_capabilities.minimize) _glfwPlatformIconifyWindow(window);
            break;
        case TITLEBAR_ACTION_NONE: break;
    }
}

static void
handle_pointer_button(_GLFWwindow *window, uint32_t button, uint32_t state) {
    uint32_t edges = XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    if (button == BTN_LEFT) {
        switch (decs.focus) {
            case CENTRAL_WINDOW: break;
            case CSD_titlebar:
                if (has_hovered_button(window) || decs.pressed_button) {
                    const GLFWTitlebarButton hovered = titlebar_button_at_x(window, x);
                    decs.dragging = false;
                    decs.titlebar_needs_update = true;
                    decs.last_click_on_top_decoration_at = 0;
                    if (state == WL_POINTER_BUTTON_STATE_PRESSED) decs.pressed_button = hovered;
                    else {
                        const GLFWTitlebarButton pressed = decs.pressed_button;
                        decs.pressed_button = TITLEBAR_BUTTON_NONE;
                        if (pressed == hovered) activate_titlebar_button(window, pressed);
                    }
                    break;
                }
                if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
                    monotonic_t last_click_at = decs.last_click_on_top_decoration_at;
                    decs.last_click_on_top_decoration_at = monotonic();
                    if (decs.last_click_on_top_decoration_at - last_click_at <= _glfwPlatformGetDoubleClickInterval(window)) {
                        decs.last_click_on_top_decoration_at = 0;
                        perform_titlebar_action(window, glfw_current_titlebar_action(TITLEBAR_GESTURE_DOUBLE_CLICK));
                        return;
                    }
                }
                decs.dragging = !has_hovered_button(window);
                break;
            // The top right corner sits under the window controls, so it
            // neither resizes nor drags: doing either there fights with them.
            case CSD_shadow_upper_right: break;
            case CSD_shadow_top:
                if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
                    decs.dragging = true;
                    if (window->wl.xdg.toplevel) xdg_toplevel_move(window->wl.xdg.toplevel, _glfw.wl.seat, _glfw.wl.pointer_serial);
                } else decs.dragging = false;
                break;
            default: edges = decoration_resize_edges(decs.focus); break;
        }
        if (edges != XDG_TOPLEVEL_RESIZE_EDGE_NONE) xdg_toplevel_resize(window->wl.xdg.toplevel, _glfw.wl.seat, _glfw.wl.pointer_serial, edges);
    } else if ((button == BTN_RIGHT || button == BTN_MIDDLE) && state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (decs.focus == CSD_titlebar)
            perform_titlebar_action(window, glfw_current_titlebar_action(button == BTN_MIDDLE ? TITLEBAR_GESTURE_MIDDLE_CLICK : TITLEBAR_GESTURE_RIGHT_CLICK));
    }
}


static void
commit_titlebar_update(_GLFWwindow *window) {
    csd_change_title(window);
    if (!window->wl.waiting_for_swap_to_commit) wl_surface_commit(window->wl.surface);
}

void
csd_handle_pointer_event(_GLFWwindow *window, int button, int state, struct wl_surface *surface) {
    if (!window_is_csd_capable(window)) return;
    decs.titlebar_needs_update = false;
    switch (button) {
        case -1: handle_pointer_move(window); break;
        case -2: handle_pointer_enter(window, surface); break;
        case -3: handle_pointer_leave(window, surface); break;
        default: handle_pointer_button(window, button, state); break;
    }
    if (decs.titlebar_needs_update) commit_titlebar_update(window);
}
#undef x
#undef y

// Called for every touch down, wherever it lands. A contact id is only reused
// after its previous sequence ended, so a tracked contact with this id is
// stale, its up was never delivered.
void
csd_drop_stale_touch(int32_t id) {
    if (decoration_touch.active && decoration_touch.id == id) csd_handle_touch_cancel();
}

bool
csd_is_tracking_touch(int32_t id) {
    return decoration_touch.active && decoration_touch.id == id;
}

bool
csd_handle_touch_down(_GLFWwindow *window, struct wl_surface *surface, int32_t id, uint32_t serial, double sx, double sy) {
    if (!csd_should_window_be_decorated(window) || !window->wl.xdg.toplevel) return false;
    const _GLFWCSDSurface which = decoration_surface(window, surface);
    if (which == CENTRAL_WINDOW) return false;
    const GLFWTitlebarButton button = which == CSD_titlebar ? titlebar_button_at(window, sx, sy) : TITLEBAR_BUTTON_NONE;
    if (which == CSD_shadow_upper_right) return true; // under the controls, see handle_pointer_button()
    if (button == TITLEBAR_BUTTON_NONE) {
        // Interactive requests require this contact's original down serial.
        // The contact is not tracked as the compositor takes it over and need
        // not send its up event to us.
        const uint32_t edges = decoration_resize_edges(which);
        if (edges != XDG_TOPLEVEL_RESIZE_EDGE_NONE) xdg_toplevel_resize(window->wl.xdg.toplevel, _glfw.wl.seat, serial, edges);
        else xdg_toplevel_move(window->wl.xdg.toplevel, _glfw.wl.seat, serial);
        return true;
    }
    // Only one titlebar button press is tracked at a time, other button
    // presses are ignored while it is down
    if (decoration_touch.active) return true;
    decoration_touch.active = true;
    decoration_touch.id = id;
    decoration_touch.window_id = window->id;
    decoration_touch.surface = surface;
    decoration_touch.x = sx;
    decoration_touch.y = sy;
    decoration_touch.button = button;
    commit_titlebar_update(window);
    return true;
}

void
csd_handle_touch_motion(int32_t id, double sx, double sy) {
    if (!csd_is_tracking_touch(id)) return;
    _GLFWwindow *window = _glfwWindowForId(decoration_touch.window_id);
    if (!window) {
        memset(&decoration_touch, 0, sizeof(decoration_touch));
        return;
    }
    const GLFWTitlebarButton button = decoration_touch.button;
    const bool was_on_button = titlebar_button_at(window, decoration_touch.x, decoration_touch.y) == button;
    decoration_touch.x = sx;
    decoration_touch.y = sy;
    if (was_on_button != (titlebar_button_at(window, sx, sy) == button)) commit_titlebar_update(window);
}

void
csd_handle_touch_cancel(void) {
    if (!decoration_touch.active) return;
    _GLFWwindow *window = _glfwWindowForId(decoration_touch.window_id);
    memset(&decoration_touch, 0, sizeof(decoration_touch));
    if (window) commit_titlebar_update(window);
}

void
csd_handle_touch_up(int32_t id) {
    if (!csd_is_tracking_touch(id)) return;
    _GLFWwindow *window = _glfwWindowForId(decoration_touch.window_id);
    const GLFWTitlebarButton pressed = decoration_touch.button;
    const bool activate = window && csd_should_window_be_decorated(window) && pressed == titlebar_button_at(window, decoration_touch.x, decoration_touch.y);
    // The close callback can destroy the window. Clear the contact first.
    csd_handle_touch_cancel();
    if (activate) activate_titlebar_button(window, pressed);
}
