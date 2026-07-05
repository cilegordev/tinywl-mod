#ifndef TINYWL_BACKGROUND_H
#define TINYWL_BACKGROUND_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#ifndef BACKGROUND_IMAGE_PATH
#define BACKGROUND_IMAGE_PATH "/var/rootfs/img0.png"
#endif

struct tinywl_server;
struct tinywl_background;

struct tinywl_background *tinywl_background_create(struct tinywl_server *server);

void tinywl_background_resize(struct tinywl_background *bg, struct tinywl_server *server);

void tinywl_background_destroy(struct tinywl_background *bg);

#ifdef __cplusplus
}
#endif
#endif
