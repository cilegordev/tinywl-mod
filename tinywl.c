#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "tinywl.h"
#include "menu.h"
#include "background.h"
#include "panel.h"
#include "services.h"
#include "window-state.h"

/* Forward declarations for minimize/restore (called back by panel.c) */
void minimize_toplevel(struct tinywl_toplevel *toplevel);
void restore_toplevel(struct tinywl_toplevel *toplevel);

/* Forward declaration for dialog stacking */
static void raise_dialogs_to_front(struct tinywl_server *server);

/* Forward declaration: needed by handle_drag_destroy before its real definition */
static struct tinywl_toplevel *desktop_toplevel_at(
		struct tinywl_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy);

/* Forward declaration: needed by the xwayland request_move/request_resize
 * handlers, which are defined earlier in the file than begin_interactive. */
static void begin_interactive(struct tinywl_toplevel *toplevel,
		enum tinywl_cursor_mode mode, uint32_t edges);

/* Handle SIGINT/SIGTERM by calling wl_display_terminate() so wl_display_destroy() runs and cleans up the Wayland socket files. */
static int handle_term_signal(int signal_number, void *data) {
	struct wl_display *display = data;
	wlr_log(WLR_INFO, "Received signal %d, shutting down", signal_number);
	fflush(NULL); /* diagnostic: force this line to disk even if something downstream hangs/gets SIGKILLed */
	wl_display_terminate(display);
	return 0;
}

/* Reap zombie children left behind by forked helpers (menu launcher, screenshot tool, startup_cmd). */
static int handle_sigchld(int signal_number, void *data) {
	(void) signal_number;
	struct tinywl_server *server = data;
	/* 
	 * Only reap PIDs we recorded ourselves; never waitpid(-1, ...) here, 
	 * or it races with wlroots' own Xwayland child reaping and breaks XWayland. 
	 */
	if (server)
		tinywl_services_try_reap(server->services);
	return 1;
}

/* Registered with atexit() so the socket/lock files are removed on any exit path. */
static char wayland_socket_path[PATH_MAX];
static char wayland_lock_path[PATH_MAX];

static void cleanup_wayland_socket_files(void) {
	if (wayland_socket_path[0])
		unlink(wayland_socket_path);
	if (wayland_lock_path[0])
		unlink(wayland_lock_path);
}

struct tinywl_popup {
	struct wl_list link;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_tree *scene_tree;
	struct tinywl_server *server;
	int last_geom_x, last_geom_y;
	struct wl_listener map;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void popup_handle_map(struct wl_listener *listener, void *data);

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->map.link);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->link);
	free(popup);
}

/* Walk up to the root-level ancestor scene node that owns `tree`. */
static struct wlr_scene_tree *scene_tree_root_child(
		struct tinywl_server *server, struct wlr_scene_tree *tree) {
	struct wlr_scene_tree *root = &server->scene->tree;
	while (tree && tree->node.parent && tree->node.parent != root) {
		tree = tree->node.parent;
	}
	if (tree && tree->node.parent == root) {
		return tree;
	}
	return NULL;
}

/* Raise a popup's owning toplevel above the panel so context menus near the bottom of the screen aren't hidden behind it. */
static void raise_popup_owner_above_panel(struct tinywl_popup *popup) {
	struct wlr_xdg_surface *parent_surface =
		wlr_xdg_surface_try_from_wlr_surface(popup->xdg_surface->popup->parent);
	if (!parent_surface || !parent_surface->data) {
		return;
	}
	struct wlr_scene_tree *parent_tree = parent_surface->data;
	struct wlr_scene_tree *root_child =
		scene_tree_root_child(popup->server, parent_tree);
	if (root_child) {
		tinywl_panel_place_node_above(popup->server->panel, &root_child->node);
	}
}

static void apply_popup_constraint(struct tinywl_popup *popup) {
	struct wlr_xdg_surface *xdg_surface = popup->xdg_surface;
	struct wlr_xdg_popup *xdg_popup = xdg_surface->popup;
	struct tinywl_server *server = popup->server;
	
	/* Get parent surface */
	struct wlr_xdg_surface *parent_surface = 
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (!parent_surface || !parent_surface->data) {
		return;
	}
	struct wlr_scene_tree *parent_tree = parent_surface->data;
	struct wlr_scene_tree *popup_tree = popup->scene_tree;
	
	/* Get output layout */
	struct wlr_output_layout *layout = server->output_layout;
	struct wlr_box usable_area;
	wlr_output_layout_get_box(layout, NULL, &usable_area);
	
	int popup_width = xdg_popup->current.geometry.width;
	int popup_height = xdg_popup->current.geometry.height;
	int popup_x = xdg_popup->current.geometry.x;
	int popup_y = xdg_popup->current.geometry.y;
	
	/* Calculate absolute position by walking up scene tree hierarchy */
	int abs_x = popup_x;
	int abs_y = popup_y;
	struct wlr_scene_tree *current = parent_tree;
	
	while (current) {
		abs_x += current->node.x;
		abs_y += current->node.y;
		if (!current->node.parent) break;
		current = (struct wlr_scene_tree *)current->node.parent;
	}
	
	int final_x = popup_x;
	int final_y = popup_y;
	
	/* RIGHT boundary */
	if (abs_x + popup_width > usable_area.x + usable_area.width) {
		int max_x = (usable_area.x + usable_area.width) - popup_width - 10;
		final_x = max_x - parent_tree->node.x;
	}
	
	/* LEFT boundary */
	if (abs_x < usable_area.x) {
		int min_x = usable_area.x + 10;
		final_x = min_x - parent_tree->node.x;
	}
	
	/* BOTTOM boundary */
	if (abs_y + popup_height > usable_area.y + usable_area.height) {
		int max_y = (usable_area.y + usable_area.height) - popup_height - 10;
		final_y = max_y - parent_tree->node.y;
	}
	
	/* TOP boundary */
	if (abs_y < usable_area.y) {
		int min_y = usable_area.y + 10;
		final_y = min_y - parent_tree->node.y;
	}
	
	if (final_x != popup_x || final_y != popup_y) {
		/* Update XDG geometry - this is what scene graph uses for rendering */
		xdg_popup->current.geometry.x = final_x;
		xdg_popup->current.geometry.y = final_y;
		
		/* Also update scene node to match */
		wlr_scene_node_set_position(&popup_tree->node, final_x, final_y);
		
		/* Track last constrained position */
		popup->last_geom_x = final_x;
		popup->last_geom_y = final_y;
	} else {
		/* No constraint needed, but track current geometry */
		popup->last_geom_x = popup_x;
		popup->last_geom_y = popup_y;
	}
}

static void popup_handle_reposition(struct wl_listener *listener, void *data) {
	struct tinywl_popup *popup = wl_container_of(listener, popup, commit);
	struct wlr_xdg_popup *xdg_popup = popup->xdg_surface->popup;
	
	/* Check if geometry changed since last constraint */
	if (xdg_popup->current.geometry.x != popup->last_geom_x ||
	    xdg_popup->current.geometry.y != popup->last_geom_y) {
		apply_popup_constraint(popup);
	}

	/* Re-assert the panel z-order fix on every commit, since the panel can be raised again while the popup stays open. */
	raise_popup_owner_above_panel(popup);
}

static void popup_handle_map(struct wl_listener *listener, void *data) {
	struct tinywl_popup *popup = wl_container_of(listener, popup, map);
	apply_popup_constraint(popup);
	raise_popup_owner_above_panel(popup);
}

/* Get the wlr_surface for either an xdg-shell or an XWayland toplevel. */
static struct wlr_surface *toplevel_wlr_surface(struct tinywl_toplevel *toplevel) {
	if (!toplevel) {
		return NULL;
	}
	if (toplevel->xdg_toplevel) {
		return toplevel->xdg_toplevel->base->surface;
	}
	if (toplevel->xwayland_surface) {
		return toplevel->xwayland_surface->surface;
	}
	return NULL;
}

/* Activates/deactivates a toplevel regardless of which protocol backs it. */
static void toplevel_set_activated(struct tinywl_toplevel *toplevel, bool activated) {
	if (!toplevel) {
		return;
	}
	if (toplevel->xdg_toplevel) {
		wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, activated);
	} else if (toplevel->xwayland_surface) {
		wlr_xwayland_surface_activate(toplevel->xwayland_surface, activated);
	}
}

/* 
 * Returns the toplevel's content geometry regardless of protocol. XWayland
 * surfaces have no separate "geometry offset" concept the way xdg-shell
 * does, so it's just (0, 0, width, height). 
 */
static void toplevel_get_geometry(struct tinywl_toplevel *toplevel, struct wlr_box *box) {
	if (toplevel->xdg_toplevel) {
		wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, box);
	} else if (toplevel->xwayland_surface) {
		box->x = 0;
		box->y = 0;
		box->width = toplevel->xwayland_surface->width;
		box->height = toplevel->xwayland_surface->height;
	} else {
		box->x = box->y = box->width = box->height = 0;
	}
}

/* 
 * Resize a toplevel regardless of protocol; assumes the scene node position is already 
 * set (XWayland needs position+size together, xdg-shell only negotiates size). 
 */
