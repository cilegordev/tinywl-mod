/*
 * tinywl-panel.c  –  Weston-style taskbar panel for TinyWL
 *
 * Overview
 * --------
 * This module implements a bottom-anchored panel with two zones:
 *
 *   LEFT  → Taskbar: one button per open window (tinywl_toplevel).
 *            Clicking a button focuses / raises that window.
 *            The button for the currently focused window is highlighted.
 *            The button label is the window's XDG title (or app_id if no
 *            title is set, or "?" as a last-resort fallback).
 *
 *   RIGHT → Clock: date + time in the format "Sabtu, 16 Mei 2026  13:25",
 *            right-aligned, refreshed every 30 seconds.
 *
 * Why taskbar instead of static launchers?
 * -----------------------------------------
 * The compositor already has a right-click popup menu (menu.c) that launches
 * Terminal, File Manager and Browser.  Adding duplicate launcher buttons on
 * the panel would be redundant.  A live taskbar is both more useful and the
 * natural complement to the existing right-click menu.
 *
 * Architecture
 * ------------
 * The panel is rendered entirely inside the wlroots scene graph:
 *
 *   wlr_scene_tree  (panel->tree)
 *   ├── wlr_scene_rect   bg_rect        — dark semi-transparent bar
 *   ├── wlr_scene_rect   task_rects[]   — per-window highlight rects
 *   ├── wlr_scene_rect   sep_rects[]    — separator lines between buttons
 *   └── wlr_scene_buffer text_buf       — Cairo text overlay (titles + clock)
 *
 * The text buffer is a single ARGB32 Cairo surface uploaded through wl_shm
 * (same non-blocking socketpair technique as menu.c / background.c).
 * Whenever the taskbar changes (window open/close/rename, focus change, clock
 * tick) only the Cairo surface is repainted and wlr_scene_buffer_set_buffer()
 * is called — no scene-tree surgery required.
 *
 * Integration with tinywl.c
 * -------------------------
 * tinywl.c must call three hooks so the panel stays in sync:
 *
 *   tinywl_panel_on_map(panel, toplevel)    — when a new window is mapped
 *   tinywl_panel_on_unmap(panel, toplevel)  — when a window is unmapped
 *   tinywl_panel_on_focus(panel, toplevel)  — when focus changes (NULL = none)
 *
 * Called from xdg_toplevel_map(), xdg_toplevel_unmap(), and focus_toplevel().
 *
 * Clock format
 * ------------
 * strftime format : "%A, %d %B %Y  %H:%M"
 * Example output  : "Saturday, 16 May 2026  13:25"
 * Day/month names come from LC_TIME in the process environment.
 *
 * Copyright
 * ---------
 * Independent implementation for wlroots / TinyWL.
 * Design reference: Weston desktop-shell (clients/desktop-shell.c),
 * © Kristian Høgsberg and Collabora Ltd., MIT licence.
 * No Weston source text was copied.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "panel.h"
#include "tinywl.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <wayland-server-core.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>

/* wlr_buffer_try_from_resource: available since wlroots 0.17.4 */
struct wlr_buffer *wlr_buffer_try_from_resource(struct wl_resource *resource);

/* Visual constants */

#define PANEL_HEIGHT      32
#define PANEL_SPACING     8

#define PANEL_FONT        "Sans Bold 10"
#define CLOCK_FONT_SIZE   13.0

#define CLOCK_FORMAT      "%A, %d %B %Y  %H:%M"
#define CLOCK_REFRESH_MS  30000
#define CLOCK_AREA_W      400

#define MAX_TASKS         32
#define TASK_BTN_MAX_W    200
#define TASK_BTN_MIN_W    80

/* Panel background */
#define PANEL_BG_R  0.08f
#define PANEL_BG_G  0.08f
#define PANEL_BG_B  0.10f
#define PANEL_BG_A  0.88f

/* Focused button */
#define TASK_FOCUS_R  0.20f
#define TASK_FOCUS_G  0.50f
#define TASK_FOCUS_B  0.80f
#define TASK_FOCUS_A  0.80f

/* Hovered button */
#define TASK_HOVER_R  0.25f
#define TASK_HOVER_G  0.25f
#define TASK_HOVER_B  0.30f
#define TASK_HOVER_A  0.80f

