// Main program and most window management logic

#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <inttypes.h>
#include "config.h"
#ifdef QUBES_HAS_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <getopt.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <wayland-server-core.h>

#include <wlr/render/allocator.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
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

// A single *physical* output (in the GUI daemon).
// Owned by the wlr_output.
struct tinywl_output {
	struct wl_list link;
	struct tinywl_server *server;
	struct wl_listener output_destroy;
	struct wlr_output *wlr_output;
};

struct tinywl_keyboard {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
	uint32_t magic;
};

static void qubes_send_frame_done(struct wlr_surface *surface,
	int sx __attribute__((unused)), int sy __attribute__((unused)),
	void *data)
{
	wlr_surface_send_frame_done(surface, data);
}

static int qubes_send_frame_callbacks(void *data)
{
	struct tinywl_server *server = data;
	struct timespec now;
	struct tinywl_view *view;
	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	server->frame_pending = false;
	wl_list_for_each(view, &server->views, link) {
		assert(QUBES_VIEW_MAGIC == view->magic);
		view->output.output.frame_pending = false;
		wlr_output_send_frame(&view->output.output);
		wlr_scene_node_for_each_surface(
			&view->scene_output->scene->node,
			qubes_send_frame_done, &now);
	}
	return 0;
}

void qubes_give_view_keyboard_focus(struct tinywl_view *view, struct wlr_surface *surface)
{
	/* Note: this function only deals with keyboard focus. */
	if (view == NULL) {
		return;
	}
	struct tinywl_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */
		if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL &&
		    !view->xdg_surface->toplevel->pending.activated)
			wlr_xdg_toplevel_set_activated(view->xdg_surface, true);
		return;
	}
	wlr_log(WLR_INFO, "Giving keyboard focus to window %u", view->window_id);
	if (prev_surface && wlr_surface_is_xdg_surface(prev_surface)) {
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret.
		 */
		struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(prev_surface);
		if (previous->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
			wlr_xdg_toplevel_set_activated(previous, false);
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	assert(keyboard);
	assert(view->xdg_surface);
	/* Move the view to the front */
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	/* Activate the new surface */
	struct wlr_xdg_surface *view_surface = view->xdg_surface;
	while (view_surface && view_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		view_surface = wlr_xdg_surface_from_wlr_surface(view_surface->popup->parent);
	}
	if (view_surface && view_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wlr_xdg_toplevel_set_activated(view_surface, true);
	}
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
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
	assert(keyboard);
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

static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in tinywl we always honor
	 */
	/* QUBES HOOK: store a copy to send to GUI qube */
	struct tinywl_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	const char **mime_type;
	assert(QUBES_SERVER_MAGIC == server->magic);
	struct wlr_data_source *source = event->source;
	// Sanitize MIME types
	wl_array_for_each(mime_type, &source->mime_types) {
		for (const char *s = *mime_type; *s; ++s) {
			if (*s < 0x21 || *s >= 0x7F) {
				wlr_log(WLR_ERROR, "Refusing to set selection with bad MIME type");
				return;
			}
		}
	}
	// SANITIZE END
	wlr_seat_set_selection(server->seat, source, event->serial);
}

static void qubes_output_destroy(struct wl_listener *listener, void *data QUBES_UNUSED)
{
	struct tinywl_output *output =
		wl_container_of(listener, output, output_destroy);
	wl_list_remove(&output->output_destroy.link);
	wl_list_remove(&output->link);
	free(output);
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
	output->output_destroy.notify = qubes_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->output_destroy);
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

static void qubes_change_window_flags(struct tinywl_view *view, uint32_t flags_set, uint32_t flags_unset)
{
#ifdef BUILD_RUST
	assert(qubes_output_created(view));
	struct {
		struct msg_hdr header;
		struct msg_window_flags flags;
	} msg = {
		.header = {
			.type = MSG_WINDOW_FLAGS,
			.window = view->window_id,
			.untrusted_len = sizeof(struct msg_window_flags),
		},
		.flags = {
			.flags_set = flags_set,
			.flags_unset = flags_unset,
		},
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.flags);
	// Asserted above, checked at call sites
	qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&msg);
#endif
}

