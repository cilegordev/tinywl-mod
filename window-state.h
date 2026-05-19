/*
 * window-state.h – Window state persistence for tinywl
 * 
 * Saves and loads window state (maximized/minimized) and geometry
 * to ~/.config/tinywl/windows.state so windows retain their state
 * across restarts, similar to XFCE.
 */

#ifndef WINDOW_STATE_H
#define WINDOW_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "tinywl.h"

/* Save window state (app_id, window geometry, maximized/minimized status) */
void save_window_state(struct tinywl_toplevel *toplevel);

/* Load window state for a window based on app_id */
bool load_window_state(struct tinywl_toplevel *toplevel, const char *app_id);

/* Remove window state from file when window is destroyed */
void remove_window_state(const char *app_id);

/* Initialize the window state system (create config dir if needed) */
void init_window_state_system(void);

/* Clear all saved window states */
void clear_all_window_states(void);

#endif