/* Separator */
#define SEP_R  0.30f
#define SEP_G  0.30f
#define SEP_B  0.35f
#define SEP_A  0.60f

/* Text */
#define TEXT_FOCUS_R  1.00f
#define TEXT_FOCUS_G  1.00f
#define TEXT_FOCUS_B  1.00f
#define TEXT_NORM_R   0.75f
#define TEXT_NORM_G   0.75f
#define TEXT_NORM_B   0.80f
#define TEXT_SHADOW_A 0.70f

/* Internal wl_shm client */

struct panel_shm_ctx {
    struct wl_display  *display;
    struct wl_registry *registry;
    struct wl_shm      *shm;
    bool                ready;
};

static void panel_shm_global(void *data, struct wl_registry *reg,
                              uint32_t name, const char *iface, uint32_t ver)
{
    struct panel_shm_ctx *ctx = data;
    if (strcmp(iface, "wl_shm") == 0) {
        ctx->shm   = wl_registry_bind(reg, name, &wl_shm_interface,
                                       ver < 1 ? 1 : (int)ver);
        ctx->ready = true;
    }
}
static void panel_shm_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }

static const struct wl_registry_listener panel_shm_reg_listener = {
    .global        = panel_shm_global,
    .global_remove = panel_shm_global_remove,
};

/* Task entry */

struct panel_task {
    struct tinywl_toplevel *toplevel;
    struct wl_listener      set_title;
    struct wl_listener      destroy;
    struct tinywl_panel    *panel;
    int x;
    int w;
};

/* Main panel state */

struct tinywl_panel {
    struct tinywl_server *server;

    int out_w, out_h, panel_y;

    struct wlr_scene_tree   *tree;
    struct wlr_scene_rect   *bg_rect;
    struct wlr_scene_rect   *task_rects[MAX_TASKS];
    struct wlr_scene_rect   *sep_rects[MAX_TASKS];
    struct wlr_scene_buffer *text_buf;

    struct wl_client        *wl_client;
    struct panel_shm_ctx     shm_ctx;
    struct wl_buffer        *wl_buf;
    uint32_t                 wl_buf_id;
    int                      memfd;
    void                    *memdata;
    size_t                   memsize;
    int                      stride;

    struct panel_task        tasks[MAX_TASKS];
    int                      n_tasks;

    struct tinywl_toplevel  *focused;
    int                      hovered;

    struct wl_listener       cursor_button;
    struct wl_listener       cursor_motion;
    struct wl_event_source  *clock_timer;

    /*
     * Callbacks into tinywl.c for minimize / restore.
     * Set by tinywl_panel_set_callbacks() after panel creation.
     * This avoids a circular dependency between panel.c and tinywl.c.
     */
    void (*cb_minimize)(struct tinywl_toplevel *toplevel);
    void (*cb_restore)(struct tinywl_toplevel *toplevel);
};

/* Forward declarations */

static void panel_layout(struct tinywl_panel *p);
static void panel_redraw(struct tinywl_panel *p);

/* Helpers */

static const char *task_label(const struct panel_task *t)
{
    if (!t->toplevel) return "?";
    struct wlr_xdg_toplevel *xt = t->toplevel->xdg_toplevel;
    if (xt->title  && xt->title[0])  return xt->title;
    if (xt->app_id && xt->app_id[0]) return xt->app_id;
    return "?";
}

/* Layout */

