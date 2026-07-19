/* 
 * panel.c: bottom taskbar; left zone lists windows, right zone shows a clock. 
 * Synced from tinywl.c via tinywl_panel_on_map/on_unmap/on_focus. 
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

#include <math.h>

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

/* Minimum horizontal cursor travel (px) after pressing a taskbar button
 * before it counts as a drag-to-reorder rather than a click. */
#define DRAG_THRESHOLD_PX 6.0

/* Calendar tooltip (shown when hovering the clock area) */
#define CAL_TOOLTIP_W     200
#define CAL_TOOLTIP_H     196
#define CAL_MARGIN          8
#define CAL_HEADER_H       26
#define CAL_WEEKDAY_H      18
#define CAL_ROWS             6
#define CAL_ROW_H         ((CAL_TOOLTIP_H - CAL_MARGIN * 2 - CAL_HEADER_H - CAL_WEEKDAY_H) / CAL_ROWS)

#define CAL_BG_R  0.09f
#define CAL_BG_G  0.09f
#define CAL_BG_B  0.12f
#define CAL_BG_A  0.96f

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

    /* order[slot] maps a visual slot to its tasks[] index; drag-reorder only permutes this array, tasks[] entries never move. */
    int                      order[MAX_TASKS];

    struct tinywl_toplevel  *focused;
    int                      hovered;

    /* Taskbar drag-to-reorder state */
    bool                     drag_pending;  /* button down on a task, not yet dragging */
    bool                     drag_active;    /* moved past the threshold, actively dragging */
    int                      drag_slot;      /* current visual slot of the dragged task */
    double                   drag_start_x;   /* cursor x at press, for threshold check */

    /* Calendar tooltip toggled by clicking the clock; clock_hit_x/w is the clock text's own bounding box, recomputed in panel_draw_text(), not the wider CLOCK_AREA_W layout reservation. */
    bool                     calendar_open;
    int                      clock_hit_x;
    int                      clock_hit_w;
    struct wlr_scene_tree   *cal_tree;
    struct wlr_scene_buffer *cal_text_buf;
    struct wl_buffer        *cal_wl_buf;
    uint32_t                 cal_wl_buf_id;
    int                      cal_memfd;
    void                    *cal_memdata;
    size_t                   cal_memsize;
    int                      cal_stride;

    struct wl_listener       cursor_button;
    struct wl_listener       cursor_motion;
    struct wl_event_source  *clock_timer;

    /* Callbacks into tinywl.c for minimize/restore, set by tinywl_panel_set_callbacks() to avoid a circular dependency. */
    void (*cb_minimize)(struct tinywl_toplevel *toplevel);
    void (*cb_restore)(struct tinywl_toplevel *toplevel);
};

/* Forward declarations */

static void panel_layout(struct tinywl_panel *p);
static void panel_redraw(struct tinywl_panel *p);
static int panel_slot_for_real(struct tinywl_panel *p, int real);
static int panel_target_slot_for_x(struct tinywl_panel *p, double ox);
static void panel_move_order(struct tinywl_panel *p, int from, int to);
static void panel_order_remove_physical(struct tinywl_panel *p, int removed_phys, int old_n_tasks);
static struct wlr_buffer *panel_calendar_shm_upload(struct tinywl_panel *p);
static void panel_draw_calendar(struct tinywl_panel *p);
static void panel_calendar_redraw(struct tinywl_panel *p);
static void panel_calendar_show(struct tinywl_panel *p, bool show);
static bool panel_hit_clock(struct tinywl_panel *p, double ox, double oy);

/* Helpers */

