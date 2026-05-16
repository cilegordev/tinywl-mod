/*
 * background.h - Wallpaper/background image support for TinyWL
 *
 * Loads a PNG image from disk and renders it as a wlr_scene_buffer node
 * placed at the bottom of the scene graph so it appears behind all windows.
 *
 * The image is scaled to fit the first available output using Cairo's
 * bilinear scaling. If the image file cannot be loaded, the background
 * falls back to a solid dark-grey rect so the compositor remains usable.
 *
 * Usage:
 *   1. Call tinywl_background_create() once after the scene is set up but
 *      before wlr_backend_start(), so the node is at the bottom of the
 *      scene graph when the first frame is rendered.
 *   2. Call tinywl_background_destroy() during compositor shutdown, before
 *      wlr_scene_node_destroy().
 *
 * The background is currently static (no hot-reload, no multi-monitor
 * per-output images). Those are left as future extensions.
 */

#ifndef TINYWL_BACKGROUND_H
#define TINYWL_BACKGROUND_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Default wallpaper path.  Override at compile time with
 *   -DBACKGROUND_IMAGE_PATH=\"/path/to/image.png\"
 */
#ifndef BACKGROUND_IMAGE_PATH
#define BACKGROUND_IMAGE_PATH "/var/rootfs/img0.png"
#endif

struct tinywl_server;
struct tinywl_background;

/**
 * tinywl_background_create() - Load and display the wallpaper.
 *
 * @server: pointer to the compositor server struct.
 *
 * Decodes the PNG at BACKGROUND_IMAGE_PATH, scales it to the first
 * output's effective resolution, uploads it through wl_shm, and
 * inserts a wlr_scene_buffer node at the very bottom of the scene tree
 * (below all toplevels and the menu).
 *
 * On failure the function still returns a valid (non-NULL) handle that
 * tinywl_background_destroy() can safely free; a solid dark fallback
 * rect is shown instead.
 *
 * Returns NULL only on fatal allocation failure.
 */
struct tinywl_background *tinywl_background_create(struct tinywl_server *server);

/**
 * tinywl_background_destroy() - Release all background resources.
 *
 * Destroys the scene node, releases the wlr_buffer, unmaps the shared
 * memory, and frees the handle.  Safe to call with NULL.
 */
void tinywl_background_destroy(struct tinywl_background *bg);

#ifdef __cplusplus
}
#endif
#endif