static void qubes_set_view_title(struct tinywl_view *view)
{
#ifdef BUILD_RUST
	if (!strncmp(view->last_title.data,
	             view->xdg_surface->toplevel->title,
	             sizeof(view->last_title.data) - 1)) {
		return;
	}
	assert(qubes_output_created(view));
	assert(view->window_id);
	strncpy(view->last_title.data,
	        view->xdg_surface->toplevel->title,
	        sizeof(view->last_title.data) - 1);
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
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.title);
	strncpy(msg.title.data,
	        view->xdg_surface->toplevel->title,
	        sizeof(msg.title.data) - 1);
	// Asserted above, checked at call sites
	qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&msg);
#endif
}

static void qubes_set_view_app_id(struct tinywl_view *view)
{
#ifdef BUILD_RUST
	assert(qubes_output_created(view));
	assert(view->window_id);
	wlr_log(WLR_DEBUG, "Sending MSG_WMCLASS (0x%x) to window %" PRIu32, MSG_WMCLASS, view->window_id);
	struct {
		struct msg_hdr header;
		struct msg_wmclass class;
	} msg = {
		.header = {
			.type = MSG_WMCLASS,
			.window = view->window_id,
			.untrusted_len = sizeof(struct msg_wmclass),
		},
		.class = {
			.res_class = { 0 },
			.res_name = { 0 },
		},
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.class);
	strncpy(msg.class.res_class, view->xdg_surface->toplevel->app_id, sizeof(msg.class.res_class) - 1);
	// Asserted above, checked at call sites
	qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&msg);
#endif
}

bool qubes_output_ensure_created(struct tinywl_view *view, struct wlr_box *box)
{
	assert(box);
	wlr_xdg_surface_get_geometry(view->xdg_surface, box);
	if (box->width <= 0 ||
	    box->height <= 0 ||
	    box->width > MAX_WINDOW_WIDTH ||
	    box->height > MAX_WINDOW_HEIGHT) {
		return false;
	}
	if (view->x != box->x || view->y != box->y) {
		view->x = box->x;
		view->y = box->y;
		wlr_scene_output_set_position(view->scene_output, view->x, view->y);
	}
	if (qubes_output_created(view))
		return true;
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
			.x = view->left,
			.y = view->top,
			.width = box->width,
			.height = box->height,
			.parent = 0,
			.override_redirect = view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP ? 1 : 0,
		},
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.create);
	// This is MSG_CREATE
	qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&msg);
	view->flags |= QUBES_OUTPUT_CREATED;
#endif
	return true;
}


static void xdg_surface_map(struct wl_listener *listener, void *data __attribute__((unused))) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	/* QUBES HOOK: MSG_MAP: map the corresponding window */
	struct tinywl_view *view = wl_container_of(listener, view, map);
	assert(QUBES_VIEW_MAGIC == view->magic);
	qubes_view_map(view);
}

void qubes_view_map(struct tinywl_view *view)
{
	struct wlr_box box;
	if (!qubes_output_ensure_created(view, &box))
		return;
	struct wlr_xdg_surface *xdg_surface = view->xdg_surface;
	if (!qubes_output_mapped(view)) {
		view->flags |= QUBES_OUTPUT_MAPPED;
		wlr_scene_node_set_enabled(&view->scene_output->scene->node, true);
		wlr_scene_node_set_enabled(view->scene_subsurface_tree, true);
		wlr_output_enable(&view->output.output, true);
	}
#ifdef BUILD_RUST
	uint32_t transient_for_window = 0;
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		uint32_t flags_to_set = 0, flags_to_unset = 0;
		if (xdg_surface->toplevel->requested.minimized) {
			flags_to_set = WINDOW_FLAG_MINIMIZE;
			flags_to_unset = WINDOW_FLAG_FULLSCREEN;
		} else if (xdg_surface->toplevel->requested.fullscreen) {
			flags_to_set = WINDOW_FLAG_FULLSCREEN;
			flags_to_unset = WINDOW_FLAG_MINIMIZE;
		}
		if (flags_to_set || flags_to_unset) {
			// Window created above, so this is safe
			qubes_change_window_flags(view, flags_to_set, flags_to_unset);
		}
		if (xdg_surface->toplevel->title) {
			// Window created above, so this is safe
			qubes_set_view_title(view);
		}
		if (xdg_surface->toplevel->app_id) {
			// Window created above, so this is safe
			qubes_set_view_app_id(view);
		}
		if (xdg_surface->toplevel->parent) {
			const struct tinywl_view *parent_view = xdg_surface->toplevel->parent->data;
			transient_for_window = parent_view->window_id;
		}
	}

	wlr_log(WLR_INFO,
	        "Sending MSG_MAP (0x%x) to window %u (transient_for = %u)",
	        MSG_MAP, view->window_id, transient_for_window);
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
			.transient_for = transient_for_window,
			.override_redirect = 0,
		},
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.info);
	// Surface created above
	qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr*)&msg);