static const char *task_label(const struct panel_task *t)
{
    if (!t->toplevel) return "?";

    /* xdg-shell and XWayland store title/class in different places. */
    if (t->toplevel->xdg_toplevel) {
        struct wlr_xdg_toplevel *xt = t->toplevel->xdg_toplevel;
        if (xt->title  && xt->title[0])  return xt->title;
        if (xt->app_id && xt->app_id[0]) return xt->app_id;
#ifdef TINYWL_HAS_XWAYLAND
    } else if (t->toplevel->xwayland_surface) {
        struct wlr_xwayland_surface *xs = t->toplevel->xwayland_surface;
        if (xs->title && xs->title[0]) return xs->title;
        if (xs->class && xs->class[0]) return xs->class;
#endif
    }
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
    for (int slot = 0; slot < p->n_tasks; slot++) {
        int real = p->order[slot];
        p->tasks[real].x = cur_x;
        p->tasks[real].w = btn_w;

        if (!p->task_rects[slot]) { cur_x += btn_w; continue; }

        bool focused = (p->tasks[real].toplevel == p->focused);
        bool hovered = (real == p->hovered && !focused);

        float cr, cg, cb, ca;
        if (focused) {
            cr = TASK_FOCUS_R; cg = TASK_FOCUS_G; cb = TASK_FOCUS_B; ca = TASK_FOCUS_A;
        } else if (hovered) {
            cr = TASK_HOVER_R; cg = TASK_HOVER_G; cb = TASK_HOVER_B; ca = TASK_HOVER_A;
        } else {
            cr = 0.0f; cg = 0.0f; cb = 0.0f; ca = 0.0f;
        }
        const float col[4] = { cr, cg, cb, ca };
        wlr_scene_rect_set_color(p->task_rects[slot], col);
        wlr_scene_rect_set_size(p->task_rects[slot], btn_w, PANEL_HEIGHT);
        wlr_scene_node_set_position(&p->task_rects[slot]->node, cur_x, 0);
        wlr_scene_node_set_enabled(&p->task_rects[slot]->node, true);

        /* Separator on right edge */
        if (p->sep_rects[slot]) {
            const float sc[4] = { SEP_R, SEP_G, SEP_B, SEP_A };
            wlr_scene_rect_set_color(p->sep_rects[slot], sc);
            wlr_scene_rect_set_size(p->sep_rects[slot], 1, PANEL_HEIGHT);
            wlr_scene_node_set_position(&p->sep_rects[slot]->node, cur_x + btn_w - 1, 0);
            wlr_scene_node_set_enabled(&p->sep_rects[slot]->node, true);
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

    /* Exact click target for the calendar popup: the clock text's own bounding box, not the wider CLOCK_AREA_W reservation. */
    {
        const int pad = 8;
        int hit_x = (int)cx - pad;
        if (hit_x < 0) hit_x = 0;
        int hit_w = (int)ext.width + pad * 2;
        if (hit_x + hit_w > p->out_w) hit_w = p->out_w - hit_x;
        p->clock_hit_x = hit_x;
        p->clock_hit_w = hit_w;
    }

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

/* Calendar tooltip shm upload reuses the wl_client/wl_shm connection from panel_shm_upload(), with its own fixed-size buffer. */
static struct wlr_buffer *panel_calendar_shm_upload(struct tinywl_panel *p)
{
    if (!p->shm_ctx.shm || !p->wl_client) return NULL;

    p->cal_stride  = CAL_TOOLTIP_W * 4;
    p->cal_memsize = (size_t)p->cal_stride * CAL_TOOLTIP_H;

    p->cal_memfd = memfd_create("tinywl-panel-cal", MFD_CLOEXEC);
    if (p->cal_memfd < 0) { wlr_log_errno(WLR_ERROR, "panel: cal memfd_create"); return NULL; }
    if (ftruncate(p->cal_memfd, (off_t)p->cal_memsize) < 0) {
        wlr_log_errno(WLR_ERROR, "panel: cal ftruncate");
        close(p->cal_memfd); p->cal_memfd = -1; return NULL;
    }
    p->cal_memdata = mmap(NULL, p->cal_memsize, PROT_READ | PROT_WRITE,
                          MAP_SHARED, p->cal_memfd, 0);
    if (p->cal_memdata == MAP_FAILED) {
        wlr_log_errno(WLR_ERROR, "panel: cal mmap");
        close(p->cal_memfd); p->cal_memfd = -1; p->cal_memdata = NULL; return NULL;
    }
    memset(p->cal_memdata, 0, p->cal_memsize);

    struct wl_shm_pool *pool =
        wl_shm_create_pool(p->shm_ctx.shm, p->cal_memfd, (int32_t)p->cal_memsize);
    if (!pool) { wlr_log(WLR_ERROR, "panel: cal shm pool failed"); return NULL; }

    p->cal_wl_buf = wl_shm_pool_create_buffer(pool, 0, CAL_TOOLTIP_W, CAL_TOOLTIP_H,
                                               p->cal_stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!p->cal_wl_buf) { wlr_log(WLR_ERROR, "panel: cal create_buffer failed"); return NULL; }

    p->cal_wl_buf_id = wl_proxy_get_id((struct wl_proxy *)p->cal_wl_buf);

    struct wl_event_loop *loop = wl_display_get_event_loop(p->server->wl_display);
    wl_display_flush(p->shm_ctx.display);
    wl_display_flush_clients(p->server->wl_display);
    wl_event_loop_dispatch(loop, 0);
    wl_display_flush_clients(p->server->wl_display);
    wl_display_dispatch_pending(p->shm_ctx.display);

    struct wl_resource *res = wl_client_get_object(p->wl_client, p->cal_wl_buf_id);
    if (!res) {
        wlr_log(WLR_ERROR, "panel: cal wl_resource id=%u not found", p->cal_wl_buf_id);
        return NULL;
    }
    struct wlr_buffer *wlr_buf = wlr_buffer_try_from_resource(res);
    if (!wlr_buf) {
        wlr_log(WLR_ERROR, "panel: cal wlr_buffer_try_from_resource failed");
        return NULL;
    }
    return wlr_buf;
}

/* Number of days in a given (proleptic Gregorian) month.
 * month is 0-based (0 = January), matching struct tm::tm_mon. */
static int days_in_month(int year, int month)
{
    static const int dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 1) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        return leap ? 29 : 28;
    }
    return dim[month];
}

/* Renders a small month calendar (header, weekday row, day grid) into the
 * tooltip's Cairo surface. Today's cell is highlighted with a filled
 * circle. Month/weekday names honour LC_TIME, same as the clock text. */
static void panel_draw_calendar(struct tinywl_panel *p)
{
    if (!p->cal_memdata) return;

    memset(p->cal_memdata, 0, p->cal_memsize);

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        (uint8_t *)p->cal_memdata, CAIRO_FORMAT_ARGB32,
        CAL_TOOLTIP_W, CAL_TOOLTIP_H, p->cal_stride);
    if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(cs); return;
    }
    cairo_t *cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Rounded card background */
    double radius = 8.0, w = CAL_TOOLTIP_W, h = CAL_TOOLTIP_H;
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - radius, radius,     radius, -M_PI / 2, 0);
    cairo_arc(cr, w - radius, h - radius, radius, 0, M_PI / 2);
    cairo_arc(cr, radius,     h - radius, radius, M_PI / 2, M_PI);
    cairo_arc(cr, radius,     radius,     radius, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, CAL_BG_R, CAL_BG_G, CAL_BG_B, CAL_BG_A);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, SEP_R, SEP_G, SEP_B, 0.90);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    time_t     rawtime = time(NULL);
    struct tm  now_tm;
    localtime_r(&rawtime, &now_tm);

    /* Header: "Month Year" */
    char header[64];
    strftime(header, sizeof(header), "%B %Y", &now_tm);

    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13.0);
    cairo_text_extents(cr, header, &ext);
    double hx = (w - ext.width) / 2.0 - ext.x_bearing;
    double hy = CAL_MARGIN + 12.0;
    cairo_set_source_rgba(cr, TEXT_FOCUS_R, TEXT_FOCUS_G, TEXT_FOCUS_B, 1.0);
    cairo_move_to(cr, hx, hy);
    cairo_show_text(cr, header);

    /* Weekday header row, week starting Monday */
    double col_w      = (w - CAL_MARGIN * 2) / 7.0;
    double grid_x0    = CAL_MARGIN;
    double weekday_y  = CAL_MARGIN + CAL_HEADER_H;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.0);
    cairo_set_source_rgba(cr, TEXT_NORM_R, TEXT_NORM_G, TEXT_NORM_B, 1.0);

    /* 2024-01-01 is a Monday; use it as a locale-aware weekday-name anchor */
    struct tm anchor = {0};
    anchor.tm_year = 124; anchor.tm_mon = 0; anchor.tm_mday = 1;
    for (int i = 0; i < 7; i++) {
        struct tm day_tm = anchor;
        day_tm.tm_mday = 1 + i;
        mktime(&day_tm);
        char wd[8];
        strftime(wd, sizeof(wd), "%a", &day_tm);
        cairo_text_extents(cr, wd, &ext);
        double cx = grid_x0 + col_w * i + (col_w - ext.width) / 2.0 - ext.x_bearing;
        cairo_move_to(cr, cx, weekday_y + 12.0);
        cairo_show_text(cr, wd);
    }

    /* Day grid */
    int year  = now_tm.tm_year + 1900;
    int month = now_tm.tm_mon;
    int today = now_tm.tm_mday;

    struct tm first_tm = {0};
    first_tm.tm_year = now_tm.tm_year;
    first_tm.tm_mon  = month;
    first_tm.tm_mday = 1;
    mktime(&first_tm);
    int first_wday = (first_tm.tm_wday + 6) % 7; /* Sunday=0 -> Monday=0 */

    int ndays = days_in_month(year, month);
    double grid_y0 = weekday_y + CAL_WEEKDAY_H;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);

    for (int d = 1; d <= ndays; d++) {
        int idx = first_wday + (d - 1);
        int row = idx / 7;
        int col = idx % 7;
        double cx0 = grid_x0 + col * col_w;
        double cy0 = grid_y0 + row * CAL_ROW_H;

        bool is_today = (d == today);
        if (is_today) {
            double rr  = CAL_ROW_H * 0.38;
            double ccx = cx0 + col_w / 2.0;
            double ccy = cy0 + CAL_ROW_H / 2.0;
            cairo_arc(cr, ccx, ccy, rr, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, TASK_FOCUS_R, TASK_FOCUS_G, TASK_FOCUS_B, TASK_FOCUS_A);
            cairo_fill(cr);
        }

        char num[16];
        snprintf(num, sizeof(num), "%d", d);
        cairo_text_extents(cr, num, &ext);
        double tx = cx0 + (col_w - ext.width) / 2.0 - ext.x_bearing;
        double ty = cy0 + CAL_ROW_H / 2.0 + ext.height / 2.0;

        if (is_today)
            cairo_set_source_rgba(cr, TEXT_FOCUS_R, TEXT_FOCUS_G, TEXT_FOCUS_B, 1.0);
        else
            cairo_set_source_rgba(cr, TEXT_NORM_R, TEXT_NORM_G, TEXT_NORM_B, 1.0);
        cairo_move_to(cr, tx, ty);
        cairo_show_text(cr, num);
    }

    cairo_surface_flush(cs);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);
}

