#ifndef TINYWL_SERVICES_H
#define TINYWL_SERVICES_H
#include <sys/types.h>
#include <stdbool.h>

struct tinywl_server;
struct wlr_backend;

struct tinywl_services;

struct tinywl_services *tinywl_services_init(struct tinywl_server *server);

void tinywl_services_destroy(struct tinywl_services *svc);

/*
 * True if `backend` is (or contains, when wrapped in a multi-backend —
 * which wlr_backend_autocreate() always does) a nested backend such as
 * the X11 or Wayland backend, i.e. tinywl is running as an ordinary
 * window inside another already-running desktop session rather than
 * owning real DRM/KMS hardware directly.
 */
bool tinywl_backend_is_nested(struct wlr_backend *backend);

#endif
