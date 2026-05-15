/*
 * tinywl.h – Struct definitions for tinywl_server and related types.
 *
 * This header is self-contained: it includes all wlr headers needed
 * so that struct tinywl_server can be parsed anywhere.
 *
 * Both tinywl.c and menu.c include this header.
 * tinywl.c no longer defines these structs directly.
 */

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
#include <wlr/util/log.h>
#include <wlr/util/box.h>

/* Forward declaration – defined in menu.c */
struct tinywl_menu;

/* Cursor mode enum */
typedef enum tinywl_cursor_mode {
    TINYWL_CURSOR_PASSTHROUGH,
    TINYWL_CURSOR_MOVE,
    TINYWL_CURSOR_RESIZE,
} tinywl_cursor_mode;

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
    struct wl_list                 toplevels;

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
    struct wl_list                 keyboards;
    tinywl_cursor_mode             cursor_mode;
    struct tinywl_toplevel        *grabbed_toplevel;
    double                         grab_x, grab_y;
    struct wlr_box                 grab_geobox;
    uint32_t                       resize_edges;

    struct wlr_output_layout      *output_layout;
    struct wl_list                 outputs;
    struct wl_listener             new_output;

    /* Right-click popup menu (analogous to twm Button1=root=f.menu, uses BTN_RIGHT) */
    struct tinywl_menu            *menu;
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
    struct wlr_xdg_toplevel    *xdg_toplevel;
    struct wlr_scene_tree      *scene_tree;
    struct wl_listener          map;
    struct wl_listener          unmap;
    struct wl_listener          destroy;
    struct wl_listener          request_move;
    struct wl_listener          request_resize;
    struct wl_listener          request_maximize;
    struct wl_listener          request_fullscreen;
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