static void toplevel_set_size(struct tinywl_toplevel *toplevel, int width, int height) {
	if (width < 1) width = 1;
	if (height < 1) height = 1;
	if (toplevel->xdg_toplevel) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
	} else if (toplevel->xwayland_surface) {
		wlr_xwayland_surface_configure(toplevel->xwayland_surface,
			toplevel->scene_tree->node.x, toplevel->scene_tree->node.y,
			width, height);
	}
}

static void focus_toplevel(struct tinywl_toplevel *toplevel, struct wlr_surface *surface) {
	/* Note: this function only deals with keyboard focus. */
	if (toplevel == NULL) {
		return;
	}
	struct tinywl_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */
		return;
	}
	if (prev_surface) {
		/* Deactivate the previously focused surface so the client repaints without focus. */
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		} else {
			struct wlr_xwayland_surface *prev_xwayland =
				wlr_xwayland_surface_try_from_wlr_surface(prev_surface);
			if (prev_xwayland != NULL) {
				wlr_xwayland_surface_activate(prev_xwayland, false);
			}
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	/* Move the toplevel to the front */
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	/* Keep the panel always on top of every window */
	tinywl_panel_raise_to_top(server->panel);
	
	/* Show/hide panel based on fullscreen state of focused window */
	if (toplevel->fullscreen) {
		tinywl_panel_hide(server->panel);
	} else {
		tinywl_panel_show(server->panel);
	}
	
	/* If dialog is focused, raise all dialogs to ensure they stay on top */
	if (toplevel->is_dialog) {
		raise_dialogs_to_front(server);
	}
	
	/* Activate the new surface */
	toplevel_set_activated(toplevel, true);
	/* Update the taskbar highlight to reflect the newly focused window */
	tinywl_panel_on_focus(server->panel, toplevel);
	/* Give the surface keyboard focus through the seat. */
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, toplevel_wlr_surface(toplevel),
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	/* 
	 * This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. 
	 */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/* A seat has only one keyboard slot; swap the underlying wlr_keyboard as devices change. */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void handle_vt_switch(struct tinywl_server *server, int vt_number) {
	/* Switch VT via ioctl (VT numbers 1-8 map to F1-F8). */
	int fd = -1;
	
	/* Method 1: Try /dev/tty0 (the virtual console multiplexer) */
	fd = open("/dev/tty0", O_RDWR);
	if (fd >= 0) {
		ioctl(fd, VT_ACTIVATE, vt_number);
		ioctl(fd, VT_WAITACTIVE, vt_number);
		close(fd);
		return;
	}
	
	/* Method 2: Try current controlling terminal */
	fd = open("/dev/console", O_RDWR);
	if (fd >= 0) {
		ioctl(fd, VT_ACTIVATE, vt_number);
		ioctl(fd, VT_WAITACTIVE, vt_number);
		close(fd);
		return;
	}
	
	/* Method 3: Try stdin/stdout/stderr if they are connected to a TTY */
	for (int std_fd = STDIN_FILENO; std_fd <= STDERR_FILENO; std_fd++) {
		if (isatty(std_fd)) {
			ioctl(std_fd, VT_ACTIVATE, vt_number);
			ioctl(std_fd, VT_WAITACTIVE, vt_number);
			return;
		}
	}
}

static bool handle_keybinding(struct tinywl_server *server, xkb_keysym_t sym) {
	/* Compositor keybindings, handled while Alt is held. */
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_Tab:
		/* Cycle to the next toplevel */
		if (wl_list_length(&server->toplevels) < 2) {
			break;
		}
		struct tinywl_toplevel *next_toplevel =
			wl_container_of(server->toplevels.prev, next_toplevel, link);
		focus_toplevel(next_toplevel, next_toplevel->xdg_toplevel->base->surface);
		break;
	default:
		return false;
	}
	return true;
}