#endif
}

static void xdg_surface_unmap(struct wl_listener *listener, void *data __attribute__((unused))) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	/* QUBES HOOK: MSG_UNMAP: unmap the corresponding window */
	struct tinywl_view *view = wl_container_of(listener, view, unmap);
	assert(QUBES_VIEW_MAGIC == view->magic);
	view->flags &= ~(__typeof__(view->flags))QUBES_OUTPUT_MAPPED;
	wlr_scene_node_set_enabled(&view->scene_output->scene->node, false);
	wlr_scene_node_set_enabled(view->scene_subsurface_tree, false);
	wlr_output_enable(&view->output.output, false);
#ifdef BUILD_RUST
	wlr_log(WLR_DEBUG, "Sending MSG_UNMAP (0x%x) to window %" PRIu32, MSG_UNMAP, view->window_id);
	struct msg_hdr header = {
		.type = MSG_UNMAP,
		.window = view->window_id,
		.untrusted_len = 0,
	};
	if (qubes_output_created(view))
		qubes_rust_send_message(view->server->backend->rust_backend, &header);
#endif
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data __attribute__((unused))) {
	/* Called when the surface is destroyed and should never be shown again. */
	struct tinywl_view *view = wl_container_of(listener, view, destroy);
	assert(QUBES_VIEW_MAGIC == view->magic);
	wl_list_remove(&view->link);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->new_popup.link);
	if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wl_list_remove(&view->request_maximize.link);
		wl_list_remove(&view->request_fullscreen.link);
		wl_list_remove(&view->request_minimize.link);
		wl_list_remove(&view->set_title.link);
		wl_list_remove(&view->set_app_id.link);
		wl_list_remove(&view->ack_configure.link);
	}
#ifdef BUILD_RUST
	wlr_log(WLR_DEBUG, "Sending MSG_DESTROY (0x%x) to window %" PRIu32, MSG_DESTROY, view->window_id);
	struct msg_hdr header = {
		.type = MSG_DESTROY,
		.window = view->window_id,
		.untrusted_len = 0,
	};
	if (qubes_output_created(view))
		qubes_rust_send_message(view->server->backend->rust_backend, &header);
	qubes_rust_delete_id(view->server->backend->rust_backend, view->window_id);
#endif
	if (view->scene_subsurface_tree)
		wlr_scene_node_destroy(view->scene_subsurface_tree);
	if (view->scene_output)
		wlr_scene_output_destroy(view->scene_output);
	wlr_output_destroy(&view->output.output);
	if (view->scene)
		wlr_scene_node_destroy(&view->scene->node);
	free(view);
}

static void qubes_new_popup(
		struct wl_listener *listener, void *data) {
	struct tinywl_view *view = wl_container_of(listener, view, new_popup);
	struct wlr_xdg_popup *popup __attribute__((unused)) = data;
	assert(QUBES_VIEW_MAGIC == view->magic);
}

static void qubes_request_maximize(
	struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to maximize window */
	struct tinywl_view *view = wl_container_of(listener, view, request_maximize);
	assert(QUBES_VIEW_MAGIC == view->magic);
#ifdef WINDOW_FLAG_MAXIMIZE
	if (qubes_output_mapped(view)) {
		wlr_log(WLR_DEBUG, "Maximizing window " PRIu32, view->window_id);
		// Mapped implies created
		qubes_change_window_flags(view, WINDOW_FLAG_MAXIMIZE, 0);
	}
#else
	wlr_log(WLR_ERROR, "window maximize: not implemented");
#endif
}

