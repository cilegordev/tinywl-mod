#ifndef TINYWL_SERVICES_H
#define TINYWL_SERVICES_H

/*
 * services.h – Integration of system services required for a usable desktop:
 *   • D-Bus session bus  (needed by virtually every desktop app)
 *   • XWayland           (X11 app compatibility)
 *   • GVFS daemon        (virtual filesystem / external drive mounting)
 *   • Polkit agent       (privilege escalation dialogs)
 *   • PulseAudio         (audio, started if pipewire-pulse is absent)
 *
 * Forward declaration only; tinywl.h is not included here to keep
 * this header lightweight.
 */

#include <sys/types.h>
#include <stdbool.h>

struct tinywl_server;

/*
 * Opaque handle returned by tinywl_services_init.
 * Keeps track of all child PIDs so they can be reaped on shutdown.
 */
struct tinywl_services;

/*
 * tinywl_services_init – Start all background services.
 *
 * Must be called AFTER:
 *   setenv("WAYLAND_DISPLAY", ...)   – Wayland socket is known
 *   wlr_backend_start()              – DRM master / outputs are up
 *
 * If xwayland is non-NULL, it must already be initialised by wlr_xwayland_create().
 * The function sets DISPLAY from wlr_xwayland->display_name before launching
 * services that need it (e.g. settings daemons built against X11).
 *
 * Returns an allocated tinywl_services handle on success, NULL on fatal error.
 * Individual service failures are non-fatal and are logged with wlr_log.
 */
struct tinywl_services *tinywl_services_init(struct tinywl_server *server);

/*
 * tinywl_services_destroy – Kill all managed child processes and free memory.
 * Safe to call with NULL.
 */
void tinywl_services_destroy(struct tinywl_services *svc);

#endif /* TINYWL_SERVICES_H */
