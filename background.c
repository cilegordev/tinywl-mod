/* 
 * background.c: wallpaper rendering (PNG -> Cairo -> wl_shm -> wlr_scene_buffer), 
 * falls back to a solid grey rect. 
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "background.h"
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
#include <wayland-client.h>
#include <wayland-server-core.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>

/* wlr_buffer_try_from_resource: available since wlroots 0.17.4 */
struct wlr_buffer *wlr_buffer_try_from_resource(struct wl_resource *resource);

/* Internal wl_shm client context */

struct bg_shm_ctx {
    struct wl_display  *display;   /* client-side connection */
    struct wl_registry *registry;
    struct wl_shm      *shm;
    bool                ready;     /* set true once wl_shm is bound */
};

static void shm_registry_global(void *data, struct wl_registry *reg,
                                 uint32_t name, const char *iface,
                                 uint32_t version)
{
    struct bg_shm_ctx *ctx = data;
    if (strcmp(iface, "wl_shm") == 0) {
        ctx->shm   = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        ctx->ready = true;
    }
}

static void shm_registry_global_remove(void *data, struct wl_registry *reg,
                                        uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener shm_registry_listener = {
    .global        = shm_registry_global,
    .global_remove = shm_registry_global_remove,
};

/* Public handle */

struct tinywl_background {
    /* Exactly one of these is non-NULL depending on whether the image
     * loaded successfully. */
    struct wlr_scene_buffer *scene_buf;  /* success path */
    struct wlr_scene_rect   *scene_rect; /* fallback path */

    /* Internal Wayland client (server side) */
    struct wl_client        *wl_client;

    /* Internal Wayland client (client side) */
    struct bg_shm_ctx        shm_ctx;

    /* wl_shm resources */
    struct wl_buffer        *wl_buf;
    uint32_t                 wl_buf_id;

    /* Shared memory */
    int                      memfd;
    void                    *memdata;
    size_t                   memsize;
};

/* Helpers */

/* create_scaled_surface: decode a PNG and scale it to (out_w x out_h); returns a CAIRO_FORMAT_ARGB32 surface owned by the caller. */
static cairo_surface_t *create_scaled_surface(const char *path,
                                               int out_w, int out_h)
{
    cairo_surface_t *src = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
        wlr_log(WLR_ERROR, "background: failed to load PNG '%s': %s",
                path, cairo_status_to_string(cairo_surface_status(src)));
        cairo_surface_destroy(src);
        return NULL;
    }

    int src_w = cairo_image_surface_get_width(src);
    int src_h = cairo_image_surface_get_height(src);
    wlr_log(WLR_DEBUG, "background: loaded '%s' (%dx%d), scaling to %dx%d",
            path, src_w, src_h, out_w, out_h);

    cairo_surface_t *dst = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, out_w, out_h);
    if (cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
        wlr_log(WLR_ERROR, "background: failed to create destination surface");
        cairo_surface_destroy(src);
        cairo_surface_destroy(dst);
        return NULL;
    }

    cairo_t *cr = cairo_create(dst);
    cairo_scale(cr, (double)out_w / src_w, (double)out_h / src_h);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(src);

    if (cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
        wlr_log(WLR_ERROR, "background: scaled surface is invalid");
        cairo_surface_destroy(dst);
        return NULL;
    }

    return dst;
}