static void handle_print_key(struct tinywl_server *server) {
	/* Print key: launch xfce4-screenshooter via fork/exec. */
	pid_t pid = fork();
	if (pid == 0) {
		/* Unblock signals inherited from the compositor (see spawn_service()
		 * in services.c for why this matters) before exec. */
		sigset_t empty_mask;
		sigemptyset(&empty_mask);
		sigprocmask(SIG_SETMASK, &empty_mask, NULL);

		/* Child process: exec xfce4-screenshooter */
		execl("/usr/libexec/xfce4/screenshooter/scripts/xfce4-screenshooter",
			"xfce4-screenshooter", (char *)NULL);
		/* If exec fails, exit quietly */
		exit(1);
	}
	/* Parent: track the pid so it gets reaped instead of becoming a zombie. */
	if (pid > 0 && server)
		tinywl_services_track_pid(server->services, pid, "xfce4-screenshooter");
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct tinywl_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	
	/* Check for Print key (screenshot) - no modifiers needed */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (syms[i] == XKB_KEY_Print) {
				handle_print_key(server);
				handled = true;
				break;
			}
		}
	}
	
	/* Check for XF86Switch_VT keysyms (sent by keyboard driver for ALT+CTRL+F1-F8) */
	if (!handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			xkb_keysym_t sym = syms[i];
			/* XF86Switch_VT_1 = 0x1008fe01, XF86Switch_VT_8 = 0x1008fe08 */
			if (sym >= 0x1008fe01 && sym <= 0x1008fe08) {
				int vt_number = sym - 0x1008fe00;  /* Extract VT number from keysym */
				handle_vt_switch(server, vt_number);
				handled = true;
				break;
			}
		}
	}
	
	/* Check for ALT+CTRL+F1-F8 to switch TTY (as fallback for other keyboards) */
	if (!handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		uint32_t alt = modifiers & WLR_MODIFIER_ALT;
		uint32_t ctrl = modifiers & WLR_MODIFIER_CTRL;
		
		if (alt && ctrl) {
			for (int i = 0; i < nsyms; i++) {
				xkb_keysym_t sym = syms[i];
				/* F1 to F8 maps to TTY 1 to 8 */
				if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F8) {
					int vt_number = sym - XKB_KEY_F1 + 1;
					handle_vt_switch(server, vt_number);
					handled = true;
					break;
				}
			}
		}
	}
	
	if (!handled && (modifiers & WLR_MODIFIER_ALT)) {
		/* If alt is held down, check all states (press, repeat, release).
		 * We suppress forwarding for any key that is a compositor keybinding
		 * regardless of state — this prevents Tab spam reaching the client. */
		for (int i = 0; i < nsyms; i++) {
			if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
				handled = handle_keybinding(server, syms[i]);
			} else {
				/* For release/repeat: just suppress without acting */
				xkb_keysym_t sym = syms[i];
				if (sym == XKB_KEY_Tab || sym == XKB_KEY_Escape) {
					handled = true;
				}
			}
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	/* wlr_keyboard is being destroyed. */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct tinywl_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct tinywl_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct tinywl_server *server,
		struct wlr_input_device *device) {
	/* Pointer handling is proxied through wlr_cursor. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In TinyWL we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct tinywl_server *server = wl_container_of(
			listener, server, request_cursor);
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		/* Set the hardware cursor image from the client-provided surface. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	/* Honor the client's selection (clipboard) request. */
	struct tinywl_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_start_drag(struct wl_listener *listener, void *data) {
       /* This event is raised when a client initiates a drag and drop operation.
        * We handle the drag and drop properly by passing the request to wlroots,
        * which manages the actual drag feedback and drop delivery. */
       struct tinywl_server *server = wl_container_of(
                       listener, server, request_start_drag);
       struct wlr_seat_request_start_drag_event *event = data;
       
       /* Validate the drag source – the seat must have pointer focus on the
        * surface trying to start the drag. This prevents rogue clients from
        * spoofing drags. */
       struct wlr_seat *seat = server->seat;
       if (wlr_seat_validate_pointer_grab_serial(seat, event->origin, event->serial)) {
               /* The serial is valid; allow the drag to proceed.
                * wlroots handles cursor feedback, highlight rendering, and drop delivery. */
               wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
       }
       /* If serial is invalid, silently drop the drag request (no harm). */
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
       /* On drag-and-drop, refocus the surface actually dropped onto, not the drag source, so closing the source afterwards doesn't fall back to stale focus. */
       struct tinywl_server *server = wl_container_of(
                       listener, server, drag_destroy);
       wl_list_remove(&server->drag_destroy.link);

       double sx, sy;
       struct wlr_surface *surface = NULL;
       struct tinywl_toplevel *target = desktop_toplevel_at(server,
                       server->cursor->x, server->cursor->y, &surface, &sx, &sy);
       if (target) {
               focus_toplevel(target, surface);
       }
}

static void seat_start_drag(struct wl_listener *listener, void *data) {
       /* Put the drag icon in the scene graph once the drag actually starts. */
       struct tinywl_server *server = wl_container_of(
                       listener, server, start_drag);
       struct wlr_drag *drag = data;

       server->drag_destroy.notify = handle_drag_destroy;
       wl_signal_add(&drag->events.destroy, &server->drag_destroy);

       if (!drag->icon) {
               return;
       }

       wlr_scene_drag_icon_create(server->drag_icon, drag->icon);

       /* Raise the drag icon to the top so it renders above everything else. */
       wlr_scene_node_raise_to_top(&server->drag_icon->node);
       wlr_scene_node_set_position(&server->drag_icon->node,
                       server->cursor->x, server->cursor->y);
}

static struct tinywl_toplevel *desktop_toplevel_at(
		struct tinywl_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* This returns the topmost node in the scene at the given layout coords.
	 * We only care about surface nodes as we are specifically looking for a
	 * surface in the surface tree of a tinywl_toplevel. */
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	/* Walk up to the scene node holding the owning toplevel's data pointer. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	if (tree == NULL) {
		return NULL;
	}
	return tree->node.data;
}

/* Edge-snap tuning: how close the cursor must get to an edge before that zone arms. */
#define SNAP_MARGIN_PX     24

#define SNAP_PREVIEW_R  0.20f
#define SNAP_PREVIEW_G  0.45f
#define SNAP_PREVIEW_B  0.85f
#define SNAP_PREVIEW_A  0.35f

/* Primary output geometry helper, shared by maximize/fullscreen/snap. */
static bool get_primary_output_box(struct tinywl_server *server,
		struct wlr_box *out_box, int *out_w, int *out_h) {
	if (wl_list_empty(&server->outputs)) {
		return false;
	}
	struct tinywl_output *output =
		wl_container_of(server->outputs.next, output, link);
	wlr_output_effective_resolution(output->wlr_output, out_w, out_h);
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, out_box);
	return true;
}

/* Which snap zone (if any) the cursor is currently within, given it's
 * mid-drag moving a window. Only checked against the primary output. */
static tinywl_snap_zone compute_snap_zone(struct tinywl_server *server) {
	struct wlr_box out_box = {0};
	int out_w = 0, out_h = 0;
	if (!get_primary_output_box(server, &out_box, &out_w, &out_h)) {
		return TINYWL_SNAP_NONE;
	}

	double cx = server->cursor->x;
	double cy = server->cursor->y;

	if (cy <= out_box.y + SNAP_MARGIN_PX) {
		return TINYWL_SNAP_TOP;
	}
	if (cx <= out_box.x + SNAP_MARGIN_PX) {
		return TINYWL_SNAP_LEFT;
	}
	if (cx >= out_box.x + out_w - SNAP_MARGIN_PX) {
		return TINYWL_SNAP_RIGHT;
	}
	return TINYWL_SNAP_NONE;
}

/* Target (x, y, width, height) a window should land at for a given snap
 * zone, on the primary output, accounting for the panel's height the same
 * way toggle_maximize() does. Returns false if there's no output. */
static bool snap_zone_box(struct tinywl_server *server, tinywl_snap_zone zone,
		struct wlr_box *box) {
	struct wlr_box out_box = {0};
	int out_w = 0, out_h = 0;
	if (!get_primary_output_box(server, &out_box, &out_w, &out_h)) {
		return false;
	}
	int usable_h = out_h - tinywl_panel_get_height(server->panel);

	switch (zone) {
	case TINYWL_SNAP_LEFT:
		box->x = out_box.x;
		box->y = out_box.y;
		box->width  = out_w / 2;
		box->height = usable_h;
		return true;
	case TINYWL_SNAP_RIGHT:
		box->x = out_box.x + out_w / 2;
		box->y = out_box.y;
		box->width  = out_w - out_w / 2;
		box->height = usable_h;
		return true;
	case TINYWL_SNAP_TOP:
		box->x = out_box.x;
		box->y = out_box.y;
		box->width  = out_w;
		box->height = usable_h;
		return true;
	default:
		return false;
	}
}

/* Shows/repositions or hides the snap preview rect to match `zone`
 * (TINYWL_SNAP_NONE hides it). Cheap to call on every motion event. */
static void update_snap_preview(struct tinywl_server *server, tinywl_snap_zone zone) {
	if (!server->snap_preview) return;

	if (zone == TINYWL_SNAP_NONE) {
		wlr_scene_node_set_enabled(&server->snap_preview->node, false);
		return;
	}

	struct wlr_box box;
	if (!snap_zone_box(server, zone, &box)) {
		wlr_scene_node_set_enabled(&server->snap_preview->node, false);
		return;
	}

	wlr_scene_rect_set_size(server->snap_preview, box.width, box.height);
	wlr_scene_node_set_position(&server->snap_preview->node, box.x, box.y);
	wlr_scene_node_raise_to_top(&server->snap_preview->node);
	wlr_scene_node_set_enabled(&server->snap_preview->node, true);
}

static void reset_cursor_mode(struct tinywl_server *server) {
	/* Reset the cursor mode to passthrough. */
	if (server->grabbed_toplevel &&
		(server->cursor_mode == TINYWL_CURSOR_MOVE ||
		 server->cursor_mode == TINYWL_CURSOR_RESIZE)) {

		/* Apply an armed snap zone: resize/reposition and save the pre-snap geometry. */
		if (server->cursor_mode == TINYWL_CURSOR_MOVE &&
				server->snap_pending != TINYWL_SNAP_NONE) {
			struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
			struct wlr_box box;
			if (snap_zone_box(server, server->snap_pending, &box)) {
				if (!toplevel->snapped && !toplevel->maximized) {
					struct wlr_box geo;
					wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
					toplevel->saved_geometry.x = toplevel->scene_tree->node.x;
					toplevel->saved_geometry.y = toplevel->scene_tree->node.y;
					toplevel->saved_geometry.width  = geo.width;
					toplevel->saved_geometry.height = geo.height;
				}
				if (server->snap_pending == TINYWL_SNAP_TOP) {
					/* Top zone snaps full-screen, i.e. maximize. */
					wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
					toplevel->maximized = true;
					toplevel->snapped = false;
					toplevel->snapped_zone = TINYWL_SNAP_NONE;
				} else {
					toplevel->snapped = true;
					toplevel->snapped_zone = server->snap_pending;
				}
				wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
					box.width, box.height);
				wlr_scene_node_set_position(&toplevel->scene_tree->node,
					box.x, box.y);
			}
		}
		server->snap_pending = TINYWL_SNAP_NONE;
		update_snap_preview(server, TINYWL_SNAP_NONE);

		/* Save the window state after resize/move is complete */
		save_window_state(server->grabbed_toplevel);
	}
	server->cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct tinywl_server *server, uint32_t time) {
	/* Move the grabbed toplevel to the new position. */
	struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);

	/* Re-evaluate the snap zone every motion event and keep the preview
	 * in sync — this is what makes the split-screen preview track the
	 * cursor as it approaches an edge while dragging. */
	tinywl_snap_zone zone = compute_snap_zone(server);
	if (zone != server->snap_pending) {
		server->snap_pending = zone;
		update_snap_preview(server, zone);
	}
}

static void process_cursor_resize(struct tinywl_server *server, uint32_t time) {
	/* Resize (and possibly move) the grabbed toplevel from any edge or corner. */
	struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box geo_box;
	toplevel_get_geometry(toplevel, &geo_box);
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box.x, new_top - geo_box.y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	toplevel_set_size(toplevel, new_width, new_height);
}

static void handle_pointer_grab_surface_destroy(struct wl_listener *listener, void *data) {
	struct tinywl_server *server = wl_container_of(
			listener, server, pointer_grab_surface_destroy);
	wl_list_remove(&server->pointer_grab_surface_destroy.link);
	server->pointer_grab_surface = NULL;
	/* 
	 * The grabbed surface can vanish mid-click with no release event; 
	 * reset pointer_button_count here or future clicks stay blocked. 
	 */
	server->pointer_button_count = 0;
	wlr_seat_pointer_clear_focus(server->seat);
}

