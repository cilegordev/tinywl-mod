#ifndef TINYWL_SERVICES_H
#define TINYWL_SERVICES_H
#include <sys/types.h>
#include <stdbool.h>

struct tinywl_server;
struct wlr_backend;

struct tinywl_services;

struct tinywl_services *tinywl_services_init(struct tinywl_server *server);

void tinywl_services_destroy(struct tinywl_services *svc);

/* Reap only the child PIDs we recorded ourselves (dbus-daemon, polkit-agent,
 * xfsettingsd, gvfsd, etc). Never call waitpid(-1, ...) anywhere else in the
 * compositor: other subsystems (e.g. wlroots' Xwayland) fork and waitpid()
 * their own children, and a blanket wait would steal that reap out from
 * under them, making them think their child died unexpectedly. */
void tinywl_services_try_reap(struct tinywl_services *svc);

/* Register an ad-hoc forked child (e.g. an app launched from the menu, or
 * the Print-key screenshot helper) so tinywl_services_try_reap() will also
 * reap it. Without this, any child forked outside services.c is invisible
 * to the reaper and becomes a permanent zombie once it exits. */
void tinywl_services_track_pid(struct tinywl_services *svc, pid_t pid, const char *name);

/* True if `backend` is (or, wrapped in a multi-backend, contains) a nested X11/Wayland backend rather than real DRM/KMS. */
bool tinywl_backend_is_nested(struct wlr_backend *backend);

#endif
