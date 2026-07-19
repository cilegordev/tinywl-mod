#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "tinywl.h"
#include "window-state.h"

#define STATE_DIR ".config/tinywl"
#define STATE_FILE "windows.state"

static char* get_state_file_path(void) {
	static char path[512] = {0};
	if (path[0] != '\0') {
		return path;
	}
	
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	
	snprintf(path, sizeof(path), "%s/%s/%s", home, STATE_DIR, STATE_FILE);
	return path;
}

static char* get_state_dir_path(void) {
	static char path[512] = {0};
	if (path[0] != '\0') {
		return path;
	}
	
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	
	snprintf(path, sizeof(path), "%s/%s", home, STATE_DIR);
	return path;
}

void init_window_state_system(void) {
	const char *state_dir = get_state_dir_path();
	mkdir(state_dir, 0755);
}

void save_window_state(struct tinywl_toplevel *toplevel) {
	if (!toplevel || !toplevel->xdg_toplevel) {
		return;
	}
	
	const char *app_id = toplevel->xdg_toplevel->app_id;
	if (!app_id) {
		app_id = "unknown";
	}
	
	/* Get current geometry */
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);

	int x = toplevel->scene_tree->node.x;
	int y = toplevel->scene_tree->node.y;
	int width = geo.width;
	int height = geo.height;
	bool maximized = toplevel->maximized;
	bool minimized = toplevel->minimized;
	bool fullscreen = toplevel->fullscreen;

	/* Called after unmap, when geometry can legitimately read back as 0x0; 
	skip saving rather than overwrite a valid saved size with garbage. */
	if (width <= 0 || height <= 0) {
		return;
	}
	
	const char *state_file = get_state_file_path();
	
	/* Read existing states and update/add the entry */
	FILE *f = fopen(state_file, "r");
	FILE *temp = NULL;
	char temp_path[520];
	snprintf(temp_path, sizeof(temp_path), "%.512s.tmp", state_file);
	temp = fopen(temp_path, "w");
	
	if (!temp) {
		if (f) fclose(f);
		return;
	}
	
	bool found = false;
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			char stored_app_id[128];
			if (sscanf(line, "%127[^|]", stored_app_id) == 1) {
				if (strcmp(stored_app_id, app_id) == 0) {
					/* Update existing entry */
					fprintf(temp, "%s|%d|%d|%d|%d|%d|%d|%d\n",
						app_id, maximized, x, y, width, height, minimized, fullscreen);
					found = true;
				} else {
					/* Keep other entries */
					fputs(line, temp);
				}
			}
		}
		fclose(f);
	}
	
	/* Add new entry if not found */
	if (!found) {
		fprintf(temp, "%s|%d|%d|%d|%d|%d|%d|%d\n",
			app_id, maximized, x, y, width, height, minimized, fullscreen);
	}
	
	fclose(temp);
	
	/* Replace original file */
	rename(temp_path, state_file);
}

bool load_window_state(struct tinywl_toplevel *toplevel, const char *app_id) {
	if (!toplevel || !app_id) {
		return false;
	}
	
	const char *state_file = get_state_file_path();
	FILE *f = fopen(state_file, "r");
	if (!f) {
		return false;
	}
	
	bool found = false;
	char line[256];
	
	while (fgets(line, sizeof(line), f)) {
		char stored_app_id[128];
		int maximized, x, y, width, height, minimized, fullscreen;
		
		/* Try reading 8 fields first (with fullscreen) */
		int result = sscanf(line, "%127[^|]|%d|%d|%d|%d|%d|%d|%d",
				stored_app_id, &maximized, &x, &y, &width, &height, &minimized, &fullscreen);
		
		/* If that fails, try reading 7 fields (old format without fullscreen) */
		if (result != 8) {
			result = sscanf(line, "%127[^|]|%d|%d|%d|%d|%d|%d",
					stored_app_id, &maximized, &x, &y, &width, &height, &minimized);
			fullscreen = 0; /* Default to not fullscreen for old files */
		}
		
		if (result >= 7) {
			if (strcmp(stored_app_id, app_id) == 0) {
				/* Found matching entry */
				toplevel->maximized = (bool)maximized;
				toplevel->minimized = (bool)minimized;
				toplevel->fullscreen = (bool)fullscreen;
				
				/* Save geometry for restore */
				toplevel->saved_geometry.x = x;
				toplevel->saved_geometry.y = y;
				toplevel->saved_geometry.width = width;
				toplevel->saved_geometry.height = height;
				
				found = true;
				break;
			}
		}
	}
	
	fclose(f);
	return found;
}

void remove_window_state(const char *app_id) {
	if (!app_id) {
		return;
	}
	
	const char *state_file = get_state_file_path();
	FILE *f = fopen(state_file, "r");
	if (!f) {
		return;
	}
	
	char temp_path[520];
	snprintf(temp_path, sizeof(temp_path), "%.512s.tmp", state_file);
	FILE *temp = fopen(temp_path, "w");
	
	if (!temp) {
		fclose(f);
		return;
	}
	
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char stored_app_id[128];
		if (sscanf(line, "%127[^|]", stored_app_id) == 1) {
			if (strcmp(stored_app_id, app_id) != 0) {
				/* Keep entries that don't match */
				fputs(line, temp);
			}
		}
	}
	
	fclose(f);
	fclose(temp);
	rename(temp_path, state_file);
}

void clear_all_window_states(void) {
	const char *state_file = get_state_file_path();
	unlink(state_file);
}
