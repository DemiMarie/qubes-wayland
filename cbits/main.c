#define _POSIX_C_SOURCE 200112L
#include "common.h"
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <wlr/allocator/interface.h>
#include <wlr/allocator/wlr_allocator.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include <xkbcommon/xkbcommon.h>

#ifdef BUILD_RUST
#include <qubes-gui-protocol.h>
#endif
#include "qubes_output.h"
#include "qubes_backend.h"
#include "qubes_allocator.h"
#include "main.h"

/* NOT IMPLEMENTABLE:
 *
 * - MSG_DOCK: involves a D-Bus listener, out of scope for initial
 *             implementation
 * - MSG_MFNDUMP: obsolete
 * - MSG_CURSOR: requires some sort of image recognition
 */

struct tinywl_output {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
};

struct tinywl_keyboard {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
	uint32_t magic;
};

static void focus_view(struct tinywl_view *view, struct wlr_surface *surface) {
	/* Note: this function only deals with keyboard focus. */
	if (view == NULL) {
		return;
	}
	struct tinywl_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */
		return;
	}
	if (prev_surface) {
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret.
		 */
		struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
					seat->keyboard_state.focused_surface);
		wlr_xdg_toplevel_set_activated(previous, false);
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	assert(keyboard);
	/* Move the view to the front */
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	/* Activate the new surface */
	wlr_xdg_toplevel_set_activated(view->xdg_surface, true);
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	assert(view->xdg_surface);
	wlr_seat_keyboard_notify_enter(seat, view->xdg_surface->surface,
		keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data __attribute__((unused))) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	/* QUBES EVENT HOOK: figure this out somehow */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	assert(QUBES_KEYBOARD_MAGIC == keyboard->magic);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->device->keyboard->modifiers);
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	/* QUBES EVENT HOOK: call this from event handler */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct tinywl_server *server = keyboard->server;
	struct wlr_event_keyboard_key *event = data;
	struct wlr_seat *seat = server->seat;
	assert(QUBES_KEYBOARD_MAGIC == keyboard->magic);

	/* Translate libinput keycode -> xkbcommon */
	wlr_seat_set_keyboard(seat, keyboard->device);
	wlr_seat_keyboard_notify_key(seat, event->time_msec,
		event->keycode, event->state);
}

static void server_new_keyboard(struct tinywl_server *server,
		struct wlr_input_device *device) {
	assert(device->keyboard);
	struct tinywl_keyboard *keyboard =
		calloc(1, sizeof(struct tinywl_keyboard));
	keyboard->magic = QUBES_KEYBOARD_MAGIC;
	keyboard->server = server;
	keyboard->device = device;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	if (keymap && device->keyboard) {
		wlr_keyboard_set_keymap(device->keyboard, keymap);
		xkb_keymap_unref(keymap);
		wlr_keyboard_set_repeat_info(device->keyboard, 0, 0);
	}
	xkb_context_unref(context);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&device->keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&device->keyboard->events.key, &keyboard->key);

	wlr_seat_set_keyboard(server->seat, device);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct tinywl_server *server,
		struct wlr_input_device *device) {
	/* We don't do anything special with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	/* QUBES UNREACHABLE HOOK: remove this */
	if (server->cursor)
		wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	assert(QUBES_SERVER_MAGIC == server->magic);
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
	assert(QUBES_SERVER_MAGIC == server->magic);
	/* This event is raised by the seat when a client provides a cursor image */
	/* QUBES HOOK: somehow convert this to an X cursor to send to GUI daemon */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		/* Once we've vetted the client, we can tell the cursor to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on and continue to do so as the
		 * cursor moves between outputs. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in tinywl we always honor
	 */
	/* QUBES HOOK: store a copy to send to GUI qube */
	struct tinywl_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	assert(QUBES_SERVER_MAGIC == server->magic);
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new output (aka a display oe
	 * monitor) becomes available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	assert(QUBES_SERVER_MAGIC == server->magic);

	/* Allocates and configures our state for this output */
	struct tinywl_output *output =
		calloc(1, sizeof(struct tinywl_output));
	output->wlr_output = wlr_output;
	output->server = server;
	wl_list_insert(&server->outputs, &output->link);

	// assert(!wl_list_empty(&wlr_output->modes));
	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	wlr_output_layout_add_auto(server->output_layout, wlr_output);
}

