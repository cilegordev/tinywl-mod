#ifndef TINYWL_SERVICES_H
#define TINYWL_SERVICES_H
#include <sys/types.h>
#include <stdbool.h>

struct tinywl_server;
struct wlr_backend;

struct tinywl_services;

struct tinywl_services *tinywl_services_init(struct tinywl_server *server);

void tinywl_services_destroy(struct tinywl_services *svc);

/* True if `backend` is (or, wrapped in a multi-backend, contains) a nested X11/Wayland backend rather than real DRM/KMS. */
bool tinywl_backend_is_nested(struct wlr_backend *backend);

#endif