static void process_cursor_motion(struct tinywl_server *server, uint32_t time) {
	/* Keep the drag-and-drop icon (if any) following the cursor. Cheap to
	 * call even when no drag is active, since drag_icon has no children
	 * then. */
	wlr_scene_node_set_position(&server->drag_icon->node,
			server->cursor->x, server->cursor->y);

	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor_mode == TINYWL_CURSOR_MOVE) {
		process_cursor_move(server, time);
		return;
	} else if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
		process_cursor_resize(server, time);
		return;
	}

	/* Otherwise, find the toplevel under the pointer and send the event along. */
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;

	if (server->pointer_button_count > 0 && server->pointer_grab_surface &&
			!server->seat->drag) {
		/*
		 * Implicit pointer grab: keep motion on the surface that had the button press,
		 * except during a wl_data_device drag-and-drop, where focus must follow
		 * the surface under the cursor.
		 */
		surface = server->pointer_grab_surface;
		sx = server->cursor->x - server->pointer_grab_offset_x;
		sy = server->cursor->y - server->pointer_grab_offset_y;
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
		return;
	}

	struct tinywl_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!toplevel) {
		/* If there's no toplevel under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any toplevels. */
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		/* Send pointer enter/motion; wlroots dedupes repeated events automatically. */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	/* Move the cursor; passing NULL for the device moves it without input. */
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	/* Absolute pointer motion (0..1 per axis); warp the cursor to match. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

static void toggle_maximize(struct tinywl_toplevel *toplevel) {
	struct tinywl_server *server = toplevel->server;

	if (toplevel->maximized) {
		/* Restore to saved geometry */
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			toplevel->saved_geometry.width,
			toplevel->saved_geometry.height);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->saved_geometry.x,
			toplevel->saved_geometry.y);
		toplevel->maximized = false;
	} else {
		/* Save pre-maximize geometry, unless already snapped (saved_geometry then holds the real pre-snap size). */
		if (!toplevel->snapped) {
			struct wlr_box geo;
			wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
			toplevel->saved_geometry.x = toplevel->scene_tree->node.x;
			toplevel->saved_geometry.y = toplevel->scene_tree->node.y;
			toplevel->saved_geometry.width  = geo.width;
			toplevel->saved_geometry.height = geo.height;
		}
		toplevel->snapped = false;
		toplevel->snapped_zone = TINYWL_SNAP_NONE;

		/* Get output dimensions */
		struct tinywl_output *output = NULL;
		if (!wl_list_empty(&server->outputs)) {
			output = wl_container_of(server->outputs.next, output, link);
		}
		int out_width = 0, out_height = 0;
		struct wlr_box out_box = {0};
		if (output) {
			wlr_output_effective_resolution(output->wlr_output,
				&out_width, &out_height);
			wlr_output_layout_get_box(server->output_layout,
				output->wlr_output, &out_box);
		}

		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, out_width,
			out_height - tinywl_panel_get_height(server->panel));
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			out_box.x, out_box.y);
		toplevel->maximized = true;
	}
	
	/* Save the updated state */
	save_window_state(toplevel);
}

static void toggle_fullscreen(struct tinywl_toplevel *toplevel) {
	struct tinywl_server *server = toplevel->server;

	if (toplevel->fullscreen) {
		/* Restore to saved geometry */
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			toplevel->saved_geometry_fullscreen.width,
			toplevel->saved_geometry_fullscreen.height);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->saved_geometry_fullscreen.x,
			toplevel->saved_geometry_fullscreen.y);
		toplevel->fullscreen = false;
		
		/* Show panel again */
		tinywl_panel_show(server->panel);
	} else {
		/* Save current geometry before going fullscreen */
		struct wlr_box geo;
		wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo);
		toplevel->saved_geometry_fullscreen.x = toplevel->scene_tree->node.x;
		toplevel->saved_geometry_fullscreen.y = toplevel->scene_tree->node.y;
		toplevel->saved_geometry_fullscreen.width  = geo.width;
		toplevel->saved_geometry_fullscreen.height = geo.height;

		/* Get output dimensions */
		struct tinywl_output *output = NULL;
		if (!wl_list_empty(&server->outputs)) {
			output = wl_container_of(server->outputs.next, output, link);
		}
		int out_width = 0, out_height = 0;
		struct wlr_box out_box = {0};
		if (output) {
			wlr_output_effective_resolution(output->wlr_output,
				&out_width, &out_height);
			wlr_output_layout_get_box(server->output_layout,
				output->wlr_output, &out_box);
		}

		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, out_width, out_height);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			out_box.x, out_box.y);
		toplevel->fullscreen = true;
		
		/* Hide panel during fullscreen */
		tinywl_panel_hide(server->panel);
	}

	/* Save the updated state */
	save_window_state(toplevel);
}

