#ifndef TINYWL_PANEL_H
#define TINYWL_PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

struct tinywl_panel;
struct tinywl_server;
struct tinywl_toplevel;

struct tinywl_panel *tinywl_panel_create(struct tinywl_server *server);

void tinywl_panel_on_map(struct tinywl_panel *p,
                          struct tinywl_toplevel *toplevel);

void tinywl_panel_on_unmap(struct tinywl_panel *p,
                             struct tinywl_toplevel *toplevel);

void tinywl_panel_on_focus(struct tinywl_panel *p,
                             struct tinywl_toplevel *toplevel);

int tinywl_panel_get_height(struct tinywl_panel *p);

void tinywl_panel_raise_to_top(struct tinywl_panel *p);

void tinywl_panel_set_callbacks(struct tinywl_panel *p,
    void (*cb_minimize)(struct tinywl_toplevel *toplevel),
    void (*cb_restore)(struct tinywl_toplevel *toplevel));

void tinywl_panel_restore_toplevel(struct tinywl_panel *p,
                                    struct tinywl_toplevel *toplevel);

void tinywl_panel_minimize_toplevel(struct tinywl_panel *p,
                                     struct tinywl_toplevel *toplevel);

void tinywl_panel_hide(struct tinywl_panel *p);

void tinywl_panel_show(struct tinywl_panel *p);

void tinywl_panel_destroy(struct tinywl_panel *p);

#ifdef __cplusplus
}
#endif
#endif
