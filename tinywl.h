/* tinywl.h: struct definitions for tinywl_server and related types. */

#ifndef TINYWL_H
#define TINYWL_H

#include <stdint.h>
#include <stdbool.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>

/* XWayland support (wlroots 0.17 built with xwayland); TINYWL_HAS_XWAYLAND lets the Makefile disable it. */
#ifndef TINYWL_NO_XWAYLAND
#include <wlr/xwayland.h>
#define TINYWL_HAS_XWAYLAND 1
#endif

/* Forward declaration – defined in menu.c */
struct tinywl_menu;

/* Forward declaration – defined in background.c */
struct tinywl_background;

/* Forward declaration – defined in tinywl-panel.c */
struct tinywl_panel;

/* Forward declaration – defined in services.c */
struct tinywl_services;

/* Cursor mode enum */
typedef enum tinywl_cursor_mode {
    TINYWL_CURSOR_PASSTHROUGH,
    TINYWL_CURSOR_MOVE,
    TINYWL_CURSOR_RESIZE,
} tinywl_cursor_mode;

/* Edge-snap ("split screen") zone, detected from cursor position while moving a window; TINYWL_SNAP_NONE means not near an edge. */
typedef enum tinywl_snap_zone {
    TINYWL_SNAP_NONE,
    TINYWL_SNAP_LEFT,
    TINYWL_SNAP_RIGHT,
    TINYWL_SNAP_TOP,
} tinywl_snap_zone;

/* Structs */

struct tinywl_server {
    struct wl_display             *wl_display;
    struct wlr_backend            *backend;
    struct wlr_renderer           *renderer;
    struct wlr_allocator          *allocator;
    struct wlr_scene              *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_xdg_shell          *xdg_shell;
    struct wl_listener             new_xdg_surface;
    struct wl_listener             new_xwayland_surface;
    struct wl_list                 toplevels;
    struct wl_list                 popups;

    struct wlr_cursor             *cursor;
    struct wlr_xcursor_manager    *cursor_mgr;
    struct wl_listener             cursor_motion;
    struct wl_listener             cursor_motion_absolute;
    struct wl_listener             cursor_button;
    struct wl_listener             cursor_axis;
    struct wl_listener             cursor_frame;

    struct wlr_seat               *seat;
    struct wl_listener             new_input;
    struct wl_listener             request_cursor;
    struct wl_listener             request_set_selection;
    struct wl_listener             request_start_drag;
    struct wl_listener             start_drag;
    struct wl_listener             drag_destroy;
    struct wlr_scene_tree         *drag_icon;

    /* Implicit pointer grab: while a button is held, motion keeps going to the surface focused when it was pressed, regardless of what's under the cursor now. */
    struct wlr_surface             *pointer_grab_surface;
    double                          pointer_grab_offset_x, pointer_grab_offset_y;
    int                             pointer_button_count;
    struct wl_listener              pointer_grab_surface_destroy;

    struct wl_list                 keyboards;
    tinywl_cursor_mode             cursor_mode;
    struct tinywl_toplevel        *grabbed_toplevel;
    double                         grab_x, grab_y;
    struct wlr_box                 grab_geobox;
    uint32_t                       resize_edges;

    /* Edge-snap preview shown while moving a window near a screen edge; snap_pending is recomputed each motion, snap_preview is the translucent rect. */
    tinywl_snap_zone                snap_pending;
    struct wlr_scene_rect          *snap_preview;

    struct wlr_output_layout      *output_layout;
    struct wl_list                 outputs;
    struct wl_listener             new_output;

    struct wlr_screencopy_manager_v1 *screencopy_mgr;

    /* Right-click popup menu (analogous to twm Button1=root=f.menu, uses BTN_RIGHT) */
    struct tinywl_menu            *menu;

    /* Wallpaper / desktop background */
    struct tinywl_background      *background;

    /* Bottom taskbar panel (Weston-style) */
    struct tinywl_panel           *panel;

    /* wlr_compositor is stored so services.c can pass it to wlr_xwayland_create(). */
    struct wlr_compositor         *compositor;

    /* Background services: D-Bus, XWayland, GVFS, Polkit, audio */
    struct tinywl_services        *services;
};

struct tinywl_output {
    struct wl_list         link;
    struct tinywl_server  *server;
    struct wlr_output     *wlr_output;
    struct wl_listener     frame;
    struct wl_listener     request_state;
    struct wl_listener     destroy;
};

struct tinywl_toplevel {
    struct wl_list              link;
    struct tinywl_server       *server;
    struct wlr_xdg_toplevel    *xdg_toplevel;       /* NULL for XWayland-backed toplevels */
    struct wlr_xwayland_surface *xwayland_surface;  /* NULL for xdg-shell-backed toplevels */
    struct wlr_scene_tree      *scene_tree;
    struct wl_listener          map;
    struct wl_listener          unmap;
    struct wl_listener          destroy;
    struct wl_listener          request_move;
    struct wl_listener          request_resize;
    struct wl_listener          request_maximize;
    struct wl_listener          request_fullscreen;
    struct wl_listener          request_minimize;
    struct wl_listener          xwayland_request_configure; /* XWayland-only */
    struct wl_listener          xwayland_associate;         /* XWayland-only */
    struct wl_listener          xwayland_dissociate;        /* XWayland-only */

    /* Maximize state */
    bool            maximized;
    struct wlr_box  saved_geometry; /* x,y,width,height before maximize */

    /* Edge-snap state; shares saved_geometry with maximize since a window is only ever snapped or maximized, never both. */
    bool             snapped;
    tinywl_snap_zone snapped_zone;

    /* Fullscreen state */
    bool            fullscreen;
    struct wlr_box  saved_geometry_fullscreen; /* x,y,width,height before fullscreen */

    /* Minimize state: scene node is hidden when minimized == true */
    bool            minimized;
    
    /* Dialog/Modal and Progress tracking */
    bool            is_dialog;
    bool            is_progress;
};

struct tinywl_keyboard {
    struct wl_list         link;
    struct tinywl_server  *server;
    struct wlr_keyboard   *wlr_keyboard;
    struct wl_listener     modifiers;
    struct wl_listener     key;
    struct wl_listener     destroy;
};

#endif