static void raise_dialogs_to_front(struct tinywl_server *server) {
	/* Raise all dialog windows to the top of the stack to keep them visible
	 * above progress windows and other background windows */
	struct tinywl_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->is_dialog) {
			wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
		}
	}
	/* Ensure panel stays on top */
	tinywl_panel_raise_to_top(server->panel);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a button
	 * event. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct tinywl_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	/* Right-click on background/root toggles the menu. */
	if (event->button == BTN_RIGHT &&
	    event->state  == WLR_BUTTON_PRESSED &&
	    toplevel == NULL) {
		if (server->menu && tinywl_menu_is_visible(server->menu)) {
			tinywl_menu_hide(server->menu);
		} else {
			tinywl_menu_show(server->menu,
				(int)server->cursor->x,
				(int)server->cursor->y,
				event->time_msec);
		}
		return;
	}

	/* Notify the client with pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);

	if (event->state == WLR_BUTTON_PRESSED) {
		if (server->pointer_button_count == 0 && surface) {
			server->pointer_grab_surface = surface;
			server->pointer_grab_offset_x = server->cursor->x - sx;
			server->pointer_grab_offset_y = server->cursor->y - sy;
			server->pointer_grab_surface_destroy.notify =
				handle_pointer_grab_surface_destroy;
			wl_signal_add(&surface->events.destroy,
					&server->pointer_grab_surface_destroy);
		}
		server->pointer_button_count++;
	} else if (event->state == WLR_BUTTON_RELEASED) {
		if (server->pointer_button_count > 0) {
			server->pointer_button_count--;
		}
		if (server->pointer_button_count == 0 && server->pointer_grab_surface) {
			wl_list_remove(&server->pointer_grab_surface_destroy.link);
			server->pointer_grab_surface = NULL;
		}
	}

	if (event->state == WLR_BUTTON_RELEASED) {
		/* If you released any buttons, we exit interactive move/resize mode. */
		reset_cursor_mode(server);
	} else {
		/* Focus that client if the button was _pressed_ */
		focus_toplevel(toplevel, surface);
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	/* Pointer frame event: groups related pointer events together. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct tinywl_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	/* Render the scene if needed and commit the output */
	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	/* This function is called when the backend requests a new state for
	 * the output. For example, Wayland and X11 backends request a new mode
	 * when the output window is resized. */
	struct tinywl_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);

	/* Re-layout background/panel on output resize. */
	struct tinywl_server *server = output->server;
	if (server->background) {
		tinywl_background_resize(server->background, server);
	}
	if (server->panel) {
		tinywl_panel_resize(server->panel, server);
	}
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct tinywl_output *output = wl_container_of(listener, output, destroy);
	struct tinywl_server *server = output->server;

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);

	/*
	 * Terminate if the last output disappeared, but only for nested backends
	 * (X11/Wayland); on real DRM/KMS, transient empty output lists during
	 * mode/VT changes shouldn't kill the compositor.
	 */
	if (wl_list_empty(&server->outputs) &&
			tinywl_backend_is_nested(server->backend)) {
		wlr_log(WLR_INFO, "Last (nested) output destroyed, shutting down");
		wl_display_terminate(server->wl_display);
	}
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Configures the output created by the backend to use our allocator
	 * and our renderer. Must be done once, before commiting the output */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	/* The output may be disabled, switch it on. */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	/* Pick the output's preferred mode. */
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	/* Atomically applies the new output state. */
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Allocates and configures our state for this output */
	struct tinywl_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;

	/* Sets up a listener for the frame event. */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	/* Sets up a listener for the state request event. */
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	/* Sets up a listener for the destroy event. */
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	/* Add the output to the layout (left-to-right) and expose a wl_output global. */
	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout,
		wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, map);

	/* Insert at the tail, not the head: this list doubles as focus-recency
	 * order, and windows that skip focus_toplevel() (e.g. progress dialogs)
	 * must not be mistaken for the most-recently-focused window later. */
	wl_list_insert(toplevel->server->toplevels.prev, &toplevel->link);

	/* Try to load saved window state based on app_id */
	bool state_loaded = false;
	if (toplevel->xdg_toplevel->app_id) {
		state_loaded = load_window_state(toplevel, toplevel->xdg_toplevel->app_id);
	}

	/* Skip restoring saved geometry for short-lived dialogs that share their parent's app_id. */
	if (state_loaded) {
		if (toplevel->saved_geometry.width <= 0 || toplevel->saved_geometry.height <= 0) {
			/* A 0x0 saved size means nothing was actually stored; treat it as not loaded. */
			state_loaded = false;
		} else {
			struct wlr_box current_geo;
			wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &current_geo);

			if (current_geo.width > 0 && current_geo.height > 0) {
				int width_diff  = abs(current_geo.width  - toplevel->saved_geometry.width);
				int height_diff = abs(current_geo.height - toplevel->saved_geometry.height);
				bool size_matches_saved =
					width_diff  <= toplevel->saved_geometry.width  / 4 &&
					height_diff <= toplevel->saved_geometry.height / 4;

				/* Saved size doesn't match what the surface is asking for; treat as not loaded. */
				if (!size_matches_saved) {
					state_loaded = false;
				}
			}
		}
	}

	struct tinywl_server *server = toplevel->server;

	/* Check if this is a dialog/modal window (has parent) */
	toplevel->is_dialog = (toplevel->xdg_toplevel->parent != NULL);
	toplevel->is_progress = false;
	
	/* Exception: Waydroid and similar containers should not be treated as dialogs even if they have parent */
	if (toplevel->is_dialog && toplevel->xdg_toplevel->app_id) {
		const char *app_id = toplevel->xdg_toplevel->app_id;
		if (strstr(app_id, "waydroid") || strstr(app_id, "Waydroid") ||
		    strstr(app_id, "container") || strstr(app_id, "Container")) {
			toplevel->is_dialog = false;
		}
	}
	
	/* Additional dialog detection: common dialog/modal window titles/names */
	if (!toplevel->is_dialog && toplevel->xdg_toplevel->title) {
		const char *title = toplevel->xdg_toplevel->title;
		/* Exception: Waydroid title should not be treated as dialog */
		if (strstr(title, "Waydroid") || strstr(title, "waydroid")) {
			/* Skip dialog detection for waydroid */
		}
		/* Detect progress windows and notifications */
		else if (strstr(title, "Progress") || strstr(title, "progress") ||
		    strstr(title, "Notification") || strstr(title, "notification")) {
			toplevel->is_progress = true;
		}
		/* Common dialog patterns */
		else if (strstr(title, "Confirm") || strstr(title, "Warning") || 
		    strstr(title, "Error") || strstr(title, "Question") ||
		    strstr(title, "replace") || strstr(title, "Replace") ||
		    strstr(title, "Create") || strstr(title, "Rename") ||
		    strstr(title, "Delete") || strstr(title, "Move") ||
		    strstr(title, "Save") || strstr(title, "Open")) {
			toplevel->is_dialog = true;
		}
	}
	
	/* Force dialog windows to NOT be maximized or fullscreen */
	if (toplevel->is_dialog) {
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
		toplevel->maximized = false;
		/* Center dialog on screen */
		if (!wl_list_empty(&server->outputs)) {
			struct tinywl_output *output = wl_container_of(server->outputs.next, output, link);
			int out_width = 0, out_height = 0;
			wlr_output_effective_resolution(output->wlr_output, &out_width, &out_height);
			
			struct wlr_box out_box;
			wlr_output_layout_get_box(server->output_layout, output->wlr_output, &out_box);
			
			struct wlr_box geo_box;
			wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo_box);
			
			int width = geo_box.width;
			int height = geo_box.height;
			int panel_height = tinywl_panel_get_height(server->panel);
			
			/* Constrain size */
			int max_width = out_width - 40;
			int max_height = out_height - panel_height - 40;
			
			if (width > max_width) width = max_width;
			if (height > max_height) height = max_height;
			
			if (width != geo_box.width || height != geo_box.height) {
				wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
			}
			
			/* Center position */
			int x = out_box.x + (out_width - width) / 2;
			int y = out_box.y + (out_height - height) / 2;
			wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
		}
		focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
		return; /* Dialog done, don't process further */
	}
	
	/* Force progress windows to smaller size and NOT be maximized */
	if (toplevel->is_progress) {
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
		toplevel->maximized = false;
		/* Center progress window on screen */
		if (!wl_list_empty(&server->outputs)) {
			struct tinywl_output *output = wl_container_of(server->outputs.next, output, link);
			int out_width = 0, out_height = 0;
			wlr_output_effective_resolution(output->wlr_output, &out_width, &out_height);
			
			struct wlr_box out_box;
			wlr_output_layout_get_box(server->output_layout, output->wlr_output, &out_box);
			
			struct wlr_box geo_box;
			wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo_box);
			
			int width = geo_box.width;
			int height = geo_box.height;
			
			/* Constrain size for progress window - much smaller */
			int max_width = 500;
			int max_height = 250;
			
			if (width > max_width) width = max_width;
			if (height > max_height) height = max_height;
			
			if (width != geo_box.width || height != geo_box.height) {
				wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
			}
			
			/* Center position */
			int x = out_box.x + (out_width - width) / 2;
			int y = out_box.y + (out_height - height) / 2;
			wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
		}
		return; /* Progress window done, don't process further */
	}
	
	/* For normal windows (non-dialog), restore saved state or center */
	if (state_loaded) {
		/* Special case: Waydroid should ALWAYS be maximized, regardless of saved state */
		bool is_waydroid = (toplevel->xdg_toplevel->app_id && 
			(strstr(toplevel->xdg_toplevel->app_id, "Waydroid") || 
			 strstr(toplevel->xdg_toplevel->app_id, "waydroid")));
		
		if (is_waydroid) {
			struct tinywl_output *output;
			if (!wl_list_empty(&server->outputs)) {
				output = wl_container_of(server->outputs.next, output, link);
				int out_width = 0, out_height = 0;
				wlr_output_effective_resolution(output->wlr_output, &out_width, &out_height);
				
				struct wlr_box out_box;
				wlr_output_layout_get_box(server->output_layout,
					output->wlr_output, &out_box);
				
				int panel_height = tinywl_panel_get_height(server->panel);
				wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
				wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, out_width, 
					out_height - panel_height);
				wlr_scene_node_set_position(&toplevel->scene_tree->node, 
					out_box.x, out_box.y);
				toplevel->maximized = true;
			}
		} else if (toplevel->maximized) {
			/* Window was previously maximized, restore that state */
			struct tinywl_output *output;
			if (!wl_list_empty(&server->outputs)) {
				output = wl_container_of(server->outputs.next, output, link);
				int out_width = 0, out_height = 0;
				wlr_output_effective_resolution(output->wlr_output, &out_width, &out_height);
				
				struct wlr_box out_box;
				wlr_output_layout_get_box(server->output_layout,
					output->wlr_output, &out_box);
				
				/* Maximize to output size, accounting for panel height */
				int panel_height = tinywl_panel_get_height(server->panel);
				wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
				wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, out_width, 
					out_height - panel_height);
				wlr_scene_node_set_position(&toplevel->scene_tree->node, 
					out_box.x, out_box.y);
			}
		} else {
			/* Restore saved geometry for non-maximized window */
			if (toplevel->saved_geometry.width > 0) {
				wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 
					toplevel->saved_geometry.width,
					toplevel->saved_geometry.height);
			}
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
				toplevel->saved_geometry.x,
				toplevel->saved_geometry.y);
		}
	} else {
		/* No saved state: check if this is Waydroid that should be maximized */
		bool is_waydroid = false;
		const char *app_id = toplevel->xdg_toplevel->app_id ?: "no-app-id";
		const char *title = toplevel->xdg_toplevel->title ?: "no-title";
		
		if (toplevel->xdg_toplevel->app_id) {
			if (strstr(app_id, "waydroid") || strstr(app_id, "Waydroid")) {
				is_waydroid = true;
			}
		}
		
		/* Also check title as fallback */
		if (!is_waydroid && strstr(title, "waydroid")) {
			is_waydroid = true;
		}
		
		/* Waydroid should be maximized by default */
		if (is_waydroid) {
			struct tinywl_output *output;
			if (!wl_list_empty(&server->outputs)) {
				output = wl_container_of(server->outputs.next, output, link);
				int out_width = 0, out_height = 0;
				wlr_output_effective_resolution(output->wlr_output, &out_width, &out_height);
				
				struct wlr_box out_box;
				wlr_output_layout_get_box(server->output_layout,
					output->wlr_output, &out_box);
				
				int panel_height = tinywl_panel_get_height(server->panel);
				wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
				wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, out_width,
					out_height - panel_height);
				wlr_scene_node_set_position(&toplevel->scene_tree->node,
					out_box.x, out_box.y);
				toplevel->maximized = true;
			}
		} else {
			/* Normal window: center new window on screen */
			struct tinywl_output *output;
		if (!wl_list_empty(&server->outputs)) {
			output = wl_container_of(server->outputs.next, output, link);

			/* Get the output's effective resolution */
			int out_width = 0, out_height = 0;
			wlr_output_effective_resolution(output->wlr_output, &out_width, &out_height);

			/* Get the output's position in the layout */
			struct wlr_box out_box;
			wlr_output_layout_get_box(server->output_layout,
				output->wlr_output, &out_box);

			/* Get the actual surface size - this is what the client rendered */
			struct wlr_box geo_box;
			wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo_box);
			
			int width = geo_box.width;
			int height = geo_box.height;
			int panel_height = tinywl_panel_get_height(server->panel);
			
			/* Constrain size for new windows */
			int max_width = out_width - 40;
			int max_height = out_height - panel_height - 40;
			
			/* For progress windows, use smaller constraint */
			if (toplevel->is_progress) {
				max_width = 500;   /* Max 500px width for progress */
				max_height = 250;  /* Max 250px height for progress */
			}
			
			if (width > max_width) {
				width = max_width;
			}
			if (height > max_height) {
				height = max_height;
			}
			
			if (width != geo_box.width || height != geo_box.height) {
				wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
			}
			
			/* Center position calculation */
			int x = out_box.x + (out_width - width) / 2;
			int y = out_box.y + (out_height - height) / 2;

			wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
		}
		}
	}

	/* Register this window in the taskbar */
	tinywl_panel_on_map(server->panel, toplevel);
	focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);

	/* If this is a dialog, ensure it stays on top of progress windows */
	if (toplevel->is_dialog) {
		raise_dialogs_to_front(server);
	}
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct tinywl_server *server = toplevel->server;

	/* If window was in fullscreen, show panel again */
	if (toplevel->fullscreen) {
		tinywl_panel_show(server->panel);
		toplevel->fullscreen = false;
	}

	/* Reset the cursor mode if the grabbed toplevel was unmapped. */
	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}

	/* Return keyboard focus to another window when the focused one closes. */
	bool was_focused = server->seat->keyboard_state.focused_surface ==
			toplevel->xdg_toplevel->base->surface;

	/* Remove this window from the taskbar */
	tinywl_panel_on_unmap(server->panel, toplevel);
	wl_list_remove(&toplevel->link);

	if (was_focused) {
		struct tinywl_toplevel *next = NULL;

		/* Prefer returning focus to the closing toplevel's actual parent window. */
		struct wlr_xdg_toplevel *wlr_parent = toplevel->xdg_toplevel->parent;
		if (wlr_parent) {
			struct tinywl_toplevel *t;
			wl_list_for_each(t, &server->toplevels, link) {
				if (t->xdg_toplevel == wlr_parent && !t->minimized) {
					next = t;
					break;
				}
			}
		}

		if (!next) {
			struct tinywl_toplevel *t;
			wl_list_for_each(t, &server->toplevels, link) {
				if (!t->minimized) {
					next = t;
					break;
				}
			}
		}

		if (next) {
			focus_toplevel(next, toplevel_wlr_surface(next));
		} else {
			wlr_seat_keyboard_notify_clear_focus(server->seat);
			tinywl_panel_on_focus(server->panel, NULL);
		}
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_toplevel is destroyed. */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	/* Save the final state before destroying */
	save_window_state(toplevel);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->request_minimize.link);

	free(toplevel);
}