static void panel_layout(struct tinywl_panel *p)
{
    /* Hide all rects first */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (p->task_rects[i])
            wlr_scene_node_set_enabled(&p->task_rects[i]->node, false);
        if (p->sep_rects[i])
            wlr_scene_node_set_enabled(&p->sep_rects[i]->node, false);
    }

    if (p->n_tasks == 0) return;

    int taskbar_w = p->out_w - CLOCK_AREA_W;
    int btn_w = taskbar_w / p->n_tasks;
    if (btn_w > TASK_BTN_MAX_W) btn_w = TASK_BTN_MAX_W;
    if (btn_w < TASK_BTN_MIN_W) btn_w = TASK_BTN_MIN_W;

    int cur_x = 0;
    for (int i = 0; i < p->n_tasks; i++) {
        p->tasks[i].x = cur_x;
        p->tasks[i].w = btn_w;

        if (!p->task_rects[i]) { cur_x += btn_w; continue; }

        bool focused = (p->tasks[i].toplevel == p->focused);
        bool hovered = (i == p->hovered && !focused);

        float cr, cg, cb, ca;
        if (focused) {
            cr = TASK_FOCUS_R; cg = TASK_FOCUS_G; cb = TASK_FOCUS_B; ca = TASK_FOCUS_A;
        } else if (hovered) {
            cr = TASK_HOVER_R; cg = TASK_HOVER_G; cb = TASK_HOVER_B; ca = TASK_HOVER_A;
        } else {
            cr = 0.0f; cg = 0.0f; cb = 0.0f; ca = 0.0f;
        }
        const float col[4] = { cr, cg, cb, ca };
        wlr_scene_rect_set_color(p->task_rects[i], col);
        wlr_scene_rect_set_size(p->task_rects[i], btn_w, PANEL_HEIGHT);
        wlr_scene_node_set_position(&p->task_rects[i]->node, cur_x, 0);
        wlr_scene_node_set_enabled(&p->task_rects[i]->node, true);

        /* Separator on right edge */
        if (p->sep_rects[i]) {
            const float sc[4] = { SEP_R, SEP_G, SEP_B, SEP_A };
            wlr_scene_rect_set_color(p->sep_rects[i], sc);
            wlr_scene_rect_set_size(p->sep_rects[i], 1, PANEL_HEIGHT);
            wlr_scene_node_set_position(&p->sep_rects[i]->node, cur_x + btn_w - 1, 0);
            wlr_scene_node_set_enabled(&p->sep_rects[i]->node, true);
        }

        cur_x += btn_w;
    }
}

/* Cairo rendering */

static void panel_draw_text(struct tinywl_panel *p)
{
    if (!p->memdata) return;

    memset(p->memdata, 0, p->memsize);

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        (uint8_t *)p->memdata, CAIRO_FORMAT_ARGB32,
        p->out_w, PANEL_HEIGHT, p->stride);
    if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(cs); return;
    }
    cairo_t *cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Pango for task labels */
    PangoLayout          *lo = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(PANEL_FONT);
    pango_layout_set_font_description(lo, fd);
    pango_font_description_free(fd);
    pango_layout_set_ellipsize(lo, PANGO_ELLIPSIZE_END);

    for (int i = 0; i < p->n_tasks; i++) {
        const char *label   = task_label(&p->tasks[i]);
        bool        focused = (p->tasks[i].toplevel == p->focused);
        int bx = p->tasks[i].x;
        int bw = p->tasks[i].w;

        pango_layout_set_width(lo, (bw - PANEL_SPACING * 2) * PANGO_SCALE);
        pango_layout_set_text(lo, label, -1);

        int tw = 0, th = 0;
        pango_layout_get_pixel_size(lo, &tw, &th);

        double tx = bx + PANEL_SPACING;
        double ty = (PANEL_HEIGHT - th) / 2.0;

        /* Shadow */
        cairo_move_to(cr, tx + 1.0, ty + 1.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, TEXT_SHADOW_A);
        pango_cairo_show_layout(cr, lo);

        /* Label */
        cairo_move_to(cr, tx, ty);
        if (focused)
            cairo_set_source_rgba(cr, TEXT_FOCUS_R, TEXT_FOCUS_G, TEXT_FOCUS_B, 1.0);
        else
            cairo_set_source_rgba(cr, TEXT_NORM_R, TEXT_NORM_G, TEXT_NORM_B, 1.0);
        pango_cairo_show_layout(cr, lo);
    }
    g_object_unref(lo);

    /* Clock */
    time_t     rawtime  = time(NULL);
    struct tm *timeinfo = localtime(&rawtime);
    char       clockstr[256];
    strftime(clockstr, sizeof(clockstr), CLOCK_FORMAT, timeinfo);

    cairo_set_font_size(cr, CLOCK_FONT_SIZE);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, clockstr, &ext);

    double cx = (double)(p->out_w - PANEL_SPACING * 2) - ext.width;
    double cy = PANEL_HEIGHT / 2.0 - 1.0 + ext.height / 2.0;

    cairo_move_to(cr, cx + 1.0, cy + 1.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, TEXT_SHADOW_A);
    cairo_show_text(cr, clockstr);

    cairo_move_to(cr, cx, cy);
    cairo_set_source_rgba(cr, TEXT_FOCUS_R, TEXT_FOCUS_G, TEXT_FOCUS_B, 1.0);
    cairo_show_text(cr, clockstr);

    cairo_surface_flush(cs);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);
}

