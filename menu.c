/*
 * menu.c – Right-click popup menu for TinyWL (wlroots 0.17.4)
 *
 * Rendering:
 *   - wlr_scene_rect  : row backgrounds (proven stable)
 *   - wlr_scene_buffer: text overlay (Cairo → wl_shm → wlr_buffer_try_from_resource)
 *
 * Creates wlr_buffer via internal Wayland client WITHOUT blocking:
 *   - No wl_display_roundtrip() → no deadlock
 *   - All operations are non-blocking:
 *     wl_display_flush() + wl_event_loop_dispatch(loop, 0) + dispatch_pending()
 *
 * If wlr_scene_buffer creation fails, the menu still displays using rects only.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "menu.h"
#include "tinywl.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <linux/input-event-codes.h>
#include <drm/drm_fourcc.h>

#include <wayland-client.h>
#include <wlr/types/wlr_buffer.h>

/* wlr_buffer_try_from_resource: available in wlroots 0.17.4 */
struct wlr_buffer *wlr_buffer_try_from_resource(struct wl_resource *resource);

/* Colors matching system.twmrc like */
static const float C_BG[4]     = { 0x22/255.f, 0xAA/255.f, 0x99/255.f, 1.f };
static const float C_TITLE[4]  = { 0.70f,      0.70f,      0.70f,      1.f };
static const float C_BORDER[4] = { 0x70/255.f, 0x80/255.f, 0x90/255.f, 1.f };
static const float C_HOVER[4]  = { 0x44/255.f, 0xCC/255.f, 0xBB/255.f, 1.f };
static const float C_SHADOW[4] = { 0.f,        0.f,        0.f,        0.4f};
#define C_FG    0.85
#define C_TFG_R (0x22/255.0)
#define C_TFG_G (0xAA/255.0)
#define C_TFG_B (0x99/255.0)
#define C_HOV_R (0x44/255.0)
#define C_HOV_G (0xCC/255.0)
#define C_HOV_B (0xBB/255.0)

#define MENU_FONT "Sans Bold 11"
#define ITEM_W   180
#define ITEM_H    24
#define TITLE_H   22
#define BORDER     1
#define SHADOW_O   3
#define PAD_X     10

/* Menu items */
typedef enum { ITEM_TITLE, ITEM_EXEC, ITEM_QUIT } ItemType;
typedef struct { ItemType type; const char *label; const char *cmd; } Item;

static const Item ITEMS[] = {
    { ITEM_TITLE, "Menu",         NULL             },
    { ITEM_EXEC,  "Terminal",     "xfce4-terminal" },
    { ITEM_EXEC,  "File Manager", "thunar"         },
    { ITEM_EXEC,  "Browser",      "firefox-esr"    },
    { ITEM_QUIT,  "Exit",         NULL             },
};
#define N_ITEMS ((int)(sizeof(ITEMS)/sizeof(ITEMS[0])))

static inline int row_h(int i)
{ return ITEMS[i].type==ITEM_TITLE ? TITLE_H : ITEM_H; }
static int row_y(int i)
{ int y=BORDER; for(int j=0;j<i;j++) y+=row_h(j); return y; }

/* Internal wl_shm client (non-blocking) */
struct shm_ctx {
    struct wl_display  *display;
    struct wl_registry *registry;
    struct wl_shm      *shm;
    bool                ready;
};