/*
 * XWayland toplevel support: mirrors the xdg_toplevel map/unmap/destroy
 * handling above so X11 windows actually get a scene node and become
 * visible/focusable/closable. Taskbar entries, saved state, and CSD-driven
 * move/resize/maximize are xdg-shell-only and not covered here.
 */

static void xwayland_surface_request_configure(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, xwayland_request_configure);
	struct wlr_xwayland_surface *xsurface = toplevel->xwayland_surface;
	struct wlr_xwayland_surface_configure_event *event = data;

	int x = event->x;
	int y = event->y;
	int width = event->width;
	int height = event->height;

	if (!xsurface->override_redirect) {
		struct tinywl_server *server = toplevel->server;
		struct wlr_box out_box;
		int out_w, out_h;
		if (get_primary_output_box(server, &out_box, &out_w, &out_h)) {
			/* 
			 * Clamp to the usable area, same as the initial map, 
			 * so a client resizing/maximizing itself doesn't push itself off-screen. 
			 */
			int panel_height = tinywl_panel_get_height(server->panel);
			int max_width = out_w - 40;
			int max_height = out_h - panel_height - 40;
			if (width > max_width) width = max_width;
			if (height > max_height) height = max_height;
			if (x + width > out_box.x + out_w) x = out_box.x + out_w - width;
			if (x < out_box.x) x = out_box.x;
			if (y + height > out_box.y + out_h - panel_height) {
				y = out_box.y + out_h - panel_height - height;
			}
			if (y < out_box.y) y = out_box.y;
		}
	}

	wlr_xwayland_surface_configure(xsurface, x, y, width, height);
	if (toplevel->scene_tree) {
		wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
	}
}

static void xwayland_surface_map(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	struct tinywl_server *server = toplevel->server;
	struct wlr_xwayland_surface *xsurface = toplevel->xwayland_surface;

	toplevel->scene_tree = wlr_scene_tree_create(&server->scene->tree);
	wlr_scene_subsurface_tree_create(toplevel->scene_tree, xsurface->surface);

	int x = xsurface->x;
	int y = xsurface->y;
	if (!xsurface->override_redirect) {
		struct wlr_box out_box;
		int out_w, out_h;
		if (get_primary_output_box(server, &out_box, &out_w, &out_h)) {
			/* 
			 * Constrain to fit on screen like xdg_toplevel_map does for dialogs, 
			 * so a client requesting an oversized window doesn't clip off-screen once centered. 
			 */
			int panel_height = tinywl_panel_get_height(server->panel);
			int max_width = out_w - 40;
			int max_height = out_h - panel_height - 40;

			int width = xsurface->width;
			int height = xsurface->height;
			if (width > max_width) width = max_width;
			if (height > max_height) height = max_height;

			if (width != xsurface->width || height != xsurface->height) {
				wlr_xwayland_surface_configure(xsurface, xsurface->x, xsurface->y,
					width, height);
			}

			if (x <= 0 && y <= 0) {
				/* No sane position requested: center it like an xdg dialog. */
				x = out_box.x + (out_w - width) / 2;
				y = out_box.y + (out_h - panel_height - height) / 2;
			}
		}
	}
	wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);

	if (!xsurface->override_redirect) {
		/*
		 * Only non-override-redirect windows are focusable toplevels (node.data set,
		 * inserted into server->toplevels). Override-redirect windows (popup menus)
		 * get a scene node but stay click-through: their ->link is never inserted,
		 * so resolving a click to them and calling focus_toplevel() would corrupt
		 * an uninitialized list node.
		 */
		toplevel->scene_tree->node.data = toplevel;
		/* Tail, not head — see the xdg_toplevel_map comment on the same
		 * pattern: only focus_toplevel() should promote a toplevel to the
		 * front of the focus-order list. */
		wl_list_insert(server->toplevels.prev, &toplevel->link);
		focus_toplevel(toplevel, xsurface->surface);
	}
}

static void xwayland_surface_unmap(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct tinywl_server *server = toplevel->server;
	struct wlr_xwayland_surface *xsurface = toplevel->xwayland_surface;

	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}

	bool was_focused = server->seat->keyboard_state.focused_surface ==
			toplevel_wlr_surface(toplevel);

	if (!xsurface->override_redirect) {
		wl_list_remove(&toplevel->link);
		wl_list_init(&toplevel->link);
	}

	if (toplevel->scene_tree) {
		wlr_scene_node_destroy(&toplevel->scene_tree->node);
		toplevel->scene_tree = NULL;
	}

	if (was_focused) {
		struct tinywl_toplevel *next = NULL;
		struct tinywl_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (!t->minimized) {
				next = t;
				break;
			}
		}
		if (next) {
			focus_toplevel(next, toplevel_wlr_surface(next));
		} else {
			wlr_seat_keyboard_notify_clear_focus(server->seat);
			tinywl_panel_on_focus(server->panel, NULL);
		}
	}
}

static void xwayland_surface_associate(struct wl_listener *listener, void *data) {
	/* xsurface->surface only becomes valid between associate and dissociate;
	 * that's when we can hook the generic wlr_surface map/unmap events. */
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, xwayland_associate);
	struct wlr_xwayland_surface *xsurface = toplevel->xwayland_surface;

	toplevel->map.notify = xwayland_surface_map;
	wl_signal_add(&xsurface->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xwayland_surface_unmap;
	wl_signal_add(&xsurface->surface->events.unmap, &toplevel->unmap);
}

static void xwayland_surface_dissociate(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, xwayland_dissociate);
	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
}

static void xwayland_surface_destroy(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	/* Defensive cleanup: unmap may never fire if the surface is destroyed while still mapped (e.g. wlr_xwayland_destroy() during shutdown); leaving a dangling scene node/list entry caused shutdown hangs. */
	if (toplevel->link.next != &toplevel->link) {
		wl_list_remove(&toplevel->link);
	}
	if (toplevel->scene_tree) {
		wlr_scene_node_destroy(&toplevel->scene_tree->node);
		toplevel->scene_tree = NULL;
	}

	wl_list_remove(&toplevel->xwayland_associate.link);
	wl_list_remove(&toplevel->xwayland_dissociate.link);
	wl_list_remove(&toplevel->xwayland_request_configure.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->destroy.link);

	free(toplevel);
}

static void xwayland_surface_request_move(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
}

static void xwayland_surface_request_resize(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	struct wlr_xwayland_resize_event *event = data;
	begin_interactive(toplevel, TINYWL_CURSOR_RESIZE, event->edges);
}

static void xwayland_surface_request_maximize(struct wl_listener *listener, void *data) {
	/* Mirrors toggle_maximize()'s xdg logic, dispatched for XWayland.
	 * wlroots already updates xsurface->maximized_horz/vert to reflect what
	 * the client is asking for before emitting this event. */
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	struct wlr_xwayland_surface *xsurface = toplevel->xwayland_surface;
	struct tinywl_server *server = toplevel->server;

	if (!toplevel->scene_tree) {
		/* Not mapped yet (e.g. client requests "start maximized" before its
		 * window is ever shown) — nothing to resize/reposition yet. */
		return;
	}

	bool want_maximized = xsurface->maximized_horz && xsurface->maximized_vert;

	if (want_maximized == toplevel->maximized) {
		return;
	}

	if (toplevel->maximized) {
		/* Restore to saved geometry */
		wlr_xwayland_surface_set_maximized(xsurface, false);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->saved_geometry.x, toplevel->saved_geometry.y);
		toplevel_set_size(toplevel, toplevel->saved_geometry.width,
			toplevel->saved_geometry.height);
		toplevel->maximized = false;
	} else {
		if (!toplevel->snapped) {
			struct wlr_box geo;
			toplevel_get_geometry(toplevel, &geo);
			toplevel->saved_geometry.x = toplevel->scene_tree->node.x;
			toplevel->saved_geometry.y = toplevel->scene_tree->node.y;
			toplevel->saved_geometry.width = geo.width;
			toplevel->saved_geometry.height = geo.height;
		}
		toplevel->snapped = false;
		toplevel->snapped_zone = TINYWL_SNAP_NONE;

		struct wlr_box out_box;
		int out_w, out_h;
		if (get_primary_output_box(server, &out_box, &out_w, &out_h)) {
			int panel_height = tinywl_panel_get_height(server->panel);
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
				out_box.x, out_box.y);
			wlr_xwayland_surface_set_maximized(xsurface, true);
			toplevel_set_size(toplevel, out_w, out_h - panel_height);
		}
		toplevel->maximized = true;
	}
}

