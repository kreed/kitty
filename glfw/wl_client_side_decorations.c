/*
 * wl_client_side_decorations.c
 * Copyright (C) 2021 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "wl_client_side_decorations.h"

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
create_shadow_mask(size_t width, size_t height, size_t margin, size_t kernel_size, kernel_type base_alpha, kernel_type sigma) {
    kernel_type *mask = calloc(2 * width * height + kernel_size, sizeof(kernel_type));
    if (!mask) return NULL;
    for (size_t y = margin; y < height - margin; y++) {
        kernel_type *row = mask + y * width;
        for (size_t x = margin; x < width - margin; x++) row[x] = base_alpha;
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
    st.segments = 7;
    st.stride = st.segments * margin;
    st.corner_size = margin * (st.segments - 1) / 2;
    kernel_type *mask = create_shadow_mask(st.stride, st.stride, margin, 2 * margin + 1, (kernel_type)0.7, 32 * margin);
    free(st.data);
    st.data = malloc(sizeof(st.data[0]) * st.stride * st.stride);
    if (st.data)
        for (size_t i = 0; i < st.stride * st.stride; i++) st.data[i] = ((uint8_t)(mask[i] * 255)) << 24;
    free(mask);
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
    pair->width = (int)round(width * scale);
    pair->height = (int)round(height * scale);
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
window_has_buffer(_GLFWwindow *window, struct wl_buffer *q) {
#define Q(which)                                           \
    if (decs.which.buffer.a == q) {                        \
        decs.which.buffer.a_needs_to_be_destroyed = false; \
        return true;                                       \
    }                                                      \
    if (decs.which.buffer.b == q) {                        \
        decs.which.buffer.b_needs_to_be_destroyed = false; \
        return true;                                       \
    }
    all_surfaces(Q);
#undef Q
    return false;
}

static void
buffer_release_event(void *data, struct wl_buffer *buffer) {
    wl_buffer_destroy(buffer);
    _GLFWwindow *window = _glfwWindowForId((uintptr_t)data);
    if (window && window_has_buffer(window, buffer)) decs.buffer_destroyed = true;
}

static struct wl_buffer_listener handle_buffer_events = {.release = buffer_release_event};

static void
alloc_buffer_pair(uintptr_t window_id, _GLFWWaylandBufferPair *pair, struct wl_shm_pool *pool, uint8_t *data, size_t *offset) {
    pair->data.a = data + *offset;
    pair->a = wl_shm_pool_create_buffer(pool, *offset, pair->width, pair->height, pair->stride, WL_SHM_FORMAT_ARGB8888);
    pair->a_needs_to_be_destroyed = true;
    wl_buffer_add_listener(pair->a, &handle_buffer_events, (void *)window_id);
    *offset += pair->size_in_bytes;
    pair->data.b = data + *offset;
    pair->b = wl_shm_pool_create_buffer(pool, *offset, pair->width, pair->height, pair->stride, WL_SHM_FORMAT_ARGB8888);
    pair->b_needs_to_be_destroyed = true;
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
    decs.metrics.top = 36;
    decs.metrics.visible_titlebar_height = decs.metrics.top - decs.metrics.width;
    decs.metrics.horizontal = 2 * decs.metrics.width;
    decs.metrics.vertical = decs.metrics.width + decs.metrics.top;
}

static void
patch_titlebar_with_alpha_mask(
    uint32_t *dest, uint8_t *src, unsigned height, unsigned dest_stride, unsigned src_width, unsigned dest_left, uint32_t bg, uint32_t fg) {
    for (unsigned y = 0; y < height; y++, src += src_width, dest += dest_stride) {
        uint32_t *d = dest + dest_left;
        for (unsigned i = 0; i < src_width; i++) {
            const uint8_t alpha = src[i], calpha = 255 - alpha;
            // Blend the red and blue components
            uint32_t ans = ((bg & 0xff00ff) * calpha + (fg & 0xff00ff) * alpha) & 0xff00ff00;
            // Blend the green component
            ans += ((bg & 0xff00) * calpha + (fg & 0xff00) * alpha) & 0xff0000;
            ans >>= 8;
            d[i] = ans | 0xff000000;
        }
    }
}

static void
render_hline(uint8_t *out, unsigned width, unsigned thickness, unsigned bottom, unsigned left, unsigned right) {
    for (unsigned y = bottom - thickness; y < bottom; y++) {
        uint8_t *dest = out + width * y;
        for (unsigned x = left; x < right; x++) dest[x] = 255;
    }
}

static void
render_vline(uint8_t *out, unsigned width, unsigned thickness, unsigned left, unsigned top, unsigned bottom) {
    for (unsigned y = top; y < bottom; y++) {
        uint8_t *dest = out + width * y;
        for (unsigned x = left; x < left + thickness; x++) dest[x] = 255;
    }
}

static int
scale(unsigned thickness, float factor) {
    return (unsigned)(roundf(thickness * factor));
}

static void
render_minimize(uint8_t *out, unsigned width, unsigned height) {
    memset(out, 0, (size_t)width * height);
    unsigned thickness = height / 12;
    unsigned baseline = height - thickness * 2;
    unsigned side_margin = scale(thickness, 3.8f);
    if (!thickness || width <= side_margin || height < baseline + 2 * thickness) return;
    render_hline(out, width, thickness, baseline, side_margin, width - side_margin);
}

static void
render_maximize(uint8_t *out, unsigned width, unsigned height) {
    memset(out, 0, (size_t)width * height);
    unsigned thickness = height / 12, half_thickness = thickness / 2;
    unsigned baseline = height - thickness * 2;
    unsigned side_margin = scale(thickness, 3.0f);
    unsigned top = 4 * thickness;
    if (!half_thickness || width <= side_margin || height < baseline + 2 * thickness || top >= baseline) return;
    render_hline(out, width, half_thickness, baseline, side_margin, width - side_margin);
    render_hline(out, width, thickness, top + thickness, side_margin, width - side_margin);
    render_vline(out, width, half_thickness, side_margin, top, baseline);
    render_vline(out, width, half_thickness, width - side_margin, top, baseline);
}

static void
render_restore(uint8_t *out, unsigned width, unsigned height) {
    memset(out, 0, (size_t)width * height);
    unsigned thickness = height / 12, half_thickness = thickness / 2;
    unsigned baseline = height - thickness * 2;
    unsigned side_margin = scale(thickness, 3.0f);
    unsigned top = 4 * thickness;
    if (!half_thickness || width <= side_margin || height < baseline + 2 * thickness || top >= baseline) return;
    unsigned box_height = ((baseline - top) * 3) / 4;
    if (box_height < 2 * thickness) return;
    unsigned box_width = ((width - 2 * side_margin) * 3) / 4;
    // bottom box
    unsigned box_top = baseline - box_height, left = side_margin, right = side_margin + box_width, bottom = baseline;
    render_hline(out, width, thickness, box_top + thickness, left, right);
    render_hline(out, width, half_thickness, bottom, left, right);
    render_vline(out, width, half_thickness, left, box_top, bottom);
    render_vline(out, width, half_thickness, side_margin + box_width, baseline - box_height, baseline);
    // top box
    unsigned box_x_shift = 2 * thickness, box_y_shift = 2 * thickness;
    box_x_shift = MIN(width - right, box_x_shift);
    box_y_shift = MIN(box_top, box_y_shift);
    unsigned left2 = left + box_x_shift, right2 = right + box_x_shift, top2 = box_top - box_y_shift, bottom2 = bottom - box_y_shift;
    render_hline(out, width, thickness, top2 + thickness, left2, right2);
    render_vline(out, width, half_thickness, right2, top2, bottom2);
    render_hline(out, width, half_thickness, bottom2, right, right2);
    render_vline(out, width, half_thickness, left2, top2, box_top);
}

static void
render_line(uint8_t *buf, unsigned width, unsigned height, unsigned thickness, int x1, int y1, int x2, int y2) {
    float m = (y2 - y1) / (float)(x2 - x1);
    float c = y1 - m * x1;
    unsigned delta = thickness / 2, extra = thickness % 2;
    for (int x = MAX(0, MIN(x1, x2)); x < MIN((int)width, MAX(x1, x2) + 1); x++) {
        float ly = m * x + c;
        for (int y = MAX(0, (int)(ly - delta)); y < MIN((int)height, (int)(ly + delta + extra + 1)); y++) buf[x + y * width] = 255;
    }
    for (int y = MAX(0, MIN(y1, y2)); y < MIN((int)height, MAX(y1, y2) + 1); y++) {
        float lx = (y - c) / m;
        for (int x = MAX(0, (int)(lx - delta)); x < MIN((int)width, (int)(lx + delta + extra + 1)); x++) buf[x + y * width] = 255;
    }
}

static void
render_close(uint8_t *out, unsigned width, unsigned height) {
    memset(out, 0, (size_t)width * height);
    unsigned thickness = height / 12;
    unsigned baseline = height - thickness * 2;
    unsigned side_margin = scale(thickness, 3.3f);
    int top = baseline - (width - 2 * side_margin);
    if (top <= 0) return;
    unsigned line_thickness = scale(thickness, 1.5f);
    render_line(out, width, height, line_thickness, side_margin, top, width - side_margin, baseline);
    render_line(out, width, height, line_thickness, side_margin, baseline, width - side_margin, top);
}

static uint32_t
average_intensity_in_src(uint8_t *src, unsigned src_width, unsigned src_x, unsigned src_y, unsigned factor) {
    uint32_t ans = 0;
    for (unsigned y = src_y; y < src_y + factor; y++) {
        uint8_t *s = src + src_width * y;
        for (unsigned x = src_x; x < src_x + factor; x++) ans += s[x];
    }
    return ans / (factor * factor);
}

static void
downsample(uint8_t *dest, uint8_t *src, unsigned dest_width, unsigned dest_height, unsigned factor) {
    unsigned src_width = factor * dest_width;
    for (unsigned y = 0; y < dest_height; y++) {
        uint8_t *d = dest + dest_width * y;
        for (unsigned x = 0; x < dest_width; x++) {
            d[x] = MIN(255u, (uint32_t)d[x] + average_intensity_in_src(src, src_width, x * factor, y * factor, factor));
        }
    }
}

static void
round_top_right_corner(uint32_t *px, size_t width, size_t height, double radius) {
    if (radius <= 0 || width < 2 || height < 2) return;
    const double cx = (double)width - radius, cy = radius;
    for (size_t y = 0; y < height && (double)y < cy; y++) {
        for (size_t x = (size_t)(cx > 0 ? cx : 0); x < width; x++) {
            const double dx = ((double)x + 0.5) - cx, dy = ((double)y + 0.5) - cy;
            if (dx <= 0 || dy >= 0) continue;  // outside the corner quadrant
            double coverage = radius - sqrt(dx * dx + dy * dy) + 0.5;
            coverage = coverage < 0 ? 0 : (coverage > 1 ? 1 : coverage);
            uint32_t *p = px + y * width + x;
            // Wayland shm buffers are premultiplied, so the color goes with the alpha.
            const uint32_t a = (uint32_t)(((*p >> 24) & 0xff) * coverage);
            const uint32_t r = (uint32_t)(((*p >> 16) & 0xff) * coverage);
            const uint32_t g = (uint32_t)(((*p >> 8) & 0xff) * coverage);
            const uint32_t b = (uint32_t)((*p & 0xff) * coverage);
            *p = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void
render_button(
    void (*which)(uint8_t *, unsigned, unsigned),
    bool antialias,
    uint32_t *dest,
    uint8_t *src,
    unsigned height,
    unsigned dest_stride,
    unsigned src_width,
    unsigned dest_left,
    uint32_t bg,
    uint32_t fg) {
    if (antialias) {
        static const unsigned factor = 4;
        uint8_t *big_src = malloc((size_t)factor * factor * height * src_width);
        if (big_src) {
            which(big_src, src_width * factor, height * factor);
            memset(src, 0, (size_t)src_width * height);
            downsample(src, big_src, src_width, height, factor);
            free(big_src);
        } else which(src, src_width, height);
    } else which(src, src_width, height);
    patch_titlebar_with_alpha_mask(dest, src, height, dest_stride, src_width, dest_left, bg, fg);
}

static void
render_title_bar(_GLFWwindow *window, bool to_front_buffer) {
    const bool is_maximized = window->wl.current.toplevel_states & TOPLEVEL_STATE_MAXIMIZED;
    // The pop-over reads as part of the frame, so it takes the frame's own base
    // color rather than a titlebar palette. Keep these in step with the base
    // colors in window_frame.slang.
    const bool is_dark = glfwGetCurrentSystemColorTheme(true) == GLFW_COLOR_SCHEME_DARK;
    const uint32_t bg_color = is_dark ? 0xff17171a : 0xfff0f0f4;
    const uint32_t fg_color = is_dark ? 0xffe6e6e6 : 0xff444444;
    const uint32_t hover_bg = is_dark ? 0xff2c2c33 : 0xffdedee4;
    uint8_t *output = to_front_buffer ? decs.titlebar.buffer.data.front : decs.titlebar.buffer.data.back;

    // The pop-over carries the window controls and nothing else, so there is
    // no title to draw: just lay down the background the buttons sit on.
    size_t button_size = decs.titlebar.buffer.height;
    unsigned num_buttons = 1;
    if (window->wl.wm_capabilities.maximize) num_buttons++;
    if (window->wl.wm_capabilities.minimize) num_buttons++;
    for (uint32_t *px = (uint32_t *)output, *end = (uint32_t *)(output + decs.titlebar.buffer.size_in_bytes); px < end; px++) *px = bg_color;

    decs.maximize.width = 0;
    decs.minimize.width = 0;
    decs.close.width = 0;
    if (!button_size) return;

    // The buffer is scaled as a whole while a button is scaled on its own, and
    // round(n * w * scale) can come out below n * round(w * scale), so derive
    // the button from the width too rather than trusting them to agree.
    if (decs.titlebar.buffer.width / num_buttons < button_size) button_size = decs.titlebar.buffer.width / num_buttons;
    if (!button_size) return;
    uint8_t *alpha_mask = malloc(button_size * button_size);
    if (!alpha_mask) return;
    int left = decs.titlebar.buffer.width - num_buttons * button_size;
#define drawb(which, antialias, func, hover_bg)       \
    {                                                 \
        render_button(                                \
            func,                                     \
            antialias,                                \
            (uint32_t *)output,                       \
            alpha_mask,                               \
            button_size,                              \
            decs.titlebar.buffer.width,               \
            button_size,                              \
            left,                                     \
            decs.which.hovered ? hover_bg : bg_color, \
            fg_color);                                \
        decs.which.left = left;                       \
        decs.which.width = button_size;               \
        left += button_size;                          \
    }

    if (window->wl.wm_capabilities.minimize) drawb(minimize, false, render_minimize, hover_bg);
    if (window->wl.wm_capabilities.maximize) {
        if (is_maximized) {
            drawb(maximize, false, render_restore, hover_bg);
        } else {
            drawb(maximize, false, render_maximize, hover_bg);
        }
    }
    drawb(close, true, render_close, is_dark ? 0xff880000 : 0xffc80000);
    free(alpha_mask);
#undef drawb
    // Only round when the window itself is rounded: maximized and fullscreen
    // windows square off their corners, matching kitty_chrome_frame().
    if (!(window->wl.current.toplevel_states & (TOPLEVEL_STATE_MAXIMIZED | TOPLEVEL_STATE_FULLSCREEN)))
        round_top_right_corner(
            (uint32_t *)output, decs.titlebar.buffer.width, decs.titlebar.buffer.height, WINDOW_CORNER_RADIUS * decs.for_window_state.fscale);
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

static void
update_title_bar(_GLFWwindow *window) {
    render_title_bar(window, false);
    apply_titlebar_opacity(decs.titlebar.buffer.data.back, decs.titlebar.buffer.size_in_bytes, decs.titlebar_opacity);
    swap_buffers(&decs.titlebar.buffer);
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
static unsigned
popover_width(_GLFWwindow *window) {
    unsigned num_buttons = 1;  // close is always available
    if (window->wl.wm_capabilities.maximize) num_buttons++;
    if (window->wl.wm_capabilities.minimize) num_buttons++;
    return num_buttons * decs.metrics.visible_titlebar_height;
}

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
#define Q(which)                                                                                                      \
    {                                                                                                                 \
        if (decs.which.buffer.a_needs_to_be_destroyed && decs.which.buffer.a) wl_buffer_destroy(decs.which.buffer.a); \
        if (decs.which.buffer.b_needs_to_be_destroyed && decs.which.buffer.b) wl_buffer_destroy(decs.which.buffer.b); \
        memset(&decs.which.buffer, 0, sizeof(_GLFWWaylandBufferPair));                                                \
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
            decs.which.buffer.a_needs_to_be_destroyed = false;                                                                                                \
        } else {                                                                                                                                              \
            decs.which.buffer.b_needs_to_be_destroyed = false;                                                                                                \
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
    if (!window_is_csd_capable(window)) return false;
    const bool has_titlebar = window_has_popover(window);
    const bool is_focused = window->id == _glfw.focusedWindowId;
    const bool focus_changed = is_focused != decs.for_window_state.focused;
    const double current_scale = _glfwWaylandWindowScale(window);
    const bool size_changed =
        (decs.for_window_state.width != window->wl.width || decs.for_window_state.height != window->wl.height ||
         decs.for_window_state.fscale != current_scale || !decs.mapping.data);
    const bool state_changed = decs.for_window_state.toplevel_states != window->wl.current.toplevel_states;
    const bool titlebar_state_changed = (has_titlebar && !decs.titlebar.surface) || (!has_titlebar && decs.titlebar.surface);
    const bool needs_update = focus_changed || size_changed || titlebar_state_changed || decs.buffer_destroyed || state_changed;
    debug(
        "CSD: old.size: %dx%d new.size: %dx%d needs_update: %d size_changed: %d state_changed: %d buffer_destroyed: %d\n",
        decs.for_window_state.width,
        decs.for_window_state.height,
        window->wl.width,
        window->wl.height,
        needs_update,
        size_changed,
        state_changed,
        decs.buffer_destroyed);
    if (!needs_update) return false;
    decs.for_window_state.fscale = current_scale; // used in create_shm_buffers
    if (size_changed || decs.buffer_destroyed || titlebar_state_changed) {
        free_csd_buffers(window);
        if (!create_shm_buffers(window)) return false;
        decs.buffer_destroyed = false;
    }

    const int top_y = 0;  // the pop-over overlays the content, it reserves nothing

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
            // Every attach ends in a release, and the release handler destroys
            // the buffer outright, so the pair is stale by the next tick.
            // csd_change_title() is the repaint that copes with that.
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
    bool has_hovered_button = false;
    int scaled_x = (int)round(decs.for_window_state.fscale * x);
#define c(which)                                                                        \
    if (decs.which.left <= scaled_x && scaled_x < decs.which.left + decs.which.width) { \
        has_hovered_button = true;                                                      \
        if (!decs.which.hovered) {                                                      \
            decs.titlebar_needs_update = true;                                          \
            decs.which.hovered = true;                                                  \
        }                                                                               \
    } else if (decs.which.hovered) {                                                    \
        decs.titlebar_needs_update = true;                                              \
        decs.which.hovered = false;                                                     \
    }

    c(minimize);
    c(maximize);
    c(close);
#undef c
    update_title_bar(window);
    return has_hovered_button;
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
#define Q(which)                         \
    if (decs.which.surface == surface) { \
        decs.focus = CSD_##which;        \
        handle_pointer_move(window);     \
        return;                          \
    } // enter is also a move

    all_surfaces(Q)
#undef Q
        decs.focus = CENTRAL_WINDOW;
    decs.dragging = false;
}

static void
handle_pointer_button(_GLFWwindow *window, uint32_t button, uint32_t state) {
    uint32_t edges = XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    if (button == BTN_LEFT) {
        switch (decs.focus) {
            case CENTRAL_WINDOW: break;
            case CSD_titlebar:
                if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
                    monotonic_t last_click_at = decs.last_click_on_top_decoration_at;
                    decs.last_click_on_top_decoration_at = monotonic();
                    if (decs.last_click_on_top_decoration_at - last_click_at <= _glfwPlatformGetDoubleClickInterval(window)) {
                        decs.last_click_on_top_decoration_at = 0;
                        if (window->wl.current.toplevel_states & TOPLEVEL_STATE_MAXIMIZED) _glfwPlatformRestoreWindow(window);
                        else _glfwPlatformMaximizeWindow(window);
                        return;
                    }
                } else {
                    if (decs.minimize.hovered) _glfwPlatformIconifyWindow(window);
                    else if (decs.maximize.hovered) {
                        if (window->wl.current.toplevel_states & TOPLEVEL_STATE_MAXIMIZED) _glfwPlatformRestoreWindow(window);
                        else _glfwPlatformMaximizeWindow(window);
                        // hack otherwise on GNOME maximize button remains hovered sometimes
                        decs.maximize.hovered = false;
                        decs.titlebar_needs_update = true;
                    } else if (decs.close.hovered) _glfwInputWindowCloseRequest(window);
                }
                decs.dragging = !has_hovered_button(window);
                break;
            case CSD_shadow_left: edges = XDG_TOPLEVEL_RESIZE_EDGE_LEFT; break;
            case CSD_shadow_upper_left: edges = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT; break;
            case CSD_shadow_right: edges = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT; break;
            case CSD_shadow_upper_right: break;
            case CSD_shadow_top:
                if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
                    decs.dragging = true;
                    if (window->wl.xdg.toplevel) xdg_toplevel_move(window->wl.xdg.toplevel, _glfw.wl.seat, _glfw.wl.pointer_serial);
                } else decs.dragging = false;
                break;
            case CSD_shadow_lower_left: edges = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT; break;
            case CSD_shadow_bottom: edges = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM; break;
            case CSD_shadow_lower_right: edges = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT; break;
        }
        if (edges != XDG_TOPLEVEL_RESIZE_EDGE_NONE) xdg_toplevel_resize(window->wl.xdg.toplevel, _glfw.wl.seat, _glfw.wl.pointer_serial, edges);
    } else if (button == BTN_RIGHT) {
        if (decs.focus == CSD_titlebar && window->wl.xdg.toplevel) {
            if (window->wl.wm_capabilities.window_menu)
                // x and y are relative to the pop-over, which sits at the top
                // right of the content, and the window geometry starts at 0,0.
                xdg_toplevel_show_window_menu(
                    window->wl.xdg.toplevel,
                    _glfw.wl.seat,
                    _glfw.wl.pointer_serial,
                    (int32_t)(window->wl.width - (int)popover_width(window) + x),
                    (int32_t)y);
            else _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland compositor does not support showing wndow menu");
            return;
        }
    }
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
    if (decs.titlebar_needs_update) {
        csd_change_title(window);
        if (!window->wl.waiting_for_swap_to_commit) wl_surface_commit(window->wl.surface);
    }
}
#undef x
#undef y