/* Scene buffer refresh */

static void panel_redraw(struct tinywl_panel *p)
{
    if (!p->text_buf || !p->memdata) return;
    panel_draw_text(p);

    struct wl_resource *res = wl_client_get_object(p->wl_client, p->wl_buf_id);
    if (!res) return;

    struct wlr_buffer *wlr_buf = wlr_buffer_try_from_resource(res);
    if (!wlr_buf) return;

    wlr_scene_buffer_set_buffer(p->text_buf, wlr_buf);
    wlr_buffer_unlock(wlr_buf);
}

/* wl_shm upload */

static struct wlr_buffer *panel_shm_upload(struct tinywl_panel *p)
{
    int sv[2];
    p->stride  = p->out_w * 4;
    p->memsize = (size_t)p->stride * PANEL_HEIGHT;

    p->memfd = memfd_create("tinywl-panel", MFD_CLOEXEC);
    if (p->memfd < 0) { wlr_log_errno(WLR_ERROR, "panel: memfd_create"); return NULL; }
    if (ftruncate(p->memfd, (off_t)p->memsize) < 0) {
        wlr_log_errno(WLR_ERROR, "panel: ftruncate");
        close(p->memfd); p->memfd = -1; return NULL;
    }
    p->memdata = mmap(NULL, p->memsize, PROT_READ | PROT_WRITE,
                      MAP_SHARED, p->memfd, 0);
    if (p->memdata == MAP_FAILED) {
        wlr_log_errno(WLR_ERROR, "panel: mmap");
        close(p->memfd); p->memfd = -1; p->memdata = NULL; return NULL;
    }
    memset(p->memdata, 0, p->memsize);

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        wlr_log_errno(WLR_ERROR, "panel: socketpair"); return NULL;
    }
    p->wl_client = wl_client_create(p->server->wl_display, sv[0]);
    if (!p->wl_client) {
        wlr_log(WLR_ERROR, "panel: wl_client_create failed");
        close(sv[0]); close(sv[1]); return NULL;
    }
    p->shm_ctx.display = wl_display_connect_to_fd(sv[1]);
    if (!p->shm_ctx.display) {
        wlr_log(WLR_ERROR, "panel: wl_display_connect_to_fd failed");
        wl_client_destroy(p->wl_client); p->wl_client = NULL; return NULL;
    }
    p->shm_ctx.registry = wl_display_get_registry(p->shm_ctx.display);
    p->shm_ctx.ready    = false;
    wl_registry_add_listener(p->shm_ctx.registry, &panel_shm_reg_listener, &p->shm_ctx);

    struct wl_event_loop *loop = wl_display_get_event_loop(p->server->wl_display);
    for (int i = 0; i < 50 && !p->shm_ctx.ready; i++) {
        wl_display_flush(p->shm_ctx.display);
        wl_display_flush_clients(p->server->wl_display);
        wl_event_loop_dispatch(loop, 1);
        wl_display_flush_clients(p->server->wl_display);
        if (wl_display_prepare_read(p->shm_ctx.display) == 0) {
            struct pollfd pfd = { .fd = wl_display_get_fd(p->shm_ctx.display),
                                  .events = POLLIN, .revents = 0 };
            if (poll(&pfd, 1, 1) > 0 && (pfd.revents & POLLIN))
                wl_display_read_events(p->shm_ctx.display);
            else
                wl_display_cancel_read(p->shm_ctx.display);
        }
        wl_display_dispatch_pending(p->shm_ctx.display);
    }
    if (!p->shm_ctx.shm) {
        wlr_log(WLR_ERROR, "panel: wl_shm not found"); return NULL;
    }

    struct wl_shm_pool *pool =
        wl_shm_create_pool(p->shm_ctx.shm, p->memfd, (int32_t)p->memsize);
    if (!pool) { wlr_log(WLR_ERROR, "panel: shm pool failed"); return NULL; }

    p->wl_buf = wl_shm_pool_create_buffer(pool, 0, p->out_w, PANEL_HEIGHT,
                                           p->stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!p->wl_buf) { wlr_log(WLR_ERROR, "panel: create_buffer failed"); return NULL; }

    p->wl_buf_id = wl_proxy_get_id((struct wl_proxy *)p->wl_buf);

    wl_display_flush(p->shm_ctx.display);
    wl_display_flush_clients(p->server->wl_display);
    wl_event_loop_dispatch(loop, 0);
    wl_display_flush_clients(p->server->wl_display);
    wl_display_dispatch_pending(p->shm_ctx.display);

    struct wl_resource *res = wl_client_get_object(p->wl_client, p->wl_buf_id);
    if (!res) {
        wlr_log(WLR_ERROR, "panel: wl_resource id=%u not found", p->wl_buf_id);
        return NULL;
    }
    struct wlr_buffer *wlr_buf = wlr_buffer_try_from_resource(res);
    if (!wlr_buf) {
        wlr_log(WLR_ERROR, "panel: wlr_buffer_try_from_resource failed");
        return NULL;
    }
    return wlr_buf;
}