static void qubes_request_minimize(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to minimize window */
	struct tinywl_view *view = wl_container_of(listener, view, request_minimize);
	assert(QUBES_VIEW_MAGIC == view->magic);
	if (qubes_output_mapped(view)) {
		wlr_log(WLR_DEBUG, "Marking window %" PRIu32 " minimized", view->window_id);
		// Mapped implies created
		qubes_change_window_flags(view, WINDOW_FLAG_MINIMIZE, 0);
	}
}

static void qubes_request_fullscreen(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to fullscreen window */
	struct tinywl_view *view = wl_container_of(listener, view, request_fullscreen);
	assert(QUBES_VIEW_MAGIC == view->magic);
	if (qubes_output_mapped(view)) {
		wlr_log(WLR_DEBUG, "Marking window %" PRIu32 " fullscreen", view->window_id);
		// Mapped implies created
		qubes_change_window_flags(view, WINDOW_FLAG_FULLSCREEN, 0);
	}
}

static void qubes_set_title(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WMNAME: ask GUI daemon to set window title */
	struct tinywl_view *view = wl_container_of(listener, view, set_title);
	assert(QUBES_VIEW_MAGIC == view->magic);
	assert(view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);
	if (qubes_output_mapped(view)) {
		// Mapped implies created
		qubes_set_view_title(view);
	}
}

static void qubes_set_app_id(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WMNAME: ask GUI daemon to set window app id */
	struct tinywl_view *view = wl_container_of(listener, view, set_app_id);
	assert(QUBES_VIEW_MAGIC == view->magic);
	assert(view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);
	if (qubes_output_mapped(view)) {
		// Mapped implies created
		qubes_set_view_app_id(view);
	}
}

static void qubes_new_decoration(struct wl_listener *listener, void *data)
{
	struct tinywl_server *server = wl_container_of(listener, server, new_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

	wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void qubes_surface_commit(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_HINTS, plus do a bunch of actual rendering stuff :) */
	struct tinywl_view *view = wl_container_of(listener, view, commit);
	struct wlr_box box;
	assert(QUBES_VIEW_MAGIC == view->magic);
	assert(view->scene_output);
	if (!qubes_output_ensure_created(view, &box))
		return;
#ifndef BUILD_RUST
# define MAX_WINDOW_WIDTH (1 << 14)
# define MAX_WINDOW_HEIGHT ((1 << 11) * 3)
#endif
	if ((view->last_width != box.width || view->last_height != box.height) &&
	    !(view->flags & QUBES_OUTPUT_IGNORE_CLIENT_RESIZE)) {
		qubes_send_configure(view, box.width, box.height);
		wlr_log(WLR_DEBUG,
		        "Resized window %u: old size %u %u, new size %u %u",
		        (unsigned)view->window_id, view->last_width,
		        view->last_height, box.width, box.height);
		wlr_output_set_custom_mode(&view->output.output, box.width, box.height, 60000);
		view->last_width = box.width;
		view->last_height = box.height;
	}
	// wlr_output_enable(&view->output.output, true);
	assert(view->scene_output->output == &view->output.output);
	wlr_output_send_frame(&view->output.output);
}

static void qubes_toplevel_ack_configure(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_surface_configure *configure = data;
	struct tinywl_view *view = wl_container_of(listener, view, ack_configure);
	assert(QUBES_VIEW_MAGIC == view->magic);

	if (view->flags & QUBES_OUTPUT_IGNORE_CLIENT_RESIZE &&
	    view->configure_serial == configure->serial) {
		view->flags &= ~QUBES_OUTPUT_IGNORE_CLIENT_RESIZE;
		qubes_send_configure(view, view->last_width, view->last_height);
	}
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;
	assert(QUBES_SERVER_MAGIC == server->magic);
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL &&
	    xdg_surface->role != WLR_XDG_SURFACE_ROLE_POPUP) {
		/* this is handled by the new_popup listener */
		return;
	}
	/* QUBES HOOK: MSG_CREATE: create toplevel window */

	/* Allocate a tinywl_view for this surface */
	struct tinywl_view *view = calloc(1, sizeof(struct tinywl_view));
	if (!view)
		goto cleanup;
	view->server = server;
	/* Add wlr_output */
	qubes_output_init(&view->output, &server->backend->backend, server->wl_display);
	wlr_output_init_render(&view->output.output, server->allocator, server->renderer);
	if (!(view->scene = wlr_scene_create()))
		goto cleanup;

	assert(view->output.output.allocator);

	if (!(view->scene_output = wlr_scene_output_create(view->scene, &view->output.output)))
		goto cleanup;
	if (!(view->scene_subsurface_tree = wlr_scene_subsurface_tree_create(&view->scene_output->scene->node, xdg_surface->surface)))
		goto cleanup;
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
	xdg_surface->data = view;

	/* And listen to the various emits the toplevel can emit */
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		struct wlr_xdg_toplevel *const toplevel = xdg_surface->toplevel;
		view->request_maximize.notify = qubes_request_maximize;
		wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
		view->request_minimize.notify = qubes_request_minimize;
		wl_signal_add(&toplevel->events.request_minimize, &view->request_minimize);
		view->request_fullscreen.notify = qubes_request_fullscreen;
		wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
		view->set_title.notify = qubes_set_title;
		wl_signal_add(&toplevel->events.set_title, &view->set_title);
		view->set_app_id.notify = qubes_set_app_id;
		wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);
		view->ack_configure.notify = qubes_toplevel_ack_configure;
		wl_signal_add(&xdg_surface->events.ack_configure, &view->ack_configure);
	} else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_popup *const popup = xdg_surface->popup;
		struct wlr_box geometry = wlr_xdg_positioner_get_geometry(&popup->positioner);
		struct tinywl_view *parent_view = wlr_xdg_surface_from_wlr_surface(popup->parent)->data;
		assert(parent_view);
		view->left = geometry.x + parent_view->left;
		view->top = geometry.y + parent_view->top;
		view->last_width = geometry.width, view->last_height = geometry.height;
	} else {
		abort();
	}

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
	return;