static void xwayland_surface_request_fullscreen(struct wl_listener *listener, void *data) {
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	struct wlr_xwayland_surface *xsurface = toplevel->xwayland_surface;
	struct tinywl_server *server = toplevel->server;

	if (!toplevel->scene_tree) {
		/* Not mapped yet — see the matching guard in request_maximize. */
		return;
	}

	bool want_fullscreen = xsurface->fullscreen;

	if (want_fullscreen == toplevel->fullscreen) {
		return;
	}

	if (toplevel->fullscreen) {
		wlr_xwayland_surface_set_fullscreen(xsurface, false);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->saved_geometry_fullscreen.x,
			toplevel->saved_geometry_fullscreen.y);
		toplevel_set_size(toplevel, toplevel->saved_geometry_fullscreen.width,
			toplevel->saved_geometry_fullscreen.height);
		toplevel->fullscreen = false;
		tinywl_panel_show(server->panel);
	} else {
		struct wlr_box geo;
		toplevel_get_geometry(toplevel, &geo);
		toplevel->saved_geometry_fullscreen.x = toplevel->scene_tree->node.x;
		toplevel->saved_geometry_fullscreen.y = toplevel->scene_tree->node.y;
		toplevel->saved_geometry_fullscreen.width = geo.width;
		toplevel->saved_geometry_fullscreen.height = geo.height;

		struct wlr_box out_box;
		int out_w, out_h;
		if (get_primary_output_box(server, &out_box, &out_w, &out_h)) {
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
				out_box.x, out_box.y);
			wlr_xwayland_surface_set_fullscreen(xsurface, true);
			toplevel_set_size(toplevel, out_w, out_h);
		}
		toplevel->fullscreen = true;
		tinywl_panel_hide(server->panel);
	}
}

