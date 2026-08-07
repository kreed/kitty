//========================================================================
// GLFW 3.4 Wayland - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2014 Jonas Ådahl <jadahl@gmail.com>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================

#include <wayland-client.h>
#include <dlfcn.h>
#include <poll.h>

typedef VkFlags VkWaylandSurfaceCreateFlagsKHR;

typedef struct VkWaylandSurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkWaylandSurfaceCreateFlagsKHR flags;
    struct wl_display *display;
    struct wl_surface *surface;
} VkWaylandSurfaceCreateInfoKHR;

typedef VkResult(APIENTRY *PFN_vkCreateWaylandSurfaceKHR)(VkInstance, const VkWaylandSurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
typedef VkBool32(APIENTRY *PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)(VkPhysicalDevice, uint32_t, struct wl_display *);

#include "posix_thread.h"
#ifdef __linux__
#include "linux_joystick.h"
#else
#include "null_joystick.h"
#endif
#include "backend_utils.h"
#include "xkb_glfw.h"
#include "wl_cursors.h"

#include "wayland-xdg-shell-client-protocol.h"
#include "wayland-xdg-decoration-unstable-v1-client-protocol.h"
#include "wayland-relative-pointer-unstable-v1-client-protocol.h"
#include "wayland-pointer-constraints-unstable-v1-client-protocol.h"
#include "wayland-primary-selection-unstable-v1-client-protocol.h"
#include "wayland-primary-selection-unstable-v1-client-protocol.h"
#include "wayland-xdg-activation-v1-client-protocol.h"
#include "wayland-cursor-shape-v1-client-protocol.h"
#include "wayland-fractional-scale-v1-client-protocol.h"
#include "wayland-viewporter-client-protocol.h"
#include "wayland-kwin-blur-v1-client-protocol.h"
#include "wayland-ext-background-effect-v1-client-protocol.h"
#include "wayland-wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wayland-single-pixel-buffer-v1-client-protocol.h"
#include "wayland-idle-inhibit-unstable-v1-client-protocol.h"
#include "wayland-keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wayland-xdg-toplevel-icon-v1-client-protocol.h"
#include "wayland-xdg-system-bell-v1-client-protocol.h"
#include "wayland-xdg-toplevel-tag-v1-client-protocol.h"
#include "wayland-xdg-toplevel-drag-v1-client-protocol.h"
#include "wayland-xdg-output-unstable-v1-client-protocol.h"

#define _glfw_dlopen(name) dlopen(name, RTLD_LAZY | RTLD_LOCAL)
#define _glfw_dlclose(handle) dlclose(handle)
#define _glfw_dlsym(handle, name) dlsym(handle, name)

#define _GLFW_PLATFORM_WINDOW_STATE _GLFWwindowWayland wl
#define _GLFW_PLATFORM_LIBRARY_WINDOW_STATE _GLFWlibraryWayland wl
#define _GLFW_PLATFORM_MONITOR_STATE _GLFWmonitorWayland wl
#define _GLFW_PLATFORM_CURSOR_STATE _GLFWcursorWayland wl

#define _GLFW_PLATFORM_CONTEXT_STATE
#define _GLFW_PLATFORM_LIBRARY_CONTEXT_STATE

typedef struct wl_cursor_theme *(*PFN_wl_cursor_theme_load)(const char *, int, struct wl_shm *);
typedef void (*PFN_wl_cursor_theme_destroy)(struct wl_cursor_theme *);
typedef struct wl_cursor *(*PFN_wl_cursor_theme_get_cursor)(struct wl_cursor_theme *, const char *);
typedef struct wl_buffer *(*PFN_wl_cursor_image_get_buffer)(struct wl_cursor_image *);
#define wl_cursor_theme_load _glfw.wl.cursor.theme_load
#define wl_cursor_theme_destroy _glfw.wl.cursor.theme_destroy
#define wl_cursor_theme_get_cursor _glfw.wl.cursor.theme_get_cursor
#define wl_cursor_image_get_buffer _glfw.wl.cursor.image_get_buffer

typedef struct wl_egl_window *(*PFN_wl_egl_window_create)(struct wl_surface *, int, int);
typedef void (*PFN_wl_egl_window_destroy)(struct wl_egl_window *);
typedef void (*PFN_wl_egl_window_resize)(struct wl_egl_window *, int, int, int, int);
#define wl_egl_window_create _glfw.wl.egl.window_create
#define wl_egl_window_destroy _glfw.wl.egl.window_destroy
#define wl_egl_window_resize _glfw.wl.egl.window_resize

struct libdecor;
struct libdecor_frame;
struct libdecor_state;
struct libdecor_configuration;

enum libdecor_error {
    LIBDECOR_ERROR_COMPOSITOR_INCOMPATIBLE,
    LIBDECOR_ERROR_INVALID_FRAME_CONFIGURATION,
};

enum libdecor_window_state {
    LIBDECOR_WINDOW_STATE_NONE = 0,
    LIBDECOR_WINDOW_STATE_ACTIVE = 1,
    LIBDECOR_WINDOW_STATE_MAXIMIZED = 2,
    LIBDECOR_WINDOW_STATE_FULLSCREEN = 4,
    LIBDECOR_WINDOW_STATE_TILED_LEFT = 8,
    LIBDECOR_WINDOW_STATE_TILED_RIGHT = 16,
    LIBDECOR_WINDOW_STATE_TILED_TOP = 32,
    LIBDECOR_WINDOW_STATE_TILED_BOTTOM = 64,
    LIBDECOR_WINDOW_STATE_SUSPENDED = 128,
    LIBDECOR_WINDOW_STATE_RESIZING = 256,
    LIBDECOR_WINDOW_STATE_CONSTRAINED_LEFT = 512,
    LIBDECOR_WINDOW_STATE_CONSTRAINED_RIGHT = 1024,
    LIBDECOR_WINDOW_STATE_CONSTRAINED_TOP = 2048,
    LIBDECOR_WINDOW_STATE_CONSTRAINED_BOTTOM = 4096,
};

enum libdecor_capabilities {
    LIBDECOR_ACTION_MOVE = 1,
    LIBDECOR_ACTION_RESIZE = 2,
    LIBDECOR_ACTION_MINIMIZE = 4,
    LIBDECOR_ACTION_FULLSCREEN = 8,
    LIBDECOR_ACTION_CLOSE = 16,
};

struct libdecor_interface {
    void (*error)(struct libdecor *, enum libdecor_error, const char *);
    void (*reserved0)(void);
    void (*reserved1)(void);
    void (*reserved2)(void);
    void (*reserved3)(void);
    void (*reserved4)(void);
    void (*reserved5)(void);
    void (*reserved6)(void);
    void (*reserved7)(void);
    void (*reserved8)(void);
    void (*reserved9)(void);
};

struct libdecor_frame_interface {
    void (*configure)(struct libdecor_frame *, struct libdecor_configuration *, void *);
    void (*close)(struct libdecor_frame *, void *);
    void (*commit)(struct libdecor_frame *, void *);
    void (*dismiss_popup)(struct libdecor_frame *, const char *, void *);
    void (*reserved0)(void);
    void (*reserved1)(void);
    void (*reserved2)(void);
    void (*reserved3)(void);
    void (*reserved4)(void);
    void (*reserved5)(void);
    void (*reserved6)(void);
    void (*reserved7)(void);
    void (*reserved8)(void);
    void (*reserved9)(void);
};

typedef struct libdecor *(*PFN_libdecor_new)(struct wl_display *, const struct libdecor_interface *);
typedef void (*PFN_libdecor_unref)(struct libdecor *);
typedef int (*PFN_libdecor_get_fd)(struct libdecor *);
typedef int (*PFN_libdecor_dispatch)(struct libdecor *, int);
typedef struct libdecor_frame *(*PFN_libdecor_decorate)(
    struct libdecor *, struct wl_surface *, const struct libdecor_frame_interface *, void *);
typedef void (*PFN_libdecor_frame_unref)(struct libdecor_frame *);
typedef void (*PFN_libdecor_frame_set_app_id)(struct libdecor_frame *, const char *);
typedef void (*PFN_libdecor_frame_set_title)(struct libdecor_frame *, const char *);
typedef void (*PFN_libdecor_frame_set_minimized)(struct libdecor_frame *);
typedef void (*PFN_libdecor_frame_set_fullscreen)(struct libdecor_frame *, struct wl_output *);
typedef void (*PFN_libdecor_frame_unset_fullscreen)(struct libdecor_frame *);
typedef void (*PFN_libdecor_frame_map)(struct libdecor_frame *);
typedef void (*PFN_libdecor_frame_commit)(struct libdecor_frame *, struct libdecor_state *, struct libdecor_configuration *);
typedef void (*PFN_libdecor_frame_set_min_content_size)(struct libdecor_frame *, int, int);
typedef void (*PFN_libdecor_frame_set_max_content_size)(struct libdecor_frame *, int, int);
typedef void (*PFN_libdecor_frame_set_maximized)(struct libdecor_frame *);
typedef void (*PFN_libdecor_frame_unset_maximized)(struct libdecor_frame *);
typedef void (*PFN_libdecor_frame_unset_capabilities)(struct libdecor_frame *, enum libdecor_capabilities);
typedef void (*PFN_libdecor_frame_set_visibility)(struct libdecor_frame *, bool);
typedef bool (*PFN_libdecor_frame_is_visible)(struct libdecor_frame *);
typedef struct xdg_toplevel *(*PFN_libdecor_frame_get_xdg_toplevel)(struct libdecor_frame *);
typedef void (*PFN_libdecor_frame_reveal_titlebar)(struct libdecor_frame *);
typedef bool (*PFN_libdecor_configuration_get_content_size)(
    struct libdecor_configuration *, struct libdecor_frame *, int *, int *);
typedef bool (*PFN_libdecor_configuration_get_window_state)(
    struct libdecor_configuration *, enum libdecor_window_state *);
typedef struct libdecor_state *(*PFN_libdecor_state_new)(int, int);
typedef void (*PFN_libdecor_state_free)(struct libdecor_state *);

#define libdecor_new _glfw.wl.libdecor.new
#define libdecor_unref _glfw.wl.libdecor.unref
#define libdecor_get_fd _glfw.wl.libdecor.get_fd
#define libdecor_dispatch _glfw.wl.libdecor.dispatch
#define libdecor_decorate _glfw.wl.libdecor.decorate
#define libdecor_frame_unref _glfw.wl.libdecor.frame_unref
#define libdecor_frame_set_app_id _glfw.wl.libdecor.frame_set_app_id
#define libdecor_frame_set_title _glfw.wl.libdecor.frame_set_title
#define libdecor_frame_set_minimized _glfw.wl.libdecor.frame_set_minimized
#define libdecor_frame_set_fullscreen _glfw.wl.libdecor.frame_set_fullscreen
#define libdecor_frame_unset_fullscreen _glfw.wl.libdecor.frame_unset_fullscreen
#define libdecor_frame_map _glfw.wl.libdecor.frame_map
#define libdecor_frame_commit _glfw.wl.libdecor.frame_commit
#define libdecor_frame_set_min_content_size _glfw.wl.libdecor.frame_set_min_content_size
#define libdecor_frame_set_max_content_size _glfw.wl.libdecor.frame_set_max_content_size
#define libdecor_frame_set_maximized _glfw.wl.libdecor.frame_set_maximized
#define libdecor_frame_unset_maximized _glfw.wl.libdecor.frame_unset_maximized
#define libdecor_frame_unset_capabilities _glfw.wl.libdecor.frame_unset_capabilities
#define libdecor_frame_set_visibility _glfw.wl.libdecor.frame_set_visibility
#define libdecor_frame_is_visible _glfw.wl.libdecor.frame_is_visible
#define libdecor_frame_get_xdg_toplevel _glfw.wl.libdecor.frame_get_xdg_toplevel
#define libdecor_frame_reveal_titlebar _glfw.wl.libdecor.frame_reveal_titlebar
#define libdecor_configuration_get_content_size _glfw.wl.libdecor.configuration_get_content_size
#define libdecor_configuration_get_window_state _glfw.wl.libdecor.configuration_get_window_state
#define libdecor_state_new _glfw.wl.libdecor.state_new
#define libdecor_state_free _glfw.wl.libdecor.state_free

typedef enum _GLFWCSDSurface {
    CENTRAL_WINDOW,
    CSD_titlebar,
    CSD_shadow_top,
    CSD_shadow_left,
    CSD_shadow_bottom,
    CSD_shadow_right,
    CSD_shadow_upper_left,
    CSD_shadow_upper_right,
    CSD_shadow_lower_left,
    CSD_shadow_lower_right,
} _GLFWCSDSurface;

typedef struct _GLFWWaylandBufferPair {
    struct wl_buffer *a, *b, *front, *back;
    struct {
        uint8_t *a, *b, *front, *back;
    } data;
    bool has_pending_update;
    size_t size_in_bytes, width, height, viewport_width, viewport_height, stride;
    bool a_needs_to_be_destroyed, b_needs_to_be_destroyed;
} _GLFWWaylandBufferPair;

typedef struct _GLFWWaylandCSDSurface {
    struct wl_surface *surface;
    struct wl_subsurface *subsurface;
    struct wp_viewport *wp_viewport;
    _GLFWWaylandBufferPair buffer;
    int x, y;
} _GLFWWaylandCSDSurface;

typedef enum WaylandWindowState {

    TOPLEVEL_STATE_NONE = 0,
    TOPLEVEL_STATE_MAXIMIZED = 1,
    TOPLEVEL_STATE_FULLSCREEN = 2,
    TOPLEVEL_STATE_RESIZING = 4,
    TOPLEVEL_STATE_ACTIVATED = 8,
    TOPLEVEL_STATE_TILED_LEFT = 16,
    TOPLEVEL_STATE_TILED_RIGHT = 32,
    TOPLEVEL_STATE_TILED_TOP = 64,
    TOPLEVEL_STATE_TILED_BOTTOM = 128,
    TOPLEVEL_STATE_SUSPENDED = 256,
    TOPLEVEL_STATE_CONSTRAINED_LEFT = 512,
    TOPLEVEL_STATE_CONSTRAINED_RIGHT = 1024,
    TOPLEVEL_STATE_CONSTRAINED_TOP = 2048,
    TOPLEVEL_STATE_CONSTRAINED_BOTTOM = 4096,
} WaylandWindowState;

typedef struct glfw_wl_xdg_activation_request {
    GLFWid window_id;
    GLFWactivationcallback callback;
    void *callback_data;
    uintptr_t request_id;
    void *token;
} glfw_wl_xdg_activation_request;


static const WaylandWindowState TOPLEVEL_STATE_DOCKED = TOPLEVEL_STATE_MAXIMIZED | TOPLEVEL_STATE_FULLSCREEN | TOPLEVEL_STATE_TILED_TOP |
                                                        TOPLEVEL_STATE_TILED_LEFT | TOPLEVEL_STATE_TILED_RIGHT | TOPLEVEL_STATE_TILED_BOTTOM;

enum WaylandWindowPendingState { PENDING_STATE_TOPLEVEL = 1, PENDING_STATE_DECORATION = 2 };

enum _GLFWWaylandAxisEvent { AXIS_EVENT_UNKNOWN = 0, AXIS_EVENT_CONTINUOUS = 1, AXIS_EVENT_DISCRETE = 2, AXIS_EVENT_VALUE120 = 3 };

// Wayland-specific per-window data
//
typedef struct _GLFWwindowWayland {
    int width, height;
    bool visible, created;
    bool hovered;
    bool transparent;
    struct wl_surface *surface;
    bool waiting_for_swap_to_commit;
    struct wl_egl_window *native;
    struct wl_callback *callback;

    struct {
        struct xdg_surface *surface;
        struct xdg_toplevel *toplevel;
        struct zxdg_toplevel_decoration_v1 *decoration;
        struct {
            int width, height;
        } top_level_bounds;
    } xdg;
    struct libdecor_frame *libdecor_frame;
    struct wp_fractional_scale_v1 *wp_fractional_scale_v1;
    struct wp_viewport *wp_viewport;
    struct org_kde_kwin_blur *org_kde_kwin_blur;
    struct ext_background_effect_surface_v1 *ext_background_effect_surface_v1;
    bool has_blur, expect_scale_from_compositor, window_fully_created;
    struct {
        bool surface_configured, preferred_scale_received, fractional_scale_received;
    } once;
    struct wl_buffer *temp_buffer_used_during_window_creation;
    struct {
        GLFWLayerShellConfig config;
        struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1;
    } layer_shell;

    /* information about axis events on current frame */
    struct {
        struct {
            enum _GLFWWaylandAxisEvent x_axis_type;
            float x;
            enum _GLFWWaylandAxisEvent y_axis_type;
            float y;
        } discrete, continuous;

        /* Event timestamp in nanoseconds */
        bool x_stop_received, y_stop_received;
        uint32_t source_type;
        monotonic_t x_start_time, x_stop_time, y_stop_time, y_start_time;
    } pointer_curr_axis_info;
    GLFWOffsetType prev_frame_offset_type;

    _GLFWcursor *currentCursor;
    double cursorPosX, cursorPosY, allCursorPosX, allCursorPosY;

    char *title;
    char appId[256], windowTag[256];

    // We need to track the monitors the window spans on to calculate the
    // optimal scaling factor.
    struct {
        uint32_t deduced, preferred;
    } integer_scale;
    uint32_t fractional_scale;
    bool initial_scale_notified;
    _GLFWmonitor **monitors;
    int monitorsCount;
    int monitorsSize;

    struct {
        struct zwp_relative_pointer_v1 *relativePointer;
        struct zwp_locked_pointer_v1 *lockedPointer;
    } pointerLock;

    struct {
        bool serverSide, buffer_destroyed, titlebar_needs_update, dragging, titlebar_hidden;
        _GLFWCSDSurface focus;

        _GLFWWaylandCSDSurface titlebar, shadow_left, shadow_right, shadow_top, shadow_bottom, shadow_upper_left, shadow_upper_right, shadow_lower_left,
            shadow_lower_right;

        struct {
            uint8_t *data;
            size_t size;
        } mapping;

        struct {
            int width, height;
            bool focused;
            double fscale;
            WaylandWindowState toplevel_states;
        } for_window_state;

        struct {
            unsigned int width, top, horizontal, vertical, visible_titlebar_height;
        } metrics;

        struct {
            int32_t x, y, width, height;
        } geometry;

        struct {
            bool hovered;
            int width, left;
        } minimize, maximize, close;

        struct {
            uint32_t *data;
            size_t for_decoration_size, stride, segments, corner_size;
        } shadow_tile;
        monotonic_t last_click_on_top_decoration_at;

        uint32_t titlebar_color;
        bool use_custom_titlebar_color;
    } decorations;

    struct {
        unsigned long long id;
        void (*callback)(unsigned long long id);
        struct wl_callback *current_wl_callback;
    } frameCallbackData;

    struct {
        int32_t width, height;
    } user_requested_content_size;

    struct {
        bool minimize, maximize, fullscreen, window_menu;
    } wm_capabilities;


    bool maximize_on_first_show;
    uint32_t pending_state;
    struct {
        int width, height;
        WaylandWindowState toplevel_states;
        uint32_t decoration_mode;
    } current, pending;
    struct zwp_keyboard_shortcuts_inhibitor_v1 *keyboard_shortcuts_inhibitor;
} _GLFWwindowWayland;

typedef struct _GLFWWaylandDataOffer {
    void *id;
    bool is_self_offer;
    bool is_primary;
    const char *mime_for_drop;
    uint32_t source_actions;
    uint32_t dnd_action;
    struct wl_surface *surface;
    const char **mimes;
    size_t mimes_capacity, mimes_count;
    const char **copy_mimes; // Working copy passed to callbacks; pointers into mimes[]
    size_t copy_mimes_count; // Count of entries in copy_mimes (accepted count after callback)
    bool drag_accepted, dropped;
    enum wl_data_device_manager_dnd_action preferred;
    int allowed;
    uint32_t serial;
    struct {
        id_type watch_id;
        int fd;
        char *mime;
    } *requested_drop_data;
    size_t dd_capacity, dd_count;
} _GLFWWaylandDataOffer;

// Wayland-specific global data
//
typedef struct _GLFWlibraryWayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_data_device_manager *dataDeviceManager;
    struct wl_data_device *dataDevice;
    struct xdg_wm_base *wmBase;
    int xdg_wm_base_version;
    struct zxdg_decoration_manager_v1 *decorationManager;
    struct zwp_relative_pointer_manager_v1 *relativePointerManager;
    struct zwp_pointer_constraints_v1 *pointerConstraints;
    struct wl_data_source *dataSourceForClipboard;
    struct zwp_primary_selection_device_manager_v1 *primarySelectionDeviceManager;
    struct zwp_primary_selection_device_v1 *primarySelectionDevice;
    struct zwp_primary_selection_source_v1 *dataSourceForPrimarySelection;
    struct xdg_activation_v1 *xdg_activation_v1;
    struct xdg_toplevel_icon_manager_v1 *xdg_toplevel_icon_manager_v1;
    struct xdg_system_bell_v1 *xdg_system_bell_v1;
    struct xdg_toplevel_tag_manager_v1 *xdg_toplevel_tag_manager_v1;
    struct xdg_toplevel_drag_manager_v1 *xdg_toplevel_drag_manager_v1;
    struct wp_cursor_shape_manager_v1 *wp_cursor_shape_manager_v1;
    struct wp_cursor_shape_device_v1 *wp_cursor_shape_device_v1;
    struct wp_fractional_scale_manager_v1 *wp_fractional_scale_manager_v1;
    struct wp_viewporter *wp_viewporter;
    struct org_kde_kwin_blur_manager *org_kde_kwin_blur_manager;
    struct ext_background_effect_manager_v1 *ext_background_effect_manager_v1;
    uint32_t ext_background_effect_capabilities;
    struct zwlr_layer_shell_v1 *zwlr_layer_shell_v1;
    uint32_t zwlr_layer_shell_v1_version;
    struct wp_single_pixel_buffer_manager_v1 *wp_single_pixel_buffer_manager_v1;
    struct zwp_idle_inhibit_manager_v1 *idle_inhibit_manager;
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *keyboard_shortcuts_inhibit_manager;
    struct zwp_pointer_gestures_v1 *pointer_gestures;
    struct zwp_pointer_gesture_hold_v1 *pointer_gesture_hold;
    struct zxdg_output_manager_v1 *xdg_output_manager;

    int compositorVersion;
    int seatVersion;
    bool has_key_repeat_events;

    struct wl_surface *cursorSurface;
    GLFWCursorShape cursorPreviousShape;
    uint32_t serial, input_serial, pointer_serial, pointer_enter_serial, keyboard_enter_serial;
    // serial of the button press that started the current pointer implicit
    // grab, and the number of currently pressed pointer buttons. Requests
    // such as wl_data_device.start_drag are silently ignored by compositors
    // unless made with the serial of an active implicit grab.
    uint32_t pointer_grab_serial;
    unsigned pointer_button_count;

    int32_t keyboardRepeatRate;
    monotonic_t keyboardRepeatDelay;

    struct {
        uint32_t key;
        id_type keyRepeatTimer;
        GLFWid keyboardFocusId;
    } keyRepeatInfo;
    id_type cursorAnimationTimer;
    _GLFWXKBData xkb;
    _GLFWDBUSData dbus;

    _GLFWwindow *pointerFocus;
    GLFWid keyboardFocusId;
    GLFWid lastKeyboardFocusId;

    struct {
        void *handle;

        PFN_wl_cursor_theme_load theme_load;
        PFN_wl_cursor_theme_destroy theme_destroy;
        PFN_wl_cursor_theme_get_cursor theme_get_cursor;
        PFN_wl_cursor_image_get_buffer image_get_buffer;
    } cursor;

    struct {
        void *handle;

        PFN_wl_egl_window_create window_create;
        PFN_wl_egl_window_destroy window_destroy;
        PFN_wl_egl_window_resize window_resize;
    } egl;

    struct {
        void *handle;
        struct libdecor *context;
        id_type watch_id;

        PFN_libdecor_new new;
        PFN_libdecor_unref unref;
        PFN_libdecor_get_fd get_fd;
        PFN_libdecor_dispatch dispatch;
        PFN_libdecor_decorate decorate;
        PFN_libdecor_frame_unref frame_unref;
        PFN_libdecor_frame_set_app_id frame_set_app_id;
        PFN_libdecor_frame_set_title frame_set_title;
        PFN_libdecor_frame_set_minimized frame_set_minimized;
        PFN_libdecor_frame_set_fullscreen frame_set_fullscreen;
        PFN_libdecor_frame_unset_fullscreen frame_unset_fullscreen;
        PFN_libdecor_frame_map frame_map;
        PFN_libdecor_frame_commit frame_commit;
        PFN_libdecor_frame_set_min_content_size frame_set_min_content_size;
        PFN_libdecor_frame_set_max_content_size frame_set_max_content_size;
        PFN_libdecor_frame_set_maximized frame_set_maximized;
        PFN_libdecor_frame_unset_maximized frame_unset_maximized;
        PFN_libdecor_frame_unset_capabilities frame_unset_capabilities;
        PFN_libdecor_frame_set_visibility frame_set_visibility;
        PFN_libdecor_frame_is_visible frame_is_visible;
        PFN_libdecor_frame_get_xdg_toplevel frame_get_xdg_toplevel;
        PFN_libdecor_frame_reveal_titlebar frame_reveal_titlebar;
        PFN_libdecor_configuration_get_content_size configuration_get_content_size;
        PFN_libdecor_configuration_get_window_state configuration_get_window_state;
        PFN_libdecor_state_new state_new;
        PFN_libdecor_state_free state_free;
    } libdecor;

    struct {
        glfw_wl_xdg_activation_request *array;
        size_t capacity, sz;
    } activation_requests;

    EventLoopData eventLoopData;
    _GLFWWaylandDataOffer untyped_data_offers[8];
    _GLFWWaylandDataOffer clipboard_data_offer, primary_data_offer, drop_data_offer;

    bool has_preferred_buffer_scale;
    char *compositor_name;

    // Drag source state
    struct {
        struct wl_data_source *source;
        struct wl_surface *drag_icon;
        struct wp_viewport *drag_viewport;
        struct xdg_toplevel_drag_v1 *toplevel_drag;
        struct xdg_surface *toplevel_xdg_surface;
        struct xdg_toplevel *toplevel_xdg_toplevel;
        struct wl_buffer *toplevel_buffer;
        // wl_data_device.start_drag is silently ignored by compositors when
        // its serial does not match an active pointer implicit grab, which
        // can happen as drags are started asynchronously and the client side
        // view of the grab can be stale. A wl_display.sync issued right after
        // start_drag detects this: any compositor event proving the DND
        // session is live (wl_pointer.leave, wl_data_device.enter, any
        // wl_data_source event) is ordered before the sync callback, so if
        // the callback fires first the start_drag was dropped.
        struct wl_callback *start_confirmation;
        bool session_confirmed;
        // The drag toplevel was configured before the session was confirmed,
        // mapping it was deferred so it cannot end up as a stray regular
        // window if start_drag was silently ignored.
        bool toplevel_map_deferred;
        // Number of extra sync roundtrips issued waiting for confirmation;
        // some compositors (e.g. niri) send the confirmation event (drag icon
        // wl_surface.enter) after the first sync roundtrip, so we retry once
        // before concluding that start_drag was silently ignored.
        uint8_t sync_retries;
        struct {
            const char *mime_type;
            int fd;
            GLFWid watch_id;
            char *pending_data;
            size_t sz, offset;
        } *data_requests;
        size_t count, capacity;
        GLFWDragOperationType action;
    } drag;
} _GLFWlibraryWayland;