static void reg_global(void *data, struct wl_registry *reg,
                        uint32_t name, const char *iface, uint32_t ver)
{
    struct shm_ctx *ctx = data;
    if (strcmp(iface, "wl_shm") == 0) {
        ctx->shm   = wl_registry_bind(reg, name, &wl_shm_interface,
                                       ver < 1 ? 1 : ver);
        ctx->ready = true;
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d;(void)r;(void)n; }
static const struct wl_registry_listener REG_L = {reg_global, reg_remove};

/* Menu state */
struct tinywl_menu {
    struct tinywl_server *server;

    /* Background rects (always visible) */
    struct wlr_scene_tree   *tree;
    struct wlr_scene_rect   *shadow;
    struct wlr_scene_rect   *border;
    struct wlr_scene_rect   *rows[N_ITEMS];

    /* Text overlay via wlr_scene_buffer */
    struct wlr_scene_buffer *text_buf;

    /* Internal Wayland client resources */
    struct wl_client   *wl_client;
    struct shm_ctx      shm_ctx;
    struct wl_buffer   *wl_buf;
    uint32_t            wl_buf_id;

    /* Shared memory */
    int    memfd;
    void  *memdata;
    size_t memsize;
    int    stride;

    int  total_w, total_h;
    int  hovered;
    bool visible;
    int  mx, my;
    uint32_t open_msec;

    struct wl_listener btn;
    struct wl_listener mot;
};

/* Helpers */
static void do_exec(const char *c) {
    if (!c) return;
    pid_t p = fork();
    if (p==0){setsid();execl("/bin/sh","/bin/sh","-c",c,(char*)NULL);_exit(127);}
}
static inline int clampi(int v,int lo,int hi){return v<lo?lo:v>hi?hi:v;}

/* Cairo render text (transparent) into memdata */
static void cairo_draw_text(struct tinywl_menu *m) {
    if (!m->memdata) return;
    int W=m->total_w, H=m->total_h, S=m->stride;

    /* Clear to transparent */
    memset(m->memdata, 0, m->memsize);

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        (uint8_t*)m->memdata, CAIRO_FORMAT_ARGB32, W, H, S);
    cairo_t *cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr,0,0,0,0); cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    PangoLayout *lo = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(MENU_FONT);
    pango_layout_set_font_description(lo, fd); pango_font_description_free(fd);

    for (int i=0; i<N_ITEMS; i++) {
        pango_layout_set_text(lo, ITEMS[i].label, -1);
        int tw, th; pango_layout_get_pixel_size(lo, &tw, &th);
        int iy=row_y(i), ih=row_h(i), iw=W-BORDER*2;
        double ty=iy+(ih-th)/2.0, tx;
        if (ITEMS[i].type==ITEM_TITLE) {
            tx=BORDER+(iw-tw)/2.0;
            cairo_set_source_rgb(cr, C_TFG_R, C_TFG_G, C_TFG_B);
        } else {
            tx=BORDER+PAD_X;
            cairo_set_source_rgb(cr, C_FG, C_FG, C_FG);
        }
        cairo_move_to(cr, tx, ty);
        pango_cairo_show_layout(cr, lo);
    }
    g_object_unref(lo);
    cairo_surface_flush(cs); cairo_destroy(cr); cairo_surface_destroy(cs);
}