static void xdg_surface_map(struct wl_listener *listener, void *data __attribute__((unused))) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	/* QUBES HOOK: MSG_MAP: map the corresponding window */
	struct tinywl_view *view = wl_container_of(listener, view, map);
	assert(QUBES_VIEW_MAGIC == view->magic);
	view->mapped = true;
	focus_view(view, view->xdg_surface->surface);
}

static void xdg_surface_unmap(struct wl_listener *listener, void *data __attribute__((unused))) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	/* QUBES HOOK: MSG_UNMAP: unmap the corresponding window */
	struct tinywl_view *view = wl_container_of(listener, view, unmap);
	assert(QUBES_VIEW_MAGIC == view->magic);
	view->mapped = false;
#ifdef BUILD_RUST
	wlr_log(WLR_DEBUG, "Sending MSG_UNMAP (0x%x) to window %" PRIu32, MSG_UNMAP, view->window_id);
	struct msg_hdr header = {
		.type = MSG_UNMAP,
		.window = view->window_id,
		.untrusted_len = 0,
	};
	assert(qubes_rust_send_message(view->server->backend->rust_backend, &header));
#endif
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data __attribute__((unused))) {
	/* Called when the surface is destroyed and should never be shown again. */
	struct tinywl_view *view = wl_container_of(listener, view, destroy);
	assert(QUBES_VIEW_MAGIC == view->magic);
	wl_list_remove(&view->link);
#ifdef BUILD_RUST
	wlr_log(WLR_DEBUG, "Sending MSG_DESTROY (0x%x) to window %" PRIu32, MSG_DESTROY, view->window_id);
	struct msg_hdr header = {
		.type = MSG_DESTROY,
		.window = view->window_id,
		.untrusted_len = 0,
	};
	assert(qubes_rust_send_message(view->server->backend->rust_backend, &header));
	qubes_rust_delete_id(view->server->backend->rust_backend, view->window_id);
#endif
	if (view->scene_subsurface_tree)
		wlr_scene_node_destroy(view->scene_subsurface_tree);
	free(view);
}

static void begin_interactive(struct tinywl_view *view,
		enum tinywl_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propegating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct tinywl_server *server = view->server;
	struct wlr_surface *focused_surface =
		server->seat->pointer_state.focused_surface;
	if (view->xdg_surface->surface != focused_surface) {
		/* Deny move/resize requests from unfocused clients. */
		return;
	}
	server->grabbed_view = view;
	server->cursor_mode = mode;

	if (mode == TINYWL_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
	} else {
		struct wlr_box geo_box;
		wlr_xdg_surface_get_geometry(view->xdg_surface, &geo_box);

		double border_x = (view->x + geo_box.x) + ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y = (view->y + geo_box.y) + ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = geo_box;
		server->grab_geobox.x += view->x;
		server->grab_geobox.y += view->y;

		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data __attribute__((unused))) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	/* QUBES EVENT HOOK: implement the above check OR omit this entirely */
	struct tinywl_view *view = wl_container_of(listener, view, request_move);
	assert(QUBES_VIEW_MAGIC == view->magic);
	begin_interactive(view, TINYWL_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	/* QUBES EVENT HOOK: implement the above check OR omit this entirely */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct tinywl_view *view = wl_container_of(listener, view, request_resize);
	assert(QUBES_VIEW_MAGIC == view->magic);
	begin_interactive(view, TINYWL_CURSOR_RESIZE, event->edges);
}

static void qubes_new_popup(
		struct wl_listener *listener, void *data) {
	struct tinywl_view *view = wl_container_of(listener, view, new_popup);
	struct wlr_xdg_popup *popup __attribute__((unused)) = data;
	assert(QUBES_VIEW_MAGIC == view->magic);
}

static void qubes_request_maximize(
		struct wl_listener *listener, void *data __attribute__((unused))) {
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to maximize window */
	struct tinywl_view *view = wl_container_of(listener, view, request_maximize);
	assert(QUBES_VIEW_MAGIC == view->magic);
	wlr_log(WLR_ERROR, "window maximize: not implemented");
}

static void qubes_request_minimize(
		struct wl_listener *listener, void *data __attribute__((unused))) {
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to minimize window */
	struct tinywl_view *view = wl_container_of(listener, view, request_minimize);
	assert(QUBES_VIEW_MAGIC == view->magic);
	wlr_log(WLR_ERROR, "window minimize: not implemented");
}

static void qubes_request_fullscreen(
		struct wl_listener *listener, void *data __attribute__((unused))) {
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to fullscreen window */
	struct tinywl_view *view = wl_container_of(listener, view, request_fullscreen);
	assert(QUBES_VIEW_MAGIC == view->magic);
	wlr_log(WLR_ERROR, "window fullscreen: not implemented");
	/* qubes_rust_set_window_flags(view->window_id) */
}

static void qubes_set_title(
		struct wl_listener *listener, void *data __attribute__((unused))) {
	/* QUBES HOOK: MSG_WMNAME: ask GUI daemon to set window title */
	struct tinywl_view *view = wl_container_of(listener, view, set_title);
	assert(QUBES_VIEW_MAGIC == view->magic);
	assert(view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);
#ifdef BUILD_RUST
	assert(view->window_id);
	wlr_log(WLR_DEBUG, "Sending MSG_WMNAME (0x%x) to window %" PRIu32, MSG_WMNAME, view->window_id);
	struct {
		struct msg_hdr header;
		struct msg_wmname title;
	} msg = {
		.header = {
			.type = MSG_WMNAME,
			.window = view->window_id,
			.untrusted_len = sizeof(struct msg_wmname),
		},
		.title = {
			.data = { 0 },
		},
	};
	strncpy(msg.title.data, view->xdg_surface->toplevel->title, sizeof(msg.title.data) - 1);
	assert(qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&msg));
#endif
}

