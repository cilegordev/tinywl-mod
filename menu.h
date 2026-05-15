#ifndef TINYWL_MENU_H
#define TINYWL_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

struct tinywl_server;
struct tinywl_menu;

struct tinywl_menu *tinywl_menu_init(struct tinywl_server *server);
void tinywl_menu_show(struct tinywl_menu *menu, int x, int y, uint32_t time_msec);
void tinywl_menu_hide(struct tinywl_menu *menu);
bool tinywl_menu_is_visible(struct tinywl_menu *menu);
void tinywl_menu_destroy(struct tinywl_menu *menu);

#ifdef __cplusplus
}
#endif
#endif