/* Create wlr_scene_buffer from wl_shm (non-blocking) */
static bool text_buf_create(struct tinywl_menu *m) {
    /* 1. Allocate memfd + mmap */
    m->stride  = m->total_w * 4;
    m->memsize = (size_t)(m->stride * m->total_h);
    m->memfd   = memfd_create("menu-text", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (m->memfd < 0) return false;
    if (ftruncate(m->memfd, (off_t)m->memsize) < 0) {
        close(m->memfd); m->memfd=-1; return false;
    }
    m->memdata = mmap(NULL, m->memsize, PROT_READ|PROT_WRITE,
                      MAP_SHARED, m->memfd, 0);
    if (m->memdata==MAP_FAILED) {
        m->memdata=NULL; close(m->memfd); m->memfd=-1; return false;
    }

    /* 2. Cairo render text */
    cairo_draw_text(m);

    /* 3. Create internal Wayland client via socketpair */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, sv) < 0) return false;

    m->wl_client = wl_client_create(m->server->wl_display, sv[0]);
    if (!m->wl_client) { close(sv[0]); close(sv[1]); return false; }

    /* 4. Connect client side (non-blocking from here) */
    m->shm_ctx.display = wl_display_connect_to_fd(sv[1]);
    if (!m->shm_ctx.display) {
        wl_client_destroy(m->wl_client); m->wl_client=NULL; return false;
    }

    /* 5. Get registry and bind wl_shm */
    m->shm_ctx.registry = wl_display_get_registry(m->shm_ctx.display);
    m->shm_ctx.ready    = false;
    wl_registry_add_listener(m->shm_ctx.registry, &REG_L, &m->shm_ctx);

    struct wl_event_loop *loop =
        wl_display_get_event_loop(m->server->wl_display);

    /*
     * Dispatch loop using correct pattern:
     * 1. Flush client → server
     * 2. Dispatch compositor (process requests, send responses)
     * 3. Flush server → client
     * 4. Read client socket (poll 1ms) + dispatch pending
     * Repeat until wl_shm is found, max 50 iterations.
     */
    for (int i = 0; i < 50 && !m->shm_ctx.ready; i++) {
        /* Flush client requests to compositor */
        wl_display_flush(m->shm_ctx.display);

        /* Process in compositor and send responses */
        wl_display_flush_clients(m->server->wl_display);
        wl_event_loop_dispatch(loop, 1);
        wl_display_flush_clients(m->server->wl_display);

        /* Read events from socket into client buffer */
        if (wl_display_prepare_read(m->shm_ctx.display) == 0) {
            struct pollfd pfd = {
                .fd      = wl_display_get_fd(m->shm_ctx.display),
                .events  = POLLIN,
                .revents = 0,
            };
            if (poll(&pfd, 1, 1) > 0 && (pfd.revents & POLLIN))
                wl_display_read_events(m->shm_ctx.display);
            else
                wl_display_cancel_read(m->shm_ctx.display);
        }

        /* Process already-read events */
        wl_display_dispatch_pending(m->shm_ctx.display);
    }

    if (!m->shm_ctx.shm) {
        wlr_log(WLR_ERROR, "menu: wl_shm not found after 50 iterations");
        return false;
    }
    wlr_log(WLR_DEBUG, "menu: wl_shm OK");

    /* 6. Create wl_shm_pool + wl_buffer */
    struct wl_shm_pool *pool = wl_shm_create_pool(
        m->shm_ctx.shm, m->memfd, (int32_t)m->memsize);
    if (!pool) return false;

    m->wl_buf = wl_shm_pool_create_buffer(pool, 0,
        m->total_w, m->total_h, m->stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!m->wl_buf) return false;

    /* Get wl_buffer object ID */
    m->wl_buf_id = wl_proxy_get_id((struct wl_proxy*)m->wl_buf);

    /* 7. Flush + dispatch so compositor knows about the buffer */
    wl_display_flush(m->shm_ctx.display);
    wl_display_flush_clients(m->server->wl_display);
    wl_event_loop_dispatch(loop, 0);
    wl_display_flush_clients(m->server->wl_display);
    wl_display_dispatch_pending(m->shm_ctx.display);

    /* 8. Access wl_resource from compositor side */
    struct wl_resource *res =
        wl_client_get_object(m->wl_client, m->wl_buf_id);
    if (!res) {
        wlr_log(WLR_ERROR, "menu: wl_resource id=%u not found",
                m->wl_buf_id);
        return false;
    }

    /* 9. Convert to wlr_buffer */
    struct wlr_buffer *wlr_buf = wlr_buffer_try_from_resource(res);
    if (!wlr_buf) {
        wlr_log(WLR_ERROR, "menu: wlr_buffer_try_from_resource failed");
        return false;
    }

    /* 10. Create wlr_scene_buffer — above all rects */
    m->text_buf = wlr_scene_buffer_create(m->tree, wlr_buf);
    if (!m->text_buf) {
        wlr_log(WLR_ERROR, "menu: wlr_scene_buffer_create failed");
        return false;
    }

    wlr_scene_node_set_position(&m->text_buf->node, 0, 0);
    wlr_log(WLR_INFO, "menu: text overlay %dx%d OK", m->total_w, m->total_h);
    return true;
}