/* upload_via_shm: upload pixel data through an internal wl_shm client (socketpair + wl_client_create), same non-blocking pattern as menu.c. */
static struct wlr_buffer *upload_via_shm(struct tinywl_background *bg,
                                          struct tinywl_server *server,
                                          const uint8_t *pixels,
                                          int width, int height, int stride)
{
    int sv[2];

    /* 1. Allocate memfd and copy pixels */
    bg->memsize = (size_t)stride * (size_t)height;

    bg->memfd = memfd_create("tinywl-background", MFD_CLOEXEC);
    if (bg->memfd < 0) {
        wlr_log_errno(WLR_ERROR, "background: memfd_create failed");
        return NULL;
    }
    if (ftruncate(bg->memfd, (off_t)bg->memsize) < 0) {
        wlr_log_errno(WLR_ERROR, "background: ftruncate failed");
        close(bg->memfd); bg->memfd = -1;
        return NULL;
    }
    bg->memdata = mmap(NULL, bg->memsize, PROT_READ | PROT_WRITE,
                       MAP_SHARED, bg->memfd, 0);
    if (bg->memdata == MAP_FAILED) {
        wlr_log_errno(WLR_ERROR, "background: mmap failed");
        close(bg->memfd); bg->memfd = -1; bg->memdata = NULL;
        return NULL;
    }
    memcpy(bg->memdata, pixels, bg->memsize);

    /* 2. Create internal client via socketpair — gives us wl_client* directly */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        wlr_log_errno(WLR_ERROR, "background: socketpair failed");
        return NULL;
    }

    bg->wl_client = wl_client_create(server->wl_display, sv[0]);
    if (!bg->wl_client) {
        wlr_log(WLR_ERROR, "background: wl_client_create failed");
        close(sv[0]); close(sv[1]);
        return NULL;
    }

    /* 3. Connect the client side to the other end of the socketpair */
    bg->shm_ctx.display = wl_display_connect_to_fd(sv[1]);
    if (!bg->shm_ctx.display) {
        wlr_log(WLR_ERROR, "background: wl_display_connect_to_fd failed");
        wl_client_destroy(bg->wl_client); bg->wl_client = NULL;
        return NULL;
    }

    /* 4. Bind wl_shm via registry (non-blocking dispatch loop) */
    bg->shm_ctx.registry = wl_display_get_registry(bg->shm_ctx.display);
    bg->shm_ctx.ready    = false;
    wl_registry_add_listener(bg->shm_ctx.registry, &shm_registry_listener,
                             &bg->shm_ctx);

    struct wl_event_loop *loop =
        wl_display_get_event_loop(server->wl_display);

    for (int i = 0; i < 50 && !bg->shm_ctx.ready; i++) {
        wl_display_flush(bg->shm_ctx.display);

        wl_display_flush_clients(server->wl_display);
        wl_event_loop_dispatch(loop, 1);
        wl_display_flush_clients(server->wl_display);

        if (wl_display_prepare_read(bg->shm_ctx.display) == 0) {
            struct pollfd pfd = {
                .fd      = wl_display_get_fd(bg->shm_ctx.display),
                .events  = POLLIN,
                .revents = 0,
            };
            if (poll(&pfd, 1, 1) > 0 && (pfd.revents & POLLIN))
                wl_display_read_events(bg->shm_ctx.display);
            else
                wl_display_cancel_read(bg->shm_ctx.display);
        }
        wl_display_dispatch_pending(bg->shm_ctx.display);
    }

    if (!bg->shm_ctx.shm) {
        wlr_log(WLR_ERROR, "background: wl_shm not found after 50 iterations");
        return NULL;
    }
    wlr_log(WLR_DEBUG, "background: wl_shm bound OK");

    /* 5. Create wl_shm_pool and wl_buffer */
    struct wl_shm_pool *pool = wl_shm_create_pool(bg->shm_ctx.shm,
                                                    bg->memfd,
                                                    (int32_t)bg->memsize);
    if (!pool) {
        wlr_log(WLR_ERROR, "background: wl_shm_create_pool failed");
        return NULL;
    }

    bg->wl_buf = wl_shm_pool_create_buffer(pool, 0,
                                             width, height, stride,
                                             WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!bg->wl_buf) {
        wlr_log(WLR_ERROR, "background: wl_shm_pool_create_buffer failed");
        return NULL;
    }

    bg->wl_buf_id = wl_proxy_get_id((struct wl_proxy *)bg->wl_buf);

    /* 6. Flush so the compositor processes the buffer creation */
    wl_display_flush(bg->shm_ctx.display);
    wl_display_flush_clients(server->wl_display);
    wl_event_loop_dispatch(loop, 0);
    wl_display_flush_clients(server->wl_display);
    wl_display_dispatch_pending(bg->shm_ctx.display);

    /* 7. Obtain the server-side wl_resource via the wl_client* we kept */
    struct wl_resource *res =
        wl_client_get_object(bg->wl_client, bg->wl_buf_id);
    if (!res) {
        wlr_log(WLR_ERROR, "background: wl_resource id=%u not found",
                bg->wl_buf_id);
        return NULL;
    }

    /* 8. Wrap in a wlr_buffer for the scene graph */
    struct wlr_buffer *wlr_buf = wlr_buffer_try_from_resource(res);
    if (!wlr_buf) {
        wlr_log(WLR_ERROR, "background: wlr_buffer_try_from_resource failed");
        return NULL;
    }

    return wlr_buf;
}

/* Public API */

