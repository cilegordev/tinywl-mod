/*
 * tinywl-panel.h  –  Public API for the TinyWL bottom taskbar panel
 *
 * The panel renders at the bottom of the first output and contains:
 *   LEFT  – Live taskbar: one button per mapped toplevel window.
 *            Clicking raises and focuses that window.
 *            The focused window's button is highlighted in blue.
 *   RIGHT – Clock in the format "Sabtu, 16 Mei 2026  13:25", refreshed
 *            every 30 seconds.
 *
 * Integration checklist (tinywl.c / tinywl.h)
 * --------------------------------------------
 *  1. tinywl.h – add forward declaration and field to struct tinywl_server:
 *
 *         struct tinywl_panel;          // forward declaration
 *
 *         struct tinywl_server {
 *             ...
 *             struct tinywl_panel *panel;
 *         };
 *
 *  2. tinywl.c – include this header:
 *
 *         #include "tinywl-panel.h"
 *
 *  3. After wlr_backend_start() + setenv("WAYLAND_DISPLAY", …):
 *
 *         server.panel = tinywl_panel_create(&server);
 *
 *  4. In xdg_toplevel_map() handler, after the surface is mapped:
 *
 *         tinywl_panel_on_map(server->panel, toplevel);
 *
 *  5. In xdg_toplevel_unmap() handler, before freeing the toplevel:
 *
 *         tinywl_panel_on_unmap(server->panel, toplevel);
 *
 *  6. In focus_toplevel(), after wlr_xdg_toplevel_set_activated():
 *
 *         tinywl_panel_on_focus(server->panel, toplevel);
 *
 *  7. Shutdown path, before wlr_scene_node_destroy():
 *
 *         tinywl_panel_destroy(server->panel);
 *
 *  8. Makefile – add tinywl-panel.c to the compile line.
 */

#ifndef TINYWL_PANEL_H
#define TINYWL_PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

struct tinywl_panel;
struct tinywl_server;
struct tinywl_toplevel;

/**
 * tinywl_panel_create() – create and display the bottom panel.
 *
 * Call after wlr_backend_start() and setenv("WAYLAND_DISPLAY", …).
 * Returns NULL only on fatal allocation failure.
 */
struct tinywl_panel *tinywl_panel_create(struct tinywl_server *server);

/**
 * tinywl_panel_on_map() – register a newly mapped toplevel in the taskbar.
 *
 * Call from the xdg_toplevel surface map handler in tinywl.c.
 * Safe to call with p == NULL.
 */
void tinywl_panel_on_map(struct tinywl_panel *p,
                          struct tinywl_toplevel *toplevel);

/**
 * tinywl_panel_on_unmap() – remove an unmapped toplevel from the taskbar.
 *
 * Call from the xdg_toplevel surface unmap handler in tinywl.c,
 * before the toplevel is freed.
 * Safe to call with p == NULL.
 */
void tinywl_panel_on_unmap(struct tinywl_panel *p,
                             struct tinywl_toplevel *toplevel);

/**
 * tinywl_panel_on_focus() – update the focused-window highlight.
 *
 * Call from focus_toplevel() in tinywl.c after activating the toplevel.
 * Pass NULL when no window is focused.
 * Safe to call with p == NULL.
 */
void tinywl_panel_on_focus(struct tinywl_panel *p,
                             struct tinywl_toplevel *toplevel);

/**
 * tinywl_panel_get_height() – return panel height in logical pixels (32).
 *
 * Use this to shrink maximized windows so they do not cover the panel:
 *
 *     wlr_xdg_toplevel_set_size(tl, out_w,
 *         out_h - tinywl_panel_get_height(server->panel));
 *
 * Returns 0 if p is NULL.
 */
int tinywl_panel_get_height(struct tinywl_panel *p);

/**
 * tinywl_panel_raise_to_top() – ensure the panel is always above windows.
 *
 * Call after wlr_scene_node_raise_to_top() on any window so the panel
 * bar is never obscured.
 * Safe to call with p == NULL.
 */
void tinywl_panel_raise_to_top(struct tinywl_panel *p);

/**
 * tinywl_panel_set_callbacks() – register minimize/restore callbacks.
 *
 * Must be called once after tinywl_panel_create(), passing pointers to
 * minimize_toplevel() and restore_toplevel() in tinywl.c so the taskbar
 * click handler can call back into the compositor without creating a
 * circular include dependency.
 *
 * @p:          panel handle
 * @cb_minimize: pointer to minimize_toplevel() in tinywl.c
 * @cb_restore:  pointer to restore_toplevel() in tinywl.c
 */
void tinywl_panel_set_callbacks(struct tinywl_panel *p,
    void (*cb_minimize)(struct tinywl_toplevel *toplevel),
    void (*cb_restore)(struct tinywl_toplevel *toplevel));

/**
 * tinywl_panel_restore_toplevel() – restore a minimized window.
 *
 * Called by the taskbar click handler when the user clicks on the button
 * of a minimized window.  Re-enables the scene node, raises it, and
 * transfers keyboard focus.
 *
 * Internally calls restore_toplevel() in tinywl.c via the function pointer
 * stored in the panel at creation time.
 * Safe to call with p == NULL or toplevel == NULL.
 */
void tinywl_panel_restore_toplevel(struct tinywl_panel *p,
                                    struct tinywl_toplevel *toplevel);

/**
 * tinywl_panel_minimize_toplevel() – minimize a window from the taskbar.
 *
 * Called by the taskbar click handler when the user clicks the button of
 * the currently focused window (toggle-to-minimize behaviour).
 * Internally calls minimize_toplevel() in tinywl.c.
 * Safe to call with p == NULL or toplevel == NULL.
 */
void tinywl_panel_minimize_toplevel(struct tinywl_panel *p,
                                     struct tinywl_toplevel *toplevel);

/**
 * tinywl_panel_destroy() – release all panel resources.
 *
 * Call before wlr_scene_node_destroy() in the compositor shutdown path.
 * Safe to call with p == NULL.
 */
void tinywl_panel_destroy(struct tinywl_panel *p);

#ifdef __cplusplus
}
#endif
#endif