/* Clean up internal client */
static void client_cleanup(struct tinywl_menu *m) {
    if (m->wl_buf)          { wl_buffer_destroy(m->wl_buf);           m->wl_buf=NULL; }
    if (m->shm_ctx.shm)     { wl_shm_destroy(m->shm_ctx.shm);         m->shm_ctx.shm=NULL; }
    if (m->shm_ctx.registry){ wl_registry_destroy(m->shm_ctx.registry);m->shm_ctx.registry=NULL; }
    if (m->shm_ctx.display) { wl_display_disconnect(m->shm_ctx.display);m->shm_ctx.display=NULL; }
    if (m->wl_client)       { wl_client_destroy(m->wl_client);         m->wl_client=NULL; }
    if (m->memdata)         { munmap(m->memdata, m->memsize);           m->memdata=NULL; }
    if (m->memfd >= 0)      { close(m->memfd);                         m->memfd=-1; }
}

/* Scene nodes */
static void nodes_destroy(struct tinywl_menu *m) {
    client_cleanup(m);
    m->text_buf = NULL;
    if (!m->tree) return;
    wlr_scene_node_destroy(&m->tree->node);
    m->tree=NULL; m->shadow=NULL; m->border=NULL;
    for (int i=0; i<N_ITEMS; i++) m->rows[i]=NULL;
}

static void nodes_create(struct tinywl_menu *m) {
    int h=BORDER; for(int i=0;i<N_ITEMS;i++) h+=row_h(i); h+=BORDER;
    m->total_w=ITEM_W+BORDER*2;
    m->total_h=h;
    m->memfd=-1;

    m->tree = wlr_scene_tree_create(&m->server->scene->tree);
    if (!m->tree) { wlr_log(WLR_ERROR,"menu: tree creation failed"); return; }

    /* Hide until position is set in tinywl_menu_show */
    wlr_scene_node_set_enabled(&m->tree->node, false);

    /* Shadow */
    m->shadow = wlr_scene_rect_create(m->tree, m->total_w, m->total_h, C_SHADOW);
    if (m->shadow)
        wlr_scene_node_set_position(&m->shadow->node, SHADOW_O, SHADOW_O);

    /* Border */
    m->border = wlr_scene_rect_create(m->tree, m->total_w, m->total_h, C_BORDER);

    /* Row backgrounds */
    for (int i=0; i<N_ITEMS; i++) {
        const float *col = ITEMS[i].type==ITEM_TITLE ? C_TITLE : C_BG;
        m->rows[i] = wlr_scene_rect_create(m->tree, ITEM_W, row_h(i), col);
        if (m->rows[i])
            wlr_scene_node_set_position(&m->rows[i]->node, BORDER, row_y(i));
    }

    wlr_log(WLR_DEBUG, "menu: nodes created %dx%d", m->total_w, m->total_h);

    /* Add text overlay (optional — if it fails, rects still display) */
    if (!text_buf_create(m))
        wlr_log(WLR_ERROR, "menu: text overlay failed, displaying without text");
}

/* Update hover */
static void row_recolor(struct tinywl_menu *m, int i) {
    if (!m->rows[i]) return;
    const float *col;
    if      (ITEMS[i].type==ITEM_TITLE) col=C_TITLE;
    else if (i==m->hovered)             col=C_HOVER;
    else                                col=C_BG;
    wlr_scene_rect_set_color(m->rows[i], col);
}

static void text_update_hover(struct tinywl_menu *m) {
    if (!m->text_buf || !m->memdata || !m->wl_client) return;
    cairo_draw_text(m);
    struct wl_resource *res = wl_client_get_object(m->wl_client, m->wl_buf_id);
    if (!res) return;
    struct wlr_buffer *buf = wlr_buffer_try_from_resource(res);
    if (buf) wlr_scene_buffer_set_buffer(m->text_buf, buf);
}

/* Hit test / close / activate */
static int menu_hit(const struct tinywl_menu *m, int rx, int ry) {
    if (rx<BORDER || rx>=m->total_w-BORDER) return -1;
    for (int i=0; i<N_ITEMS; i++) {
        int y0=row_y(i), y1=y0+row_h(i);
        if (ry>=y0 && ry<y1) return i;
    }
    return -1;
}

static void menu_close(struct tinywl_menu *m) {
    nodes_destroy(m);
    m->visible=false; m->hovered=-1;
    m->total_w=0; m->total_h=0;
}

