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
 * Reap only child PIDs we recorded ourselves; other subsystems 
 * (e.g. wlroots' Xwayland) reap their own, and a blanket wait would steal that from them. 
 */
void tinywl_services_try_reap(struct tinywl_services *svc);

/* 
 * Register an ad-hoc forked child (menu launches, screenshot helper) 
 * so tinywl_services_try_reap() reaps it too, instead of leaving a zombie. 
 */
void tinywl_services_track_pid(struct tinywl_services *svc, pid_t pid, const char *name);

/* 
 * True if `backend` is (or, wrapped in a multi-backend, contains) 
 * a nested X11/Wayland backend rather than real DRM/KMS. 
 */
bool tinywl_backend_is_nested(struct wlr_backend *backend);

#endif