/* Clock timer */

static int panel_clock_tick(void *data)
{
    struct tinywl_panel *p = data;
    panel_redraw(p);
    wl_event_source_timer_update(p->clock_timer, CLOCK_REFRESH_MS);
    return 0;
}

/* Hit testing */

static int panel_hit_task(struct tinywl_panel *p, double ox, double oy)
{
    int py = (int)oy - p->panel_y;
    if (py < 0 || py >= PANEL_HEIGHT) return -1;
    int px = (int)ox;
    for (int i = 0; i < p->n_tasks; i++) {
        if (px >= p->tasks[i].x && px < p->tasks[i].x + p->tasks[i].w)
            return i;
    }
    return -1;
}

/* Input handlers */

static void on_cursor_motion(struct wl_listener *listener, void *data)
{
    struct tinywl_panel *p = wl_container_of(listener, p, cursor_motion);
    (void)data;
    int prev = p->hovered;
    p->hovered = panel_hit_task(p, p->server->cursor->x, p->server->cursor->y);
    if (p->hovered != prev) {
        panel_layout(p);
        panel_redraw(p);
    }
}

static void on_cursor_button(struct wl_listener *listener, void *data)
{
    struct tinywl_panel *p = wl_container_of(listener, p, cursor_button);
    struct wlr_pointer_button_event *ev = data;

    if (ev->button != BTN_LEFT ||
        ev->state  != WL_POINTER_BUTTON_STATE_RELEASED)
        return;

    int idx = panel_hit_task(p, p->server->cursor->x, p->server->cursor->y);
    if (idx < 0 || idx >= p->n_tasks) return;

    struct tinywl_toplevel *tl = p->tasks[idx].toplevel;
    if (!tl) return;

    if (tl->minimized) {
        /*
         * Window is minimized — restore it: re-enable scene node, raise,
         * and focus.  Calls back into tinywl.c via the function pointer
         * stored in the panel (restore_toplevel).
         */
        tinywl_panel_restore_toplevel(p, tl);
    } else if (tl == p->focused) {
        /*
         * Clicking the taskbar button of the already-focused window
         * minimizes it (toggle behaviour, matching common desktop UX).
         */
        tinywl_panel_minimize_toplevel(p, tl);
    } else {
        /* Raise and focus the non-minimized window */
        wlr_scene_node_raise_to_top(&tl->scene_tree->node);
        tinywl_panel_raise_to_top(p);
        struct wlr_surface *surface = tl->xdg_toplevel->base->surface;
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(p->server->seat);
        wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, true);
        if (kb)
            wlr_seat_keyboard_notify_enter(p->server->seat, surface,
                                            kb->keycodes, kb->num_keycodes,
                                            &kb->modifiers);
        tinywl_panel_on_focus(p, tl);
    }
}

/* Task listener callbacks */

static void on_task_set_title(struct wl_listener *listener, void *data)
{
    (void)data;
    struct panel_task *t = wl_container_of(listener, t, set_title);
    panel_redraw(t->panel);
}

static void on_task_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    struct panel_task *t = wl_container_of(listener, t, destroy);
    wl_list_remove(&t->set_title.link);
    wl_list_remove(&t->destroy.link);
    wl_list_init(&t->set_title.link);
    wl_list_init(&t->destroy.link);
    t->toplevel = NULL;
}

