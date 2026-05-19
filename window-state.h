#ifndef WINDOW_STATE_H
#define WINDOW_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "tinywl.h"

void save_window_state(struct tinywl_toplevel *toplevel);

bool load_window_state(struct tinywl_toplevel *toplevel, const char *app_id);

void remove_window_state(const char *app_id);

void init_window_state_system(void);

void clear_all_window_states(void);

#endif