static void qubes_surface_commit(
		struct wl_listener *listener, void *data __attribute__((unused))) {
	/* QUBES HOOK: MSG_WINDOW_HINTS, plus do a bunch of actual rendering stuff :) */
	struct tinywl_view *view = wl_container_of(listener, view, commit);
	assert(QUBES_VIEW_MAGIC == view->magic);
	assert(view->scene_output);
	struct wlr_box box;
	wlr_xdg_surface_get_geometry(view->xdg_surface, &box);
	bool need_configure = false;
	if (view->x != box.x || view->y != box.y) {
		view->x = box.x;
		view->y = box.y;
		need_configure = true;
		wlr_scene_output_set_position(view->scene_output, view->x, view->y);
	}
#ifndef BUILD_RUST
# define MAX_WINDOW_WIDTH (1 << 14)
# define MAX_WINDOW_HEIGHT ((1 << 11) * 3)
#endif
	if (box.width <= 0 || box.height <= 0 || box.width > MAX_WINDOW_WIDTH || box.height > MAX_WINDOW_HEIGHT) {
	}
	if (view->last_width != box.width || view->last_height != box.height) {
		need_configure = true;
		view->last_width = box.width;
		view->last_height = box.height;
	}
	wlr_output_set_custom_mode(&view->output.output, box.width, box.height, 60000);
	wlr_output_enable(&view->output.output, view->mapped);
	if (!view->mapped)
		return;
	assert(view->scene_output->output == &view->output.output);
	wlr_scene_output_commit(view->scene_output);
	wlr_log(WLR_DEBUG, "Width is %" PRIu32 " height is %" PRIu32, (uint32_t)box.width, (uint32_t)box.height);
#ifdef BUILD_RUST
	if (need_configure) {
		wlr_log(WLR_DEBUG, "Sending MSG_CONFIGURE (0x%x) to window %" PRIu32, MSG_CONFIGURE, view->window_id);
		struct {
			struct msg_hdr header;
			struct msg_configure configure;
		} msg = {
			.header = {
				.type = MSG_CONFIGURE,
				.window = view->window_id,
				.untrusted_len = sizeof(struct msg_configure),
			},
			.configure = {
				.x = box.x,
				.y = box.y,
				.width = box.width,
				.height = box.height,
				.override_redirect = 0,
			},
		};
		assert(qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&msg));
	}
	wlr_log(WLR_DEBUG, "Sending MSG_MAP (0x%x) to window %" PRIu32, MSG_MAP, view->window_id);

	struct {
		struct msg_hdr header;
		struct msg_map_info info;
	} msg = {
		.header = {
			.type = MSG_MAP,
			.window = view->window_id,
			.untrusted_len = sizeof(struct msg_map_info),
		},
		.info = {
			.transient_for = 0,
			.override_redirect = 0,
		},
	};
	assert(qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&msg));
	wlr_log(WLR_DEBUG, "Sending MSG_SHMIMAGE (0x%x) to window %" PRIu32, MSG_SHMIMAGE, view->window_id);
	struct {
		struct msg_hdr header;
		struct msg_shmimage shmimage;
	} new_msg = {
		.header = {
			.type = MSG_SHMIMAGE,
			.window = view->window_id,
			.untrusted_len = sizeof(struct msg_shmimage),
		},
		.shmimage = {
			.x = 0,
			.y = 0,
			.width = INT32_MAX,
			.height = INT32_MAX,
		},
	};
	assert(qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&new_msg));