/* Repaints the tooltip surface and pushes it into the scene buffer. */
static void panel_calendar_redraw(struct tinywl_panel *p)
{
    if (!p->cal_text_buf || !p->cal_memdata) return;
    panel_draw_calendar(p);

    struct wl_resource *res = wl_client_get_object(p->wl_client, p->cal_wl_buf_id);
    if (!res) return;

    struct wlr_buffer *wlr_buf = wlr_buffer_try_from_resource(res);
    if (!wlr_buf) return;

    wlr_scene_buffer_set_buffer(p->cal_text_buf, wlr_buf);
    wlr_buffer_unlock(wlr_buf);
}

/* Shows or hides the calendar tooltip. Position is recomputed on every
 * show, right-aligned above the clock area, since it only depends on the
 * (possibly just-resized) output width. */
static void panel_calendar_show(struct tinywl_panel *p, bool show)
{
    if (!p || !p->cal_tree) return;

    if (show) {
        int x = p->out_w - CAL_TOOLTIP_W - PANEL_SPACING;
        if (x < 0) x = 0;
        int y = -CAL_TOOLTIP_H - 4;
        wlr_scene_node_set_position(&p->cal_tree->node, x, y);
        panel_calendar_redraw(p);
    }
    wlr_scene_node_set_enabled(&p->cal_tree->node, show);
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

/* True when (ox, oy) falls over the clock text's exact bounding box, not the wider CLOCK_AREA_W reservation. */
static bool panel_hit_clock(struct tinywl_panel *p, double ox, double oy)
{
    int py = (int)oy - p->panel_y;
    if (py < 0 || py >= PANEL_HEIGHT) return false;
    if (p->clock_hit_w <= 0) return false;
    int px = (int)ox;
    return px >= p->clock_hit_x && px < p->clock_hit_x + p->clock_hit_w;
}

/* Taskbar drag-to-reorder only permutes p->order[]; the tasks[] entries and their listeners never move. */

/* Visual slot currently occupied by physical task index `real`, or -1. */
static int panel_slot_for_real(struct tinywl_panel *p, int real)
{
    for (int s = 0; s < p->n_tasks; s++) {
        if (p->order[s] == real) return s;
    }
    return -1;
}

/* Which visual slot the cursor's x position corresponds to, based on the
 * midpoint of each button — i.e. "insert here" rather than raw hit-test.
 * Falls back to the first/last slot when ox is outside the taskbar. */
static int panel_target_slot_for_x(struct tinywl_panel *p, double ox)
{
    if (p->n_tasks <= 0) return -1;
    int px = (int)ox;
    for (int s = 0; s < p->n_tasks; s++) {
        int real = p->order[s];
        int mid  = p->tasks[real].x + p->tasks[real].w / 2;
        if (px < mid) return s;
    }
    return p->n_tasks - 1;
}

/* Moves the order[] entry at slot `from` to slot `to`, shifting the
 * entries in between. Pure array bookkeeping, no listeners touched. */
static void panel_move_order(struct tinywl_panel *p, int from, int to)
{
    if (from == to || from < 0 || to < 0 ||
        from >= p->n_tasks || to >= p->n_tasks)
        return;

    int moved = p->order[from];
    if (from < to) {
        for (int i = from; i < to; i++) p->order[i] = p->order[i + 1];
    } else {
        for (int i = from; i > to; i--) p->order[i] = p->order[i - 1];
    }
    p->order[to] = moved;
}

/* Keeps order[] consistent with on_unmap()'s physical compaction: drop the removed slot, shift down references above it. */
static void panel_order_remove_physical(struct tinywl_panel *p,
                                          int removed_phys, int old_n_tasks)
{
    int new_len = 0;
    for (int s = 0; s < old_n_tasks; s++) {
        int v = p->order[s];
        if (v == removed_phys) continue;
        if (v > removed_phys) v--;
        p->order[new_len++] = v;
    }
}

/* Input handlers */

static void on_cursor_motion(struct wl_listener *listener, void *data)
{
    struct tinywl_panel *p = wl_container_of(listener, p, cursor_motion);
    (void)data;

    if (p->drag_pending && !p->drag_active) {
        if (fabs(p->server->cursor->x - p->drag_start_x) > DRAG_THRESHOLD_PX)
            p->drag_active = true;
    }

    if (p->drag_active) {
        int target = panel_target_slot_for_x(p, p->server->cursor->x);
        if (target >= 0 && target != p->drag_slot) {
            panel_move_order(p, p->drag_slot, target);
            p->drag_slot = target;
            panel_layout(p);
            panel_redraw(p);
        }
        return;
    }

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

    if (ev->button != BTN_LEFT) return;

    if (ev->state == WLR_BUTTON_PRESSED) {
        /* Arm a potential drag; whether it becomes a reorder or a plain click is decided later (DRAG_THRESHOLD_PX / release). */
        int idx = panel_hit_task(p, p->server->cursor->x, p->server->cursor->y);
        int slot = (idx >= 0) ? panel_slot_for_real(p, idx) : -1;
        p->drag_pending = (slot >= 0);
        p->drag_active  = false;
        p->drag_slot    = slot;
        p->drag_start_x = p->server->cursor->x;
        return;
    }

    /* ev->state == WLR_BUTTON_RELEASED */
    if (p->drag_active) {
        /* Reorder already applied live in on_cursor_motion; just settle. */
        p->drag_active  = false;
        p->drag_pending = false;
        panel_layout(p);
        panel_redraw(p);
        return;
    }
    p->drag_pending = false;

    /* Clicking the clock area toggles the calendar popup open/closed. */
    if (panel_hit_clock(p, p->server->cursor->x, p->server->cursor->y)) {
        p->calendar_open = !p->calendar_open;
        panel_calendar_show(p, p->calendar_open);
        return;
    }

    /* Any other click closes the popup if it's open, but still lets the
     * click act on whatever taskbar button (if any) it landed on below. */
    if (p->calendar_open) {
        p->calendar_open = false;
        panel_calendar_show(p, false);
    }

    int idx = panel_hit_task(p, p->server->cursor->x, p->server->cursor->y);
    if (idx < 0 || idx >= p->n_tasks) return;

    struct tinywl_toplevel *tl = p->tasks[idx].toplevel;
    if (!tl) return;

    if (tl->minimized) {
        /* Minimized window: restore it (re-enable scene node, raise, focus) via the callback stored in the panel. */
        tinywl_panel_restore_toplevel(p, tl);
    } else if (tl == p->focused) {
        /* Clicking the already-focused window's taskbar button minimizes it (toggle behaviour). */
        tinywl_panel_minimize_toplevel(p, tl);
    } else {
        /* Not minimized: go through the same callback restore_toplevel()
         * uses, so this goes through focus_toplevel() too (deactivates the
         * previous window and keeps server->toplevels' focus order correct). */
        tinywl_panel_restore_toplevel(p, tl);
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
    p->server     = server;
    p->memfd      = -1;
    p->hovered    = -1;
    p->cal_memfd  = -1;
    p->drag_slot  = -1;

    for (int i = 0; i < MAX_TASKS; i++) {
        wl_list_init(&p->tasks[i].set_title.link);
        wl_list_init(&p->tasks[i].destroy.link);
        p->tasks[i].panel = p;
        p->order[i] = i;
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

    /* Calendar tooltip — created hidden, shown on clock hover. Reuses the
     * wl_client/wl_shm connection set up by panel_shm_upload() above. */
    p->cal_tree = wlr_scene_tree_create(p->tree);
    if (p->cal_tree) {
        struct wlr_buffer *cal_buf = panel_calendar_shm_upload(p);
        if (cal_buf) {
            panel_draw_calendar(p);
            p->cal_text_buf = wlr_scene_buffer_create(p->cal_tree, cal_buf);
            wlr_buffer_unlock(cal_buf);
        }
        wlr_scene_node_set_enabled(&p->cal_tree->node, false);
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
    t->destroy.notify = on_task_destroy;
    if (toplevel->xdg_toplevel) {
        wl_signal_add(&toplevel->xdg_toplevel->events.set_title, &t->set_title);
        wl_signal_add(&toplevel->xdg_toplevel->base->events.destroy, &t->destroy);
#ifdef TINYWL_HAS_XWAYLAND
    } else if (toplevel->xwayland_surface) {
        wl_signal_add(&toplevel->xwayland_surface->events.set_title, &t->set_title);
        wl_signal_add(&toplevel->xwayland_surface->events.destroy, &t->destroy);
#endif
    }

    /* New task's physical index is always the current n_tasks; append it
     * as the new rightmost visual slot. */
    p->order[p->n_tasks] = p->n_tasks;

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

    int old_n_tasks = p->n_tasks;

    wl_list_remove(&p->tasks[found].set_title.link);
    wl_list_remove(&p->tasks[found].destroy.link);
    wl_list_init(&p->tasks[found].set_title.link);
    wl_list_init(&p->tasks[found].destroy.link);

    /* 
     * Shift tasks down by re-registering each wl_listener at its new address; 
     * copying the struct would leave the wl_signal pointing at a stale address. 
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
        p->tasks[i].destroy.notify = on_task_destroy;
        if (moved_toplevel->xdg_toplevel) {
            wl_signal_add(&moved_toplevel->xdg_toplevel->events.set_title,
                          &p->tasks[i].set_title);
            wl_signal_add(&moved_toplevel->xdg_toplevel->base->events.destroy,
                          &p->tasks[i].destroy);
#ifdef TINYWL_HAS_XWAYLAND
        } else if (moved_toplevel->xwayland_surface) {
            wl_signal_add(&moved_toplevel->xwayland_surface->events.set_title,
                          &p->tasks[i].set_title);
            wl_signal_add(&moved_toplevel->xwayland_surface->events.destroy,
                          &p->tasks[i].destroy);
#endif
        }

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

    panel_order_remove_physical(p, found, old_n_tasks);

    /* A drag in progress refers to slots/physical indices that this
     * removal may have just invalidated — simplest and safest is to
     * cancel it outright; the user can just start the drag again. */
    p->drag_pending = false;
    p->drag_active  = false;

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

void tinywl_panel_place_node_above(struct tinywl_panel *p,
                                     struct wlr_scene_node *node)
{
    if (!p || !p->tree || !node) return;
    wlr_scene_node_place_above(node, &p->tree->node);
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

    /* Rebuild everything sized against the output resolution at creation time (background rect, tree position, text overlay buffer) after a resize. */
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

    /* The tooltip's screen position depends on out_w; force it closed
     * rather than let it linger at a stale position — the user can just
     * click the clock again to reopen it. */
    p->calendar_open = false;
    panel_calendar_show(p, false);

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
    if (p->cal_wl_buf)       wl_buffer_destroy(p->cal_wl_buf);
    if (p->cal_memdata)      munmap(p->cal_memdata, p->cal_memsize);
    if (p->cal_memfd >= 0)   close(p->cal_memfd);
    if (p->wl_buf)           wl_buffer_destroy(p->wl_buf);
    if (p->shm_ctx.shm)      wl_shm_destroy(p->shm_ctx.shm);
    if (p->shm_ctx.registry) wl_registry_destroy(p->shm_ctx.registry);
    if (p->shm_ctx.display)  wl_display_disconnect(p->shm_ctx.display);
    if (p->wl_client)        wl_client_destroy(p->wl_client);
    if (p->memdata)          munmap(p->memdata, p->memsize);
    if (p->memfd >= 0)       close(p->memfd);

    free(p);
}