cleanup:
	wl_resource_post_no_memory(xdg_surface->resource);
	if (view) {
		if (view->scene_subsurface_tree)
			wlr_scene_node_destroy(view->scene_subsurface_tree);
		if (view->scene_output)
			wlr_scene_output_destroy(view->scene_output);
		wlr_output_destroy(&view->output.output);
		qubes_rust_delete_id(view->server->backend->rust_backend, view->window_id);
		free(view);
	}
	return;
}

static int qubes_clean_exit(int signal_number, void *data)
{
	char *sig;
	switch (signal_number) {
	case SIGTERM:
		sig = "SIGTERM";
		break;
	case SIGHUP:
		sig = "SIGHUP";
		break;
	case SIGINT:
		sig = "SIGINT";
		break;
	default:
		abort();
	}
	wlr_log(WLR_ERROR, "Terminating due to signal %s", sig);
	wl_display_terminate(((struct tinywl_server *)data)->wl_display);
	return 0;
}
volatile sig_atomic_t crashing = 0;

static void sigpipe_handler(int signum, siginfo_t *siginfo, void *ucontext)
{
}

int main(int argc, char *argv[]) {

	const char *startup_cmd = NULL;
	int c, loglevel = WLR_ERROR;
	if (argc < 1) {
		fputs("NULL argv[0] passed\n", stderr);
		return 1;
	}

	while ((c = getopt(argc, argv, "vs:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'v':
			if (loglevel == WLR_ERROR)
				loglevel = WLR_INFO;
			else
				loglevel = WLR_DEBUG;
			break;
		default:
			printf("Usage: %s [-v] [-s startup command] [--] domid\n", argv[0]);
			return 0;
		}
	}
	if (optind != argc - 1) {
		printf("Usage: %s [-v] [-s startup command] [--] domid\n", argv[0]);
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

	struct sigaction sigpipe = {
		.sa_sigaction = sigpipe_handler,
		.sa_flags = SA_SIGINFO | SA_RESTART,
	}, old_sighnd;
	sigemptyset(&sigpipe.sa_mask);
	if (sigaction(SIGPIPE, &sigpipe, &old_sighnd)) {
		wlr_log(WLR_ERROR, "Cannot set up signal handler: %s", strerror(errno));
		return 1;
	}

	wlr_log_init(loglevel, NULL);
	struct tinywl_server *server = calloc(1, sizeof(*server));
	if (!server) {
		wlr_log(WLR_ERROR, "Cannot create tinywl_server");
		return 1;
	}
	server->magic = QUBES_SERVER_MAGIC;
	server->domid = domid;

	/* The allocator is the bridge between the renderer and the backend.
	 * It handles the buffer managment between the two, allowing wlroots
	 * to render onto the screen */
	if (!(server->allocator = qubes_allocator_create(domid))) {
		wlr_log(WLR_ERROR, "Cannot create Qubes allocator");
		return 1;
	}

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
	if (!(server->backend = qubes_backend_create(server->wl_display, domid, &server->views))) {
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

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	wlr_compositor_create(server->wl_display, server->renderer);
	wlr_data_device_manager_create(server->wl_display);

	/* Enable server-side decorations.  By default, Wayland clients decorate
	 * themselves, but that will lead to duplicate decorations on Qubes OS. */
	server->old_manager =
		wlr_server_decoration_manager_create(server->wl_display);
	if (server->old_manager) {
		wlr_server_decoration_manager_set_default_mode(server->old_manager,
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	}
	server->new_manager =
		wlr_xdg_decoration_manager_v1_create(server->wl_display);
	if (server->new_manager) {
		server->new_decoration.notify = qubes_new_decoration;
		wl_signal_add(&server->new_manager->events.new_toplevel_decoration,
		              &server->new_decoration);
	}

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

	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
	assert(loop);

	if (!(server->timer = wl_event_loop_add_timer(loop, qubes_send_frame_callbacks, server))) {
		wlr_log(WLR_ERROR, "Cannot create timer");
		return 1;
	}
	wl_event_source_timer_update(server->timer, 16);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(&server->backend->backend)) {
		wlr_backend_destroy(&server->backend->backend);
		wl_display_destroy(server->wl_display);
		return 1;
	}

	/*
	 * Add signal handlers for SIGTERM, SIGINT, and SIGHUP
	 */
	struct wl_event_source *sigterm = wl_event_loop_add_signal(loop,
		SIGTERM, qubes_clean_exit, server);
	struct wl_event_source *sigint = wl_event_loop_add_signal(loop,
		SIGINT, qubes_clean_exit, server);
	struct wl_event_source *sighup = wl_event_loop_add_signal(loop,
		SIGHUP, qubes_clean_exit, server);
	if (!sigterm || !sigint || !sighup) {
#ifdef QUBES_HAS_SYSTEMD
		sd_notifyf(0, "ERRNO=%d", errno);
#endif
		wlr_log(WLR_ERROR, "Cannot setup signal handlers");
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			unsetenv("NOTIFY_SOCKET");
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);
#ifdef QUBES_HAS_SYSTEMD
	if (sd_notify(0, "READY=1") < 0)
		return 1;
#endif
	wl_display_run(server->wl_display);

	/* Once wl_display_run returns, we shut down the server */
	wl_display_destroy_clients(server->wl_display);
	wl_event_source_remove(sighup);
	wl_event_source_remove(sigint);
	wl_event_source_remove(sigterm);

	struct tinywl_keyboard *keyboard_to_free, *tmp_keyboard;
	wl_list_for_each_safe(keyboard_to_free, tmp_keyboard, &server->keyboards, link) {
		wl_list_remove(&keyboard_to_free->key.link);
		wl_list_remove(&keyboard_to_free->modifiers.link);
		wl_list_remove(&keyboard_to_free->link);
		// wlr_input_device_destroy(keyboard_to_free->input_device);
		free(keyboard_to_free);
	}
	wlr_renderer_destroy(server->renderer);
	wlr_allocator_destroy(server->allocator);
	wlr_output_layout_destroy(server->output_layout);
	wl_display_destroy(server->wl_display);
	free(server);
	return 0;
}
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