static void new_xwayland_surface(struct wl_listener *listener, void *data) {
	struct tinywl_server *server =
		wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xsurface = data;

	struct tinywl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		wlr_log(WLR_ERROR, "Failed to allocate memory for xwayland toplevel");
		return;
	}
	toplevel->server = server;
	toplevel->xdg_toplevel = NULL;
	toplevel->xwayland_surface = xsurface;
	xsurface->data = toplevel;
	wl_list_init(&toplevel->link);

	toplevel->xwayland_associate.notify = xwayland_surface_associate;
	wl_signal_add(&xsurface->events.associate, &toplevel->xwayland_associate);
	toplevel->xwayland_dissociate.notify = xwayland_surface_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &toplevel->xwayland_dissociate);
	toplevel->destroy.notify = xwayland_surface_destroy;
	wl_signal_add(&xsurface->events.destroy, &toplevel->destroy);
	toplevel->xwayland_request_configure.notify = xwayland_surface_request_configure;
	wl_signal_add(&xsurface->events.request_configure,
		&toplevel->xwayland_request_configure);
	toplevel->request_move.notify = xwayland_surface_request_move;
	wl_signal_add(&xsurface->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xwayland_surface_request_resize;
	wl_signal_add(&xsurface->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xwayland_surface_request_maximize;
	wl_signal_add(&xsurface->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xwayland_surface_request_fullscreen;
	wl_signal_add(&xsurface->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void begin_interactive(struct tinywl_toplevel *toplevel,
		enum tinywl_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propegating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	if (!toplevel->scene_tree) {
		/* Not mapped yet — nothing to move/resize. */
		return;
	}
	struct tinywl_server *server = toplevel->server;
	struct wlr_surface *focused_surface =
		server->seat->pointer_state.focused_surface;
	if (toplevel_wlr_surface(toplevel) !=
			wlr_surface_get_root_surface(focused_surface)) {
		/* Deny move/resize requests from unfocused clients. */
		return;
	}
	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == TINYWL_CURSOR_MOVE) {
		if (toplevel->snapped) {
			/* Restore pre-snap size before dragging, so grabbing a snapped titlebar un-snaps it. */
			struct wlr_box cur_geo;
			toplevel_get_geometry(toplevel, &cur_geo);
			int cur_x = toplevel->scene_tree->node.x;
			int cur_w = cur_geo.width > 0 ? cur_geo.width : 1;

			double rel_x = (server->cursor->x - cur_x) / (double)cur_w;
			if (rel_x < 0.0) rel_x = 0.0;
			if (rel_x > 1.0) rel_x = 1.0;

			int new_w = toplevel->saved_geometry.width  > 0 ?
				toplevel->saved_geometry.width  : cur_w;
			int new_h = toplevel->saved_geometry.height > 0 ?
				toplevel->saved_geometry.height : cur_geo.height;
			int new_x = (int)(server->cursor->x - rel_x * new_w);
			int new_y = (int)server->cursor->y - 10;

			wlr_scene_node_set_position(&toplevel->scene_tree->node, new_x, new_y);
			toplevel_set_size(toplevel, new_w, new_h);

			toplevel->snapped = false;
			toplevel->snapped_zone = TINYWL_SNAP_NONE;
		}

		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		struct wlr_box geo_box;
		toplevel_get_geometry(toplevel, &geo_box);

		double border_x = (toplevel->scene_tree->node.x + geo_box.x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box.y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = geo_box;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;

		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	/* Client requests an interactive move (e.g. from CSD). */
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* Client requests an interactive resize (e.g. from CSD). */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, TINYWL_CURSOR_RESIZE, event->edges);
}

void minimize_toplevel(struct tinywl_toplevel *toplevel) {
	/* Minimize: xdg-shell has no minimized state, so just hide the scene node. */
	if (toplevel->minimized)
		return;
	toplevel->minimized = true;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
	toplevel_set_activated(toplevel, false);

	/* Pass keyboard focus to the next visible (non-minimized) window */
	struct tinywl_server *server = toplevel->server;
	struct tinywl_toplevel *next = NULL;
	struct tinywl_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t != toplevel && !t->minimized) {
			next = t;
			break;
		}
	}
	if (next) {
		focus_toplevel(next, toplevel_wlr_surface(next));
	} else {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		tinywl_panel_on_focus(server->panel, NULL);
	}
	
	/* Save the minimized state */
	save_window_state(toplevel);
}

void restore_toplevel(struct tinywl_toplevel *toplevel) {
	/* Restore a minimized window and give it focus. */
	if (!toplevel->minimized) {
		/* Not minimized — just focus */
		focus_toplevel(toplevel, toplevel_wlr_surface(toplevel));
		return;
	}
	toplevel->minimized = false;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	tinywl_panel_raise_to_top(toplevel->server->panel);
	focus_toplevel(toplevel, toplevel_wlr_surface(toplevel));
	
	/* Save the restored state */
	save_window_state(toplevel);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	/* Client requested maximize. For dialogs/modals, we ignore this.
	 * For normal windows, we honor the request. */
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	
	/* Check if this is a dialog/modal window */
	if (toplevel->is_dialog) {
		/* This is a transient/modal dialog - REJECT maximize request */
		return;
	}
	
	/* For normal windows, allow maximize */
	toggle_maximize(toplevel);
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	/* Client requested fullscreen. Dialog windows should not go fullscreen. */
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	
	/* Check if this is a dialog/modal window */
	if (toplevel->is_dialog || toplevel->is_progress) {
		/* This is a transient/modal dialog or progress window - REJECT fullscreen */
		return;
	}
	
	/* For normal windows, allow fullscreen */
	toggle_fullscreen(toplevel);
}

static void xdg_toplevel_request_minimize(
		struct wl_listener *listener, void *data) {
	/* Client requested minimize via its own CSD minimize button */
	struct tinywl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_minimize);
	minimize_toplevel(toplevel);
}

/* Unconstrain a new popup via its positioner before the first configure, so clients size themselves correctly from the start. */
static void unconstrain_new_popup(struct tinywl_server *server,
		struct wlr_xdg_surface *xdg_surface, struct wlr_scene_tree *parent_tree) {
	struct wlr_box out_box = {0};
	int out_w = 0, out_h = 0;
	if (!get_primary_output_box(server, &out_box, &out_w, &out_h)) {
		return;
	}
	int usable_h = out_h - tinywl_panel_get_height(server->panel);

	struct wlr_scene_tree *root_child = scene_tree_root_child(server, parent_tree);
	int root_x = root_child ? root_child->node.x : 0;
	int root_y = root_child ? root_child->node.y : 0;

	struct wlr_box box = {
		.x = out_box.x - root_x,
		.y = out_box.y - root_y,
		.width  = out_w,
		.height = usable_h,
	};
	wlr_xdg_popup_unconstrain_from_box(xdg_surface->popup, &box);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	/* Add xdg popups to the scene graph via their parent's scene node. */
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_surface *parent =
			wlr_xdg_surface_try_from_wlr_surface(xdg_surface->popup->parent);
		assert(parent != NULL);
		struct wlr_scene_tree *parent_tree = parent->data;
		struct wlr_scene_tree *popup_tree = wlr_scene_xdg_surface_create(
			parent_tree, xdg_surface);
		xdg_surface->data = popup_tree;

		unconstrain_new_popup(server, xdg_surface, parent_tree);

		/* Create popup tracker to apply constraints when mapped */
		struct tinywl_popup *popup = calloc(1, sizeof(*popup));
		if (popup) {
			popup->xdg_surface = xdg_surface;
			popup->scene_tree = popup_tree;
			popup->server = server;
			popup->last_geom_x = 0;
			popup->last_geom_y = 0;
			popup->map.notify = popup_handle_map;
			popup->commit.notify = popup_handle_reposition;
			popup->destroy.notify = popup_handle_destroy;
			wl_signal_add(&xdg_surface->surface->events.map, &popup->map);
			wl_signal_add(&xdg_surface->surface->events.commit, &popup->commit);
			wl_signal_add(&xdg_surface->events.destroy, &popup->destroy);
			wl_list_insert(&server->popups, &popup->link);
		}
		return;
	}
	assert(xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);

	/* Allocate a tinywl_toplevel for this surface */
	struct tinywl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_surface->toplevel;
	toplevel->scene_tree = wlr_scene_xdg_surface_create(
			&toplevel->server->scene->tree, toplevel->xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_surface->data = toplevel->scene_tree;

	/* Listen to the various events it can emit */
	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_surface->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_surface->surface->events.unmap, &toplevel->unmap);
	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &toplevel->destroy);

	/* cotd */
	struct wlr_xdg_toplevel *xdg_toplevel = xdg_surface->toplevel;
	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize,
		&toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen,
		&toplevel->request_fullscreen);
	toplevel->request_minimize.notify = xdg_toplevel_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize,
		&toplevel->request_minimize);
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	/* Initialize window state persistence system */
	init_window_state_system();

	/* Setup Wayland environment for child processes */
	if (!getenv("XDG_RUNTIME_DIR")) {
		char runtime_dir[256];
		snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%u", getuid());
		setenv("XDG_RUNTIME_DIR", runtime_dir, 0);
	}
	
	/* Enable Wayland support for applications */
	setenv("MOZ_ENABLE_WAYLAND", "1", 0);
	setenv("QT_QPA_PLATFORM", "wayland", 0);
	setenv("GDK_BACKEND", "wayland", 0);

	struct tinywl_server server = {0};
	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	server.wl_display = wl_display_create();

	/* Register signal handling early, before any child processes are forked. */
	struct wl_event_loop *term_loop = wl_display_get_event_loop(server.wl_display);
	wl_event_loop_add_signal(term_loop, SIGINT, handle_term_signal, server.wl_display);
	wl_event_loop_add_signal(term_loop, SIGTERM, handle_term_signal, server.wl_display);
	/* SIGCHLD is registered later, after XWayland has been started (see
	 * tinywl_services_init() below) — see the comment there for why. */

	/* Autocreate the backend for the current environment. */
	server.backend = wlr_backend_autocreate(server.wl_display, NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	/* Autocreate the renderer (Pixman/GLES2/Vulkan, or via WLR_RENDERER). */
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* Autocreate the allocator bridging renderer and backend. */
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	/* Core wlroots interfaces: compositor, subcompositor, data device manager. */
	server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	/* Create screencopy manager for screenshot/screen recording support */
	server.screencopy_mgr = wlr_screencopy_manager_v1_create(server.wl_display);
	if (server.screencopy_mgr == NULL) {
		wlr_log(WLR_ERROR, "Failed to create screencopy manager");
		return 1;
	}

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	server.output_layout = wlr_output_layout_create();

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/* Scene graph handles rendering and damage tracking. */
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	/* Scene tree that drag-and-drop icons attach to. */
	server.drag_icon = wlr_scene_tree_create(&server.scene->tree);

	/* Preview rect for edge-snap, toggled while dragging near a screen edge. */
	{
		const float snap_color[4] = {
			SNAP_PREVIEW_R, SNAP_PREVIEW_G, SNAP_PREVIEW_B, SNAP_PREVIEW_A
		};
		server.snap_preview =
			wlr_scene_rect_create(&server.scene->tree, 1, 1, snap_color);
		if (server.snap_preview) {
			wlr_scene_node_set_enabled(&server.snap_preview->node, false);
		}
	}

	/* xdg-shell protocol for application windows. */
	wl_list_init(&server.toplevels);
	wl_list_init(&server.popups);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_surface.notify = server_new_xdg_surface;
	wl_signal_add(&server.xdg_shell->events.new_surface,
			&server.new_xdg_surface);

	/* Cursor: tracks the on-screen cursor image. */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Xcursor manager for cursor themes across scale factors (Adwaita, size 24). */
	server.cursor_mgr = wlr_xcursor_manager_create("Adwaita", 24);

	/* wlr_cursor only displays the cursor; attached input devices generate the motion events handled above. */
	server.cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/* Configure the seat (keyboard/pointer/touch/tablet) and watch for new input devices. */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);
        server.request_start_drag.notify = seat_request_start_drag;
        wl_signal_add(&server.seat->events.request_start_drag,
                        &server.request_start_drag);
        server.start_drag.notify = seat_start_drag;
        wl_signal_add(&server.seat->events.start_drag,
                        &server.start_drag);
	/* Init right-click background menu (replicates TWM's "defops" menu). */
	server.menu = tinywl_menu_init(&server);
	if (!server.menu) {
		wlr_log(WLR_ERROR, "Failed to initialize menu");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Register socket/lock cleanup immediately after creating them. */
	const char *runtime_dir_for_socket = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir_for_socket) {
		snprintf(wayland_socket_path, sizeof(wayland_socket_path),
				"%s/%s", runtime_dir_for_socket, socket);
		snprintf(wayland_lock_path, sizeof(wayland_lock_path),
				"%s/%s.lock", runtime_dir_for_socket, socket);
		atexit(cleanup_wayland_socket_files);
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);

	/* Start background services: D-Bus, XWayland, GVFS, Polkit, audio, settings daemon. */
	server.services = tinywl_services_init(&server);
	if (!server.services) {
		wlr_log(WLR_ERROR, "Failed to initialise background services");
		/* Non-fatal: compositor runs fine without them */
	}

	/* Hook up X11 window handling (see XWayland toplevel support above); 
	without it X11 clients run but never display a window. */
	struct wlr_xwayland *xwayland = tinywl_services_get_xwayland(server.services);
	if (xwayland) {
		server.new_xwayland_surface.notify = new_xwayland_surface;
		wl_signal_add(&xwayland->events.new_surface, &server.new_xwayland_surface);
	}

	/* Registered after XWayland starts: wl_event_loop_add_signal() blocks SIGCHLD 
	for this process and everything it forks afterward, including wlroots' 
	own Xwayland child, which needs it unblocked. */
	wl_event_loop_add_signal(term_loop, SIGCHLD, handle_sigchld, &server);

	/* Load the wallpaper; must run after the backend starts and WAYLAND_DISPLAY is set. */
	server.background = tinywl_background_create(&server);
	if (!server.background) {
		wlr_log(WLR_ERROR, "Failed to initialise background");
		/* Non-fatal: compositor works fine without a background */
	}

	/* Create the bottom panel; must run after the backend starts and WAYLAND_DISPLAY is set. */
	server.panel = tinywl_panel_create(&server);
	if (!server.panel) {
		wlr_log(WLR_ERROR, "Failed to initialise panel");
		/* Non-fatal */
	}
	/* Register minimize/restore callbacks so the taskbar can call back in */
	tinywl_panel_set_callbacks(server.panel, minimize_toplevel, restore_toplevel);

	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}
	/* Run the Wayland event loop until the compositor exits. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);

	wl_display_run(server.wl_display);

	/*
	 * Remove the socket/lock files first: service shutdown can block, and a session
	 * manager's logout timeout may SIGKILL this process before atexit runs.
	 */
	cleanup_wayland_socket_files();

	/*
	 * Destroy background services (including XWayland) before tearing down Wayland
	 * clients, or wlroots sees XWayland's connection die unexpectedly and
	 * auto-restarts it mid-teardown, hanging shutdown.
	 */
	tinywl_services_destroy(server.services);
	wl_display_destroy_clients(server.wl_display);
	tinywl_panel_destroy(server.panel);
	tinywl_background_destroy(server.background);
	tinywl_menu_destroy(server.menu);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_output_layout_destroy(server.output_layout);
	wl_display_destroy(server.wl_display);

	return 0;
}