#else
	(void)need_configure;
#endif
	struct timespec now;
	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	wlr_output_send_frame(&view->output.output);
	wlr_surface_send_frame_done(view->xdg_surface->surface, &now);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;
	assert(QUBES_SERVER_MAGIC == server->magic);
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		/* this is handled by the new_popup listener */
		return;
	}
	/* QUBES HOOK: MSG_CREATE: create toplevel window */

	/* Allocate a tinywl_view for this surface */
	struct tinywl_view *view = calloc(1, sizeof(struct tinywl_view));
	if (!view)
		goto cleanup;
	assert(view);
	view->server = server;
	/* Add wlr_output */
	qubes_output_init(&view->output, &server->backend->backend, server->wl_display);
	assert(wlr_output_set_allocator(&view->output.output, server->allocator));
	view->output.output.allocator->render_formats = view->output.formats;
	if (!(view->scene = wlr_scene_create()))
		goto cleanup;

	assert(view->output.output.allocator);

	if (!(view->scene_output = wlr_scene_output_create(view->scene, &view->output.output)))
		goto cleanup;
	if (!(view->scene_subsurface_tree = wlr_scene_subsurface_tree_create(&view->scene_output->scene->node, xdg_surface->surface)))
		goto cleanup;
	wlr_scene_node_set_enabled(&view->scene_output->scene->node, true);
	wlr_scene_node_set_enabled(view->scene_subsurface_tree, true);
	wlr_scene_node_raise_to_top(view->scene_subsurface_tree);

	view->magic = QUBES_VIEW_MAGIC;
	view->xdg_surface = xdg_surface;

	/* Listen to the various events it can emit */
	view->map.notify = xdg_surface_map;
	wl_signal_add(&xdg_surface->events.map, &view->map);
	view->unmap.notify = xdg_surface_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
	view->destroy.notify = xdg_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
	view->new_popup.notify = qubes_new_popup;
	wl_signal_add(&xdg_surface->events.new_popup, &view->new_popup);

	/* And listen to the various emits the toplevel can emit */
	struct wlr_xdg_toplevel *toplevel = xdg_surface->toplevel;
	view->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = qubes_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
	view->request_minimize.notify = qubes_request_minimize;
	wl_signal_add(&toplevel->events.request_minimize, &view->request_minimize);
	view->request_fullscreen.notify = qubes_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
	view->set_title.notify = qubes_set_title;
	wl_signal_add(&toplevel->events.set_title, &view->set_title);

	/* Listen to surface events */
	view->commit.notify = qubes_surface_commit;
	wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);

	/* Add it to the list of views. */
	wl_list_insert(&server->views, &view->link);

	/* Get the window ID */
	assert(view->window_id == 0);
	view->window_id = qubes_rust_generate_id(view->server->backend->rust_backend, view);

	/* Tell GUI daemon to create window */
	struct wlr_box box;
	wlr_xdg_surface_get_geometry(xdg_surface, &box);
	if (box.width <= 0)
		box.width = 1;
	if (box.height <= 0)
		box.height = 1;
	wlr_output_set_custom_mode(&view->output.output, box.width, box.height, 60000);
	wlr_output_enable(&view->output.output, true);
#ifdef BUILD_RUST
	wlr_log(WLR_DEBUG, "Sending MSG_CREATE (0x%x) to window %" PRIu32, MSG_CREATE, view->window_id);
	struct {
		struct msg_hdr header;
		struct msg_create create;
	} msg = {
		.header = {
			.type = MSG_CREATE,
			.window = view->window_id,
			.untrusted_len = sizeof(struct msg_create),
		},
		.create = {
			.x = box.x,
			.y = box.y,
			.width = box.width,
			.height = box.height,
			.parent = 0,
			.override_redirect = 0,
		},
	};
	assert(qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&msg));
#endif
	return;