// Wayland-specific per-monitor data
//
typedef struct _GLFWmonitorWayland {
    struct wl_output *output;
    struct zxdg_output_v1 *xdg_output;
    uint32_t name;
    int currentMode;

    int x;
    int y;
    int scale;
    int32_t transform;

    int32_t xdg_logical_width;
    int32_t xdg_logical_height;
    double fractional_scale;
    bool xdg_size_received;
    bool xdg_position_received;

} _GLFWmonitorWayland;

// Wayland-specific per-cursor data
//
typedef struct _GLFWcursorWayland {
    struct wl_cursor *cursor;
    struct wl_buffer *buffer;
    int width, height;
    int xhot, yhot;
    unsigned int currentImage;
    /** The scale of the cursor, or 0 if the cursor should be loaded late, or -1 if the cursor variable itself is unused. */
    int scale;
    /** Cursor shape stored to allow late cursor loading in setCursorImage. */
    GLFWCursorShape shape;
} _GLFWcursorWayland;


void _glfwAddOutputWayland(uint32_t name, uint32_t version);
void _glfwCreateXdgOutputWayland(_GLFWmonitor *monitor);
void _glfwWaylandBeforeBufferSwap(_GLFWwindow *window);
void _glfwWaylandAfterBufferSwap(_GLFWwindow *window);
void _glfwSetupWaylandDataDevice(void);
void _glfwSetupWaylandPrimarySelectionDevice(void);
double _glfwWaylandWindowScale(_GLFWwindow *);
int _glfwWaylandIntegerWindowScale(_GLFWwindow *);
void animateCursorImage(id_type timer_id, void *data);
struct wl_cursor *_glfwLoadCursor(GLFWCursorShape, struct wl_cursor_theme *);
void destroy_data_offer(_GLFWWaylandDataOffer *);
const char *_glfwWaylandCompositorName(void);
void _glfwWaylandConfirmDragSession(void);

typedef struct wayland_cursor_shape {
    int which;
    const char *name;
} wayland_cursor_shape;

wayland_cursor_shape glfw_cursor_shape_to_wayland_cursor_shape(GLFWCursorShape g);