/* Public API */

struct tinywl_panel *tinywl_panel_create(struct tinywl_server *server)
{
    struct tinywl_panel *p = calloc(1, sizeof(*p));
    if (!p) { wlr_log(WLR_ERROR, "panel: out of memory"); return NULL; }
    p->server  = server;
    p->memfd   = -1;
    p->hovered = -1;

    for (int i = 0; i < MAX_TASKS; i++) {
        wl_list_init(&p->tasks[i].set_title.link);
        wl_list_init(&p->tasks[i].destroy.link);
        p->tasks[i].panel = p;
    }

    /* Output geometry */
    p->out_w = 1920; p->out_h = 1080;
    struct wlr_box out_box = {0};
    if (!wl_list_empty(&server->outputs)) {
        struct tinywl_output *out =
            wl_container_of(server->outputs.next, out, link);
        wlr_output_effective_resolution(out->wlr_output, &p->out_w, &p->out_h);
        wlr_output_layout_get_box(server->output_layout,
                                   out->wlr_output, &out_box);
    }
    p->panel_y = p->out_h - PANEL_HEIGHT;

    /* Scene tree */
    p->tree = wlr_scene_tree_create(&server->scene->tree);
    if (!p->tree) { free(p); return NULL; }
    wlr_scene_node_set_position(&p->tree->node, out_box.x, out_box.y + p->panel_y);

    /* Background rect */
    const float bg[4] = { PANEL_BG_R, PANEL_BG_G, PANEL_BG_B, PANEL_BG_A };
    p->bg_rect = wlr_scene_rect_create(p->tree, p->out_w, PANEL_HEIGHT, bg);

    /* Task rects + separators (hidden initially) */
    const float tr[4] = { 0, 0, 0, 0 };
    const float sc[4] = { SEP_R, SEP_G, SEP_B, SEP_A };
    for (int i = 0; i < MAX_TASKS; i++) {
        p->task_rects[i] = wlr_scene_rect_create(p->tree, 1, PANEL_HEIGHT, tr);
        if (p->task_rects[i])
            wlr_scene_node_set_enabled(&p->task_rects[i]->node, false);
        p->sep_rects[i] = wlr_scene_rect_create(p->tree, 1, PANEL_HEIGHT, sc);
        if (p->sep_rects[i])
            wlr_scene_node_set_enabled(&p->sep_rects[i]->node, false);
    }

    /* Text overlay */
    struct wlr_buffer *wlr_buf = panel_shm_upload(p);
    if (wlr_buf) {
        panel_draw_text(p);
        p->text_buf = wlr_scene_buffer_create(p->tree, wlr_buf);
        wlr_buffer_unlock(wlr_buf);
    }

    /* Clock timer */
    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    p->clock_timer = wl_event_loop_add_timer(loop, panel_clock_tick, p);
    if (p->clock_timer)
        wl_event_source_timer_update(p->clock_timer, CLOCK_REFRESH_MS);

    /* Input */
    p->cursor_button.notify = on_cursor_button;
    wl_signal_add(&server->cursor->events.button, &p->cursor_button);
    p->cursor_motion.notify = on_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &p->cursor_motion);

    return p;
}

void tinywl_panel_on_map(struct tinywl_panel *p, struct tinywl_toplevel *toplevel)
{
    if (!p || !toplevel || p->n_tasks >= MAX_TASKS) return;

    struct panel_task *t = &p->tasks[p->n_tasks];
    t->toplevel = toplevel;
    t->panel    = p;
    t->x = 0; t->w = 0;

    t->set_title.notify = on_task_set_title;
    wl_signal_add(&toplevel->xdg_toplevel->events.set_title, &t->set_title);

    t->destroy.notify = on_task_destroy;
    wl_signal_add(&toplevel->xdg_toplevel->base->events.destroy, &t->destroy);

    p->n_tasks++;
    panel_layout(p);
    panel_redraw(p);
}