cleanup:
	wl_resource_post_no_memory(xdg_surface->resource);
	if (view) {
		if (view->scene_subsurface_tree)
			wlr_scene_node_destroy(view->scene_subsurface_tree);
		if (view->scene_output)
			wlr_scene_output_destroy(view->scene_output);
		qubes_rust_delete_id(view->server->backend->rust_backend, view->window_id);
		free(view);
	}
	return;
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	char *startup_cmd = NULL;

	int c;
	if (argc < 1) {
		fputs("NULL argv[0] passed\n", stderr);
		return 1;
	}

	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command] [--] domid\n", argv[0]);
			return 0;
		}
	}
	if (optind != argc - 1) {
		printf("Usage: %s [-s startup command] [--] domid\n", argv[0]);
		return 0;
	}
	const char *domid_str = argv[argc - 1];
	if (domid_str[0] < '0' || domid_str[0] > '9') {
bad_domid:
		fprintf(stderr, "%s: %s is not a valid domain ID\n", argv[0], domid_str);
		return 1;
	}
	bool octal_domid = domid_str[0] == '0' && (domid_str[1] != 'x' && domid_str[1] != '\0');
	errno = 0;
	char *endptr = NULL;
	unsigned long raw_domid = strtoul(domid_str, &endptr, 0);
	uint16_t domid;
	if (endptr && *endptr == '\0') {
		if (raw_domid && octal_domid) {
			fprintf(stderr, "%s: Sorry, but octal domain ID %s isn't allowed\n", argv[0], domid_str);
			return 1;
		}
		if (errno == ERANGE || raw_domid > UINT16_MAX) {
			fprintf(stderr, "%s: Sorry, domain ID %s is too large (maximum is 65535)\n", argv[0], domid_str);
			return 1;
		}
		if (errno)
			goto bad_domid;
		domid = raw_domid;
	} else goto bad_domid;

	struct tinywl_server *server = calloc(1, sizeof(*server));
	if (!server) {
		wlr_log(WLR_ERROR, "Cannot create tinywl_server");
		return 1;
	}
	server->magic = QUBES_SERVER_MAGIC;
	server->domid = domid;

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	if (!(server->wl_display = wl_display_create())) {
		wlr_log(WLR_ERROR, "Cannot create wl_display");
		return 1;
	}

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	if (!(server->backend = qubes_backend_create(server->wl_display, domid))) {
		wlr_log(WLR_ERROR, "Cannot create wlr_backend");
		return 1;
	}

	/* If we don't provide a renderer, autocreate makes a GLES2 renderer for us.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	if (!(server->renderer = wlr_backend_get_renderer(&server->backend->backend))) {
		wlr_log(WLR_ERROR, "No renderer from wlr_backend");
		return 1;
	}

	wlr_renderer_init_wl_display(server->renderer, server->wl_display);

	/* The allocator is the bridge between the renderer and the backend.
	 * It handles the buffer managment between the two, allowing wlroots
	 * to render onto the screen */
	if (!(server->allocator = qubes_allocator_create(domid))) {
		wlr_log(WLR_ERROR, "Cannot create Qubes allocator");
		return 1;
	}

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	wlr_compositor_create(server->wl_display, server->renderer);
	wlr_data_device_manager_create(server->wl_display);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	if (!(server->output_layout = wlr_output_layout_create())) {
		wlr_log(WLR_ERROR, "Cannot create output layout");
		return 1;
	}

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server->outputs);
	server->new_output.notify = server_new_output;
	wl_signal_add(&server->backend->backend.events.new_output, &server->new_output);

	/* Set up our list of views and the xdg-shell. The xdg-shell is a Wayland
	 * protocol which is used for application windows. For more detail on
	 * shells, refer to my article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&server->views);
	if (!(server->xdg_shell = wlr_xdg_shell_create(server->wl_display))) {
		wlr_log(WLR_ERROR, "Cannot create xdg_shell");
		return 1;
	}

	server->new_xdg_surface.notify = server_new_xdg_surface;
	wl_signal_add(&server->xdg_shell->events.new_surface,
			&server->new_xdg_surface);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server->keyboards);
	server->new_input.notify = server_new_input;
	wl_signal_add(&server->backend->backend.events.new_input, &server->new_input);
	if (!(server->seat = wlr_seat_create(server->wl_display, "seat0"))) {
		wlr_log(WLR_ERROR, "Cannot create wlr_seat");
		return 1;
	}

	server->request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor,
			&server->request_cursor);
	server->request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
			&server->request_set_selection);

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server->wl_display);
	if (!socket) {
		wlr_log(WLR_ERROR, "Cannot listen on Wayland socket");
		wlr_backend_destroy(&server->backend->backend);
		return 1;
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(&server->backend->backend)) {
		wlr_backend_destroy(&server->backend->backend);
		wl_display_destroy(server->wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}
	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);
	wl_display_run(server->wl_display);

	/* Once wl_display_run returns, we shut down the server-> */
	wl_display_destroy_clients(server->wl_display);
	wl_display_destroy(server->wl_display);
	free(server);
	return 0;
}
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