static void menu_activate(struct tinywl_menu *m, int idx) {
    if (idx<0||idx>=N_ITEMS) return;
    menu_close(m);
    switch (ITEMS[idx].type) {
    case ITEM_TITLE: break;
    case ITEM_EXEC:
        wlr_log(WLR_INFO,"menu: exec '%s'",ITEMS[idx].cmd);
        do_exec(ITEMS[idx].cmd); break;
    case ITEM_QUIT:
        wlr_log(WLR_INFO,"menu: Exit");
        wl_display_terminate(m->server->wl_display); break;
    }
}

/* Listeners */
static void on_btn(struct wl_listener *l, void *data) {
    struct tinywl_menu *m=wl_container_of(l,m,btn);
    struct wlr_pointer_button_event *ev=data;
    if (!m->visible) return;
    if (ev->state!=WLR_BUTTON_PRESSED) return;
    if (ev->time_msec==m->open_msec) return;
    int rx=(int)m->server->cursor->x-m->mx;
    int ry=(int)m->server->cursor->y-m->my;
    int idx=menu_hit(m,rx,ry);
    if (idx>=0) menu_activate(m,idx);
    else        menu_close(m);
}

static void on_mot(struct wl_listener *l, void *data) {
    struct tinywl_menu *m=wl_container_of(l,m,mot);
    (void)data;
    if (!m->visible) return;
    int rx=(int)m->server->cursor->x-m->mx;
    int ry=(int)m->server->cursor->y-m->my;
    int hov=menu_hit(m,rx,ry);
    if (hov>=0&&ITEMS[hov].type==ITEM_TITLE) hov=-1;
    if (hov==m->hovered) return;
    int old=m->hovered;
    m->hovered=hov;
    if (old>=0) row_recolor(m,old);
    if (hov>=0) row_recolor(m,hov);
    text_update_hover(m);
}

/* Public API */

struct tinywl_menu *tinywl_menu_init(struct tinywl_server *server) {
    struct tinywl_menu *m=calloc(1,sizeof(*m));
    if (!m) return NULL;
    m->server=server; m->visible=false; m->hovered=-1; m->memfd=-1;
    m->btn.notify=on_btn; wl_signal_add(&server->cursor->events.button,&m->btn);
    m->mot.notify=on_mot; wl_signal_add(&server->cursor->events.motion,&m->mot);
    wlr_log(WLR_INFO,"menu: init OK");
    return m;
}

bool tinywl_menu_is_visible(struct tinywl_menu *menu)
{ return menu && menu->visible; }

void tinywl_menu_show(struct tinywl_menu *menu, int x, int y, uint32_t time_msec) {
    if (!menu) return;
    nodes_destroy(menu);
    menu->hovered=-1; menu->visible=false;
    menu->total_w=0; menu->total_h=0;
    menu->open_msec=time_msec;

    nodes_create(menu);
    if (!menu->tree) return;

    struct wlr_box lb;
    wlr_output_layout_get_box(menu->server->output_layout,NULL,&lb);
    menu->mx=clampi(x,lb.x,lb.x+lb.width -menu->total_w-SHADOW_O);
    menu->my=clampi(y,lb.y,lb.y+lb.height-menu->total_h-SHADOW_O);

    wlr_scene_node_set_position(&menu->tree->node,menu->mx,menu->my);
    wlr_scene_node_set_enabled(&menu->tree->node, true);
    menu->visible=true;

    wlr_log(WLR_INFO,"menu: show (%d,%d) %dx%d text=%s",
            menu->mx,menu->my,menu->total_w,menu->total_h,
            menu->text_buf?"OK":"none");
}

void tinywl_menu_hide(struct tinywl_menu *menu)
{ if (menu) menu_close(menu); }

void tinywl_menu_destroy(struct tinywl_menu *menu) {
    if (!menu) return;
    menu_close(menu);
    wl_list_remove(&menu->btn.link);
    wl_list_remove(&menu->mot.link);
    free(menu);
}