void tinywl_panel_on_unmap(struct tinywl_panel *p, struct tinywl_toplevel *toplevel)
{
    if (!p || !toplevel) return;

    int found = -1;
    for (int i = 0; i < p->n_tasks; i++) {
        if (p->tasks[i].toplevel == toplevel) { found = i; break; }
    }
    if (found < 0) return;

    wl_list_remove(&p->tasks[found].set_title.link);
    wl_list_remove(&p->tasks[found].destroy.link);
    wl_list_init(&p->tasks[found].set_title.link);
    wl_list_init(&p->tasks[found].destroy.link);

    /*
     * Shift the remaining tasks down WITHOUT copying live wl_listener
     * structs by value. `p->tasks[i] = p->tasks[i + 1]` looks harmless but
     * a struct assignment copies the wl_listener's `link` pointer values
     * as raw bytes; the wl_signal that owns that listener (embedded in
     * the corresponding xdg_toplevel, untouched by this code) keeps
     * pointing at the OLD slot's address, not the new one. Any later
     * event on that signal — including the memset() that used to run on
     * the vacated last slot right after this loop — then reads/writes a
     * listener_list node through a stale address, corrupting the list
     * (or zeroing a still-referenced `notify` pointer) and eventually
     * crashing with a jump through a garbage/NULL function pointer.
     *
     * Instead, explicitly unregister each shifted task's listeners from
     * their old address and re-register them at the new one.
     */
    for (int i = found; i < p->n_tasks - 1; i++) {
        struct tinywl_toplevel *moved_toplevel = p->tasks[i + 1].toplevel;

        wl_list_remove(&p->tasks[i + 1].set_title.link);
        wl_list_remove(&p->tasks[i + 1].destroy.link);

        p->tasks[i].toplevel = moved_toplevel;
        p->tasks[i].panel    = p->tasks[i + 1].panel;
        p->tasks[i].x        = p->tasks[i + 1].x;
        p->tasks[i].w        = p->tasks[i + 1].w;

        p->tasks[i].set_title.notify = on_task_set_title;
        wl_signal_add(&moved_toplevel->xdg_toplevel->events.set_title,
                      &p->tasks[i].set_title);

        p->tasks[i].destroy.notify = on_task_destroy;
        wl_signal_add(&moved_toplevel->xdg_toplevel->base->events.destroy,
                      &p->tasks[i].destroy);

        wl_list_init(&p->tasks[i + 1].set_title.link);
        wl_list_init(&p->tasks[i + 1].destroy.link);
        p->tasks[i + 1].toplevel = NULL;
    }

    p->n_tasks--;
    memset(&p->tasks[p->n_tasks], 0, sizeof(p->tasks[0]));
    p->tasks[p->n_tasks].panel = p;
    wl_list_init(&p->tasks[p->n_tasks].set_title.link);
    wl_list_init(&p->tasks[p->n_tasks].destroy.link);

    if (p->focused == toplevel) p->focused = NULL;
    if (p->hovered >= p->n_tasks) p->hovered = -1;

    panel_layout(p);
    panel_redraw(p);
}

void tinywl_panel_on_focus(struct tinywl_panel *p, struct tinywl_toplevel *toplevel)
{
    if (!p || p->focused == toplevel) return;
    p->focused = toplevel;
    panel_layout(p);
    panel_redraw(p);
}

int tinywl_panel_get_height(struct tinywl_panel *p)
{
    if (!p) return 0;
    return PANEL_HEIGHT;
}

void tinywl_panel_raise_to_top(struct tinywl_panel *p)
{
    if (!p || !p->tree) return;
    wlr_scene_node_raise_to_top(&p->tree->node);
}

void tinywl_panel_set_callbacks(struct tinywl_panel *p,
    void (*cb_minimize)(struct tinywl_toplevel *toplevel),
    void (*cb_restore)(struct tinywl_toplevel *toplevel))
{
    if (!p) return;
    p->cb_minimize = cb_minimize;
    p->cb_restore  = cb_restore;
}

void tinywl_panel_restore_toplevel(struct tinywl_panel *p,
                                    struct tinywl_toplevel *toplevel)
{
    if (!p || !toplevel || !p->cb_restore) return;
    p->cb_restore(toplevel);
    /* Update taskbar highlight after restore */
    tinywl_panel_on_focus(p, toplevel);
}

void tinywl_panel_minimize_toplevel(struct tinywl_panel *p,
                                     struct tinywl_toplevel *toplevel)
{
    if (!p || !toplevel || !p->cb_minimize) return;
    p->cb_minimize(toplevel);
}

