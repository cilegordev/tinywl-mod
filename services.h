#ifndef TINYWL_SERVICES_H
#define TINYWL_SERVICES_H
#include <sys/types.h>
#include <stdbool.h>

struct tinywl_server;

struct tinywl_services;

struct tinywl_services *tinywl_services_init(struct tinywl_server *server);

void tinywl_services_destroy(struct tinywl_services *svc);

#endif