/* background_cleanup_resources: tear down a previous wallpaper generation's scene node/shm client without freeing the struct itself, so it can be reused. */
static void background_cleanup_resources(struct tinywl_background *bg)
{
    if (bg->scene_buf)
        wlr_scene_node_destroy(&bg->scene_buf->node);
    if (bg->scene_rect)
        wlr_scene_node_destroy(&bg->scene_rect->node);
    bg->scene_buf = NULL;
    bg->scene_rect = NULL;

    if (bg->wl_buf)
        wl_buffer_destroy(bg->wl_buf);
    if (bg->shm_ctx.shm)
        wl_shm_destroy(bg->shm_ctx.shm);
    if (bg->shm_ctx.registry)
        wl_registry_destroy(bg->shm_ctx.registry);
    if (bg->shm_ctx.display)
        wl_display_disconnect(bg->shm_ctx.display);
    bg->wl_buf = NULL;
    memset(&bg->shm_ctx, 0, sizeof(bg->shm_ctx));

    if (bg->wl_client)
        wl_client_destroy(bg->wl_client);
    bg->wl_client = NULL;

    if (bg->memdata)
        munmap(bg->memdata, bg->memsize);
    if (bg->memfd >= 0)
        close(bg->memfd);
    bg->memdata = NULL;
    bg->memsize = 0;
    bg->memfd = -1;
}

/* background_regenerate: (re)build the wallpaper or fallback rect sized to the current output; used for initial creation and for resize. */
static void background_regenerate(struct tinywl_background *bg,
                                   struct tinywl_server *server)
{
    /* Determine output resolution */
    int out_w = 1920, out_h = 1080; /* sane defaults if no output yet */
    struct wlr_box out_box = {0};

    if (!wl_list_empty(&server->outputs)) {
        struct tinywl_output *output =
            wl_container_of(server->outputs.next, output, link);
        wlr_output_effective_resolution(output->wlr_output, &out_w, &out_h);
        wlr_output_layout_get_box(server->output_layout,
                                   output->wlr_output, &out_box);
    }

    /* Decode and scale the PNG */
    cairo_surface_t *surf = create_scaled_surface(
        BACKGROUND_IMAGE_PATH, out_w, out_h);
    if (!surf) {
        wlr_log(WLR_INFO,
                "background: PNG load failed, using solid colour fallback");
        goto fallback;
    }

    cairo_surface_flush(surf);
    const uint8_t *pixels = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    /* Upload via wl_shm */
    struct wlr_buffer *wlr_buf =
        upload_via_shm(bg, server, pixels, out_w, out_h, stride);

    cairo_surface_destroy(surf);

    if (!wlr_buf) {
        wlr_log(WLR_INFO,
                "background: shm upload failed, using solid colour fallback");
        goto fallback;
    }

    /* Place the scene buffer at the root of the scene tree so it renders behind everything else (menu, panel, toplevels not yet created). */
    bg->scene_buf = wlr_scene_buffer_create(&server->scene->tree, wlr_buf);
    if (!bg->scene_buf) {
        wlr_log(WLR_ERROR, "background: wlr_scene_buffer_create failed");
        wlr_buffer_unlock(wlr_buf);
        goto fallback;
    }

    wlr_scene_node_set_position(&bg->scene_buf->node,
                                 out_box.x, out_box.y);
    wlr_scene_node_lower_to_bottom(&bg->scene_buf->node);

    wlr_log(WLR_INFO,
            "background: wallpaper '%s' displayed at (%d,%d) %dx%d",
            BACKGROUND_IMAGE_PATH, out_box.x, out_box.y, out_w, out_h);
    return;

fallback:
    {
        /* Catppuccin Mocha base — dark but intentional */
        static const float dark[4] = { 0.118f, 0.118f, 0.180f, 1.0f };
        bg->scene_rect = wlr_scene_rect_create(
            &server->scene->tree, out_w, out_h, dark);
        if (bg->scene_rect) {
            wlr_scene_node_set_position(&bg->scene_rect->node,
                                         out_box.x, out_box.y);
            wlr_scene_node_lower_to_bottom(&bg->scene_rect->node);
        }
        wlr_log(WLR_INFO, "background: solid colour fallback active");
    }
}

struct tinywl_background *tinywl_background_create(struct tinywl_server *server)
{
    struct tinywl_background *bg = calloc(1, sizeof(*bg));
    if (!bg) {
        wlr_log(WLR_ERROR, "background: out of memory");
        return NULL;
    }
    bg->memfd = -1;

    background_regenerate(bg, server);
    return bg;
}

void tinywl_background_resize(struct tinywl_background *bg, struct tinywl_server *server)
{
    if (!bg)
        return;

    background_cleanup_resources(bg);
    bg->memfd = -1;
    background_regenerate(bg, server);
}

void tinywl_background_destroy(struct tinywl_background *bg)
{
    if (!bg)
        return;

    background_cleanup_resources(bg);
    free(bg);
}