void tinywl_panel_hide(struct tinywl_panel *p)
{
    if (!p || !p->tree) return;
    wlr_scene_node_set_enabled(&p->tree->node, false);
}

void tinywl_panel_show(struct tinywl_panel *p)
{
    if (!p || !p->tree) return;
    wlr_scene_node_set_enabled(&p->tree->node, true);
}

void tinywl_panel_resize(struct tinywl_panel *p, struct tinywl_server *server)
{
    if (!p) return;

    /*
     * Re-reads the current output's resolution and rebuilds everything
     * that was sized against the OLD resolution at creation time: the
     * background rect, the tree's position (panel sits at the bottom of
     * the output), and the text-overlay shm buffer (clock/task labels),
     * which is allocated at exactly out_w pixels wide. Without this, the
     * panel stayed pinned to whatever size the output was when tinywl
     * started, so resizing/maximizing a nested X11 backend window later
     * left the taskbar only covering the original (smaller) width.
     */
    int new_w = 1920, new_h = 1080;
    struct wlr_box out_box = {0};
    if (!wl_list_empty(&server->outputs)) {
        struct tinywl_output *out =
            wl_container_of(server->outputs.next, out, link);
        wlr_output_effective_resolution(out->wlr_output, &new_w, &new_h);
        wlr_output_layout_get_box(server->output_layout, out->wlr_output, &out_box);
    }

    p->out_w = new_w;
    p->out_h = new_h;
    p->panel_y = p->out_h - PANEL_HEIGHT;

    wlr_scene_node_set_position(&p->tree->node, out_box.x, out_box.y + p->panel_y);

    if (p->bg_rect)
        wlr_scene_rect_set_size(p->bg_rect, p->out_w, PANEL_HEIGHT);

    /* Tear down the old text-overlay shm buffer/client — it was sized to
     * the previous out_w — and rebuild it at the new width. */
    if (p->text_buf) {
        wlr_scene_node_destroy(&p->text_buf->node);
        p->text_buf = NULL;
    }
    if (p->wl_buf)           { wl_buffer_destroy(p->wl_buf); p->wl_buf = NULL; }
    if (p->shm_ctx.shm)      { wl_shm_destroy(p->shm_ctx.shm); }
    if (p->shm_ctx.registry) { wl_registry_destroy(p->shm_ctx.registry); }
    if (p->shm_ctx.display)  { wl_display_disconnect(p->shm_ctx.display); }
    if (p->wl_client)        { wl_client_destroy(p->wl_client); p->wl_client = NULL; }
    if (p->memdata)          { munmap(p->memdata, p->memsize); p->memdata = NULL; }
    if (p->memfd >= 0)       { close(p->memfd); }
    p->memfd = -1;
    p->memsize = 0;
    memset(&p->shm_ctx, 0, sizeof(p->shm_ctx));

    struct wlr_buffer *wlr_buf = panel_shm_upload(p);
    if (wlr_buf) {
        panel_draw_text(p);
        p->text_buf = wlr_scene_buffer_create(p->tree, wlr_buf);
        wlr_buffer_unlock(wlr_buf);
    }

    panel_layout(p);
    panel_redraw(p);
}

void tinywl_panel_destroy(struct tinywl_panel *p)
{
    if (!p) return;

    if (p->clock_timer) wl_event_source_remove(p->clock_timer);
    wl_list_remove(&p->cursor_button.link);
    wl_list_remove(&p->cursor_motion.link);

    for (int i = 0; i < p->n_tasks; i++) {
        wl_list_remove(&p->tasks[i].set_title.link);
        wl_list_remove(&p->tasks[i].destroy.link);
    }

    if (p->tree)             wlr_scene_node_destroy(&p->tree->node);
    if (p->wl_buf)           wl_buffer_destroy(p->wl_buf);
    if (p->shm_ctx.shm)      wl_shm_destroy(p->shm_ctx.shm);
    if (p->shm_ctx.registry) wl_registry_destroy(p->shm_ctx.registry);
    if (p->shm_ctx.display)  wl_display_disconnect(p->shm_ctx.display);
    if (p->wl_client)        wl_client_destroy(p->wl_client);
    if (p->memdata)          munmap(p->memdata, p->memsize);
    if (p->memfd >= 0)       close(p->memfd);

    free(p);
}
