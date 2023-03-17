// vchan message dispatch and input event handling

#include "common.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include <xcb/xproto.h>

#ifdef QUBES_HAS_SYSTEMD
#include <systemd/sd-daemon.h>
#else
#define sd_notify(...)                                                         \
	do {                                                                        \
	} while (0)
#define sd_notifyf(...)                                                        \
	do {                                                                        \
	} while (0)
#endif

#include <qubes-gui-protocol.h>

#include "qubes_allocator.h"
#include "qubes_backend.h"
#include "qubes_clipboard.h"
#include "qubes_data_source.h"
#include "qubes_output.h"
#include "qubes_xwayland.h"

static void handle_keypress(struct qubes_output *output, uint32_t timestamp,
                            const uint8_t *ptr)
{
	struct msg_keypress keypress;
	enum wl_keyboard_key_state state;
	struct wlr_seat *seat = output->server->seat;
	struct qubes_backend *backend = output->server->backend;

	memcpy(&keypress, ptr, sizeof(keypress));
	switch (keypress.type) {
	case XCB_KEY_PRESS:
		state = WL_KEYBOARD_KEY_STATE_PRESSED;
		break;
	case XCB_KEY_RELEASE:
		state = WL_KEYBOARD_KEY_STATE_RELEASED;
		break;
	default:
		wlr_log(WLR_ERROR, "Bad keypress event type %" PRIu32, keypress.type);
		return; /* bad state */
	}

	if (keypress.keycode < 0x8 || keypress.keycode >= 0x108) {
		wlr_log(WLR_ERROR, "Bad keycode %" PRIu32, keypress.keycode);
		return; /* not valid in X11, which the GUI daemon uses */
	}

	const uint8_t keycode = keypress.keycode - 0x8;
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	assert(keyboard);
	uint8_t const i = keycode >> 3;
	uint8_t const j = keycode & 0x7;
	bool was_pressed = (backend->keymap.keys[i] >> j & 1) ^ (3 - keypress.type);
	backend->keymap.keys[i] =
	   (backend->keymap.keys[i] & ~(1 << j)) | (3 - keypress.type) << j;

	if (was_pressed) {
		struct wlr_keyboard_key_event event = {
			.time_msec = timestamp,
			.keycode = keycode,
			.update_state = true,
			.state = state,
		};
		wlr_keyboard_notify_key(keyboard, &event);
	}
}

static void handle_button(struct wlr_seat *seat, uint32_t timestamp,
                          const uint8_t *ptr)
{
	struct msg_button button;
	enum wlr_button_state state;

	memcpy(&button, ptr, sizeof(button));
	switch (button.type) {
	case XCB_BUTTON_PRESS:
		state = WLR_BUTTON_PRESSED;
		break;
	case XCB_BUTTON_RELEASE:
		state = WLR_BUTTON_RELEASED;
		break;
	default:
		wlr_log(WLR_ERROR, "Bad button event type %" PRIu32, button.type);
		return; /* bad state */
	}

	switch (button.button) {
	case XCB_BUTTON_INDEX_1:
		/* Left mouse button */
		wlr_seat_pointer_notify_button(seat, timestamp, 0x110, state);
		break;
	case XCB_BUTTON_INDEX_2:
		/* Right mouse button */
		wlr_seat_pointer_notify_button(seat, timestamp, 0x112, state);
		break;
	case XCB_BUTTON_INDEX_3:
		/* Middle mouse button */
		wlr_seat_pointer_notify_button(seat, timestamp, 0x111, state);
		break;
	case XCB_BUTTON_INDEX_4:
		/* Scroll up */
		wlr_seat_pointer_notify_axis(
		   seat, timestamp, WLR_AXIS_ORIENTATION_VERTICAL, -15.0,
		   -WLR_POINTER_AXIS_DISCRETE_STEP, WLR_AXIS_SOURCE_WHEEL);
		break;
	case XCB_BUTTON_INDEX_5:
		/* Scroll down */
		wlr_seat_pointer_notify_axis(
		   seat, timestamp, WLR_AXIS_ORIENTATION_VERTICAL, 15.0,
		   WLR_POINTER_AXIS_DISCRETE_STEP, WLR_AXIS_SOURCE_WHEEL);
		break;
	case 6: /* Scroll left */
		wlr_seat_pointer_notify_axis(
		   seat, timestamp, WLR_AXIS_ORIENTATION_HORIZONTAL, -15.0,
		   -WLR_POINTER_AXIS_DISCRETE_STEP, WLR_AXIS_SOURCE_WHEEL);
		break;
	case 7: /* Scroll right */
		wlr_seat_pointer_notify_axis(
		   seat, timestamp, WLR_AXIS_ORIENTATION_HORIZONTAL, 15.0,
		   WLR_POINTER_AXIS_DISCRETE_STEP, WLR_AXIS_SOURCE_WHEEL);
		break;
	default:
		wlr_log(WLR_DEBUG, "Unknown button event type %" PRIu32, button.button);
		return;
	}
	wlr_seat_pointer_send_frame(seat);
}

static void handle_pointer_movement(struct qubes_output *output, int32_t x,
                                    int32_t y, uint32_t timestamp,
                                    struct wlr_seat *seat)
{
	const double seat_relative_x = x + (double)output->x,
	             seat_relative_y = y + (double)output->y;
	double sx, sy;
	struct wlr_surface *surface = NULL;
	if (QUBES_VIEW_MAGIC == output->magic) {
		struct tinywl_view *view = wl_container_of(output, view, output);
		surface = wlr_xdg_surface_surface_at(view->xdg_surface, seat_relative_x,
		                                     seat_relative_y, &sx, &sy);
	} else if (QUBES_XWAYLAND_MAGIC == output->magic) {
		struct qubes_xwayland_view *view = wl_container_of(output, view, output);
		surface = view->xwayland_surface->surface;
		sx = x;
		sy = y;
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, timestamp, sx, sy);
	} else {
		wlr_seat_pointer_notify_clear_focus(seat);
	}
	wlr_seat_pointer_notify_frame(seat);
}

static void handle_motion(struct qubes_output *output, uint32_t timestamp,
                          const uint8_t *ptr)
{
	struct wlr_seat *seat = output->server->seat;
	struct msg_motion motion;
	memcpy(&motion, ptr, sizeof motion);
	handle_pointer_movement(output, motion.x, motion.y, timestamp, seat);
}

static void handle_crossing(struct qubes_output *output, uint32_t timestamp,
                            const uint8_t *ptr)
{
	struct msg_crossing crossing;
	struct wlr_seat *seat = output->server->seat;

	memcpy(&crossing, ptr, sizeof crossing);

	switch (crossing.type) {
	case XCB_ENTER_NOTIFY:
		handle_pointer_movement(output, crossing.x, crossing.y, timestamp, seat);
		return;
	case XCB_LEAVE_NOTIFY:
		wlr_seat_pointer_notify_clear_focus(seat);
		wlr_seat_pointer_notify_frame(seat);
		return;
	default:
		wlr_log(WLR_ERROR, "bad Crossing event type %" PRIu32, crossing.type);
		return;
	}
}

static void qubes_give_view_keyboard_focus(struct qubes_output *output,
                                           struct wlr_surface *surface)
{
	/* Note: this function only deals with keyboard focus. */
	struct tinywl_server *server = output->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */
		if (output->magic == QUBES_VIEW_MAGIC) {
			struct tinywl_view *view = wl_container_of(output, view, output);
			if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL &&
			    !view->xdg_surface->toplevel->pending.activated)
				wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);
		} else if (output->magic == QUBES_XWAYLAND_MAGIC) {
			struct qubes_xwayland_view *view =
			   wl_container_of(output, view, output);
			wlr_xwayland_surface_activate(view->xwayland_surface, true);
		} else {
			abort();
		}
		return;
	}
	wlr_log(WLR_INFO, "Giving keyboard focus to window %u", output->window_id);
	if (prev_surface) {
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret.
		 */
		if (wlr_surface_is_xdg_surface(prev_surface)) {
			struct wlr_xdg_surface *previous =
			   wlr_xdg_surface_from_wlr_surface(prev_surface);
			if (previous->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
				wlr_xdg_toplevel_set_activated(previous->toplevel, false);
			}
		} else {
			struct wlr_xwayland_surface *previous =
			   wlr_xwayland_surface_from_wlr_surface(prev_surface);
			if (previous)
				wlr_xwayland_surface_activate(previous, false);
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	assert(keyboard);
	/* Move the view to the front */
	wl_list_remove(&output->link);
	wl_list_insert(&server->views, &output->link);
	/* Activate the new surface */
	if (output->magic == QUBES_VIEW_MAGIC) {
		struct tinywl_view *view = wl_container_of(output, view, output);
		struct wlr_xdg_surface *view_surface = view->xdg_surface;
		while (view_surface && view_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
			view_surface =
			   wlr_xdg_surface_from_wlr_surface(view_surface->popup->parent);
		}
		if (view_surface && view_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
			wlr_xdg_toplevel_set_activated(view_surface->toplevel, true);
		}
	} else if (output->magic == QUBES_XWAYLAND_MAGIC) {
		struct qubes_xwayland_view *view = wl_container_of(output, view, output);
		wlr_xwayland_surface_activate(view->xwayland_surface, true);
	}
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	if (surface)
		wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
		                               keyboard->num_keycodes,
		                               &keyboard->modifiers);
}

static void handle_focus(struct qubes_output *output, uint32_t timestamp,
                         const uint8_t *ptr)
{
	/* This is specifically *keyboard* focus */
	struct msg_focus focus;
	struct wlr_seat *seat = output->server->seat;

	memcpy(&focus, ptr, sizeof focus);
	switch (focus.type) {
	case XCB_FOCUS_IN:
		wlr_log(WLR_INFO, "Window %" PRIu32 " has gained keyboard focus",
		        output->window_id);
		qubes_give_view_keyboard_focus(output, qubes_output_surface(output));
		break;
	case XCB_FOCUS_OUT:
		wlr_log(WLR_INFO, "Window %" PRIu32 " has lost keyboard focus",
		        output->window_id);
		if (seat->keyboard_state.focused_surface) {
			/*
			 * Deactivate the previously focused surface. This lets the client know
			 * it no longer has focus and the client will repaint accordingly, e.g.
			 * stop displaying a caret.
			 */
			if (wlr_surface_is_xdg_surface(seat->keyboard_state.focused_surface)) {
				struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
				   seat->keyboard_state.focused_surface);
				if (previous->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
					wlr_xdg_toplevel_set_activated(previous->toplevel, false);
				} else if (previous->role == WLR_XDG_SURFACE_ROLE_POPUP) {
					wlr_xdg_popup_destroy(previous->popup);
					assert(seat->keyboard_state.focused_surface == NULL);
				}
			} else {
				struct wlr_xwayland_surface *previous =
				   wlr_xwayland_surface_from_wlr_surface(
				      seat->keyboard_state.focused_surface);
				if (previous)
					wlr_xwayland_surface_activate(previous, false);
			}
		}
		wlr_seat_keyboard_notify_clear_focus(seat);
		break;
	default:
		wlr_log(WLR_ERROR, "Window %" PRIu32 ": Bad Focus event type %" PRIu32,
		        output->window_id, focus.type);
		return;
	}
}

static void handle_window_flags(struct qubes_output *output, const uint8_t *ptr)
{
	struct msg_window_flags flags;

	memcpy(&flags, ptr, sizeof flags);

	if (flags.flags_set & flags.flags_unset) {
		wlr_log(
		   WLR_ERROR,
		   "GUI daemon tried to set and unset the same flag on window %" PRIu32
		   "(flags_set: 0x%" PRIx32 ", flags_unset: 0x%" PRIx32 ")",
		   output->window_id, flags.flags_set, flags.flags_unset);
		return;
	}

	if (QUBES_VIEW_MAGIC != output->magic) {
		assert(QUBES_XWAYLAND_MAGIC == output->magic);
		wlr_log(WLR_ERROR,
		        "not yet implemented: setting flags for Xwayland surfaces");
		return;
	}

	struct tinywl_view *view = wl_container_of(output, view, output);

	if (view->xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wlr_log(
		   WLR_INFO,
		   "GUI daemon tried to change flags for non-toplevel window %" PRIu32
		   "(flags_set: 0x%" PRIx32 ", flags_unset: 0x%" PRIx32 ")",
		   output->window_id, flags.flags_set, flags.flags_unset);
		return;
	}

	if ((flags.flags_set | flags.flags_unset) & WINDOW_FLAG_FULLSCREEN)
		wlr_xdg_toplevel_set_fullscreen(view->xdg_surface->toplevel,
		                                flags.flags_set & WINDOW_FLAG_FULLSCREEN);

	// Setting the "minimized" flag directly would be better, but xdg-shell
	// doesn't support that
	if ((flags.flags_set | flags.flags_unset) & WINDOW_FLAG_MINIMIZE)
		wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel,
		                               !(flags.flags_set & WINDOW_FLAG_MINIMIZE));

	// DEMANDS_ATTENTION has no Wayland analog
}

static void handle_configure(struct qubes_output *output, uint32_t timestamp,
                             const uint8_t *ptr)
{
	struct msg_configure configure;

	memcpy(&configure, ptr, sizeof(configure));
	// Just ACK the configure
	wlr_log(WLR_DEBUG,
	        "handle_configure: old rect x=%d y=%d w=%u h=%u, new rect x=%d y=%d "
	        "x=%u y=%u",
	        output->left, output->top, output->last_width, output->last_height,
	        configure.x, configure.y, configure.width, configure.height);
	output->left = configure.x;
	output->top = configure.y;
	if (configure.width == (uint32_t)output->last_width &&
	    configure.height == (uint32_t)output->last_height) {
		// Just ACK without doing anything
		qubes_send_configure(output, configure.width, configure.height);
		return;
	}

	if (configure.width <= 0 || configure.height <= 0 ||
	    configure.width > MAX_WINDOW_WIDTH ||
	    configure.height > MAX_WINDOW_HEIGHT) {
		wlr_log(WLR_ERROR,
		        "Bad configure from GUI daemon: width %" PRIu32 " height %" PRIu32
		        " window %" PRIu32,
		        configure.x, configure.y, output->window_id);
		// this should never happen, but better to ACK the configure
		// than to crash; return to avoid giving clients an invalid state
		qubes_send_configure(output, configure.width, configure.height);
		return;
	}

	output->last_width = configure.width, output->last_height = configure.height;
	wlr_output_set_custom_mode(&output->output, configure.width,
	                           configure.height, 60000);

	if (QUBES_VIEW_MAGIC == output->magic) {
		output->flags |= QUBES_OUTPUT_IGNORE_CLIENT_RESIZE;
		struct tinywl_view *view = wl_container_of(output, view, output);
		if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
			view->configure_serial = wlr_xdg_toplevel_set_size(
			   view->xdg_surface->toplevel, configure.width, configure.height);
			wlr_log(WLR_DEBUG,
			        "Will ACK configure from GUI daemon (width %u, height %u)"
			        " when client ACKS configure with serial %u",
			        configure.width, configure.height, view->configure_serial);
		} else {
			// There won’t be a configure event ACKd by the client, so
			// ACK early
			wlr_log(WLR_DEBUG,
			        "Got a configure event for non-toplevel window %" PRIu32
			        "; returning early",
			        output->window_id);
			qubes_send_configure(output, configure.width, configure.height);
		}
	} else if (QUBES_XWAYLAND_MAGIC == output->magic) {
		struct qubes_xwayland_view *view = wl_container_of(output, view, output);
		wlr_xwayland_surface_configure(view->xwayland_surface, configure.x,
		                               configure.y, configure.width,
		                               configure.height);
		// There won’t be a configure event ACKd by the client, so
		// ACK early.  Neglecting this for Xwayland cost two weeks of debugging.
		qubes_send_configure(output, configure.width, configure.height);
	} else {
		abort();
	}
}

static void handle_clipboard_data(struct qubes_output *output, uint32_t len,
                                  const uint8_t *ptr)
{
	struct tinywl_server *server = output->server;
	assert(server);
	struct wlr_seat *seat = server->seat;
	assert(seat);
	struct qubes_data_source *source =
	   qubes_data_source_create(server->wl_display, len, ptr);
	wlr_seat_set_selection(server->seat, (struct wlr_data_source *)source,
	                       wl_display_get_serial(server->wl_display));
}

static void handle_clipboard_request(struct qubes_output *output)
{
	struct tinywl_server *server = output->server;
	assert(server);
	struct wlr_seat *seat = server->seat;
	assert(seat);
	if (!seat->selection_source)
		return; /* Nothing to do */
	struct wlr_data_source *const source = seat->selection_source;
	char **mime_type;
	wl_array_for_each (mime_type, &source->mime_types) {
		// MIME types are already sanitized against injection attacks
		wlr_log(WLR_DEBUG, "Received event of MIME type %s", *mime_type);
		if (strcmp(*mime_type, "text/plain"))
			continue;
		int pipefds[2], res = pipe2(pipefds, O_CLOEXEC | O_NONBLOCK), ctrl = 0;
		if (res == -1)
			return;
		assert(res == 0);
		struct qubes_clipboard_handler *handler =
		   qubes_clipboard_handler_create(server, pipefds[0]);
		if (handler) {
			res = ioctl(pipefds[1], FIONBIO, &ctrl);
			assert(res == 0);
			wlr_data_source_send(source, *mime_type, pipefds[1]);
		} else {
			assert(close(pipefds[1]) == 0);
		}
		return;
	}
}

// Called when the GUI agent has reconnected to the daemon.
static void qubes_recreate_window(struct qubes_output *output)
{
	struct wlr_box box;
	switch (output->magic) {
	case QUBES_VIEW_MAGIC: {
		struct tinywl_view *view = wl_container_of(output, view, output);
		wlr_xdg_surface_get_geometry(view->xdg_surface, &box);
		break;
	}
	case QUBES_XWAYLAND_MAGIC: {
		struct qubes_xwayland_view *view = wl_container_of(output, view, output);
		box.x = view->xwayland_surface->x;
		box.y = view->xwayland_surface->y;
		box.width = view->xwayland_surface->width;
		box.height = view->xwayland_surface->height;
		break;
	}
	default:
		assert(!"Invalid output type");
		abort();
	}

	output->last_width = box.width, output->last_height = box.height;
	if (!qubes_output_ensure_created(output, box))
		return;
	qubes_send_configure(output, box.width, box.height);
	if (output->buffer) {
		qubes_output_dump_buffer(output, box, NULL);
	}
	if (!qubes_output_mapped(output))
		return;
	switch (output->magic) {
	case QUBES_VIEW_MAGIC: {
		struct tinywl_view *view = wl_container_of(output, view, output);
		qubes_view_map(view);
		break;
	}
	case QUBES_XWAYLAND_MAGIC: {
		struct qubes_xwayland_view *view = wl_container_of(output, view, output);
		qubes_xwayland_surface_map(view);
		break;
	}
	default:
		assert(!"Invalid output type");
		abort();
	}
}

static void qubes_reconnect(struct qubes_backend *const backend,
                            uint32_t const msg_type,
                            uint32_t const protocol_version)
{
	extern bool qubes_rust_reconnect(struct qubes_rust_backend * backend);
	switch (msg_type) {
	case 2: {
		unsigned int const major_version = protocol_version >> 16;
		unsigned int const minor_version = protocol_version & 0xFFFF;
		sd_notifyf(
		   0, "READY=1\nSTATUS=GUI daemon reconnected, protocol version %u.%u\n",
		   major_version, minor_version);
		wlr_log(WLR_INFO, "GUI daemon reconnected, protocol version %u.%u\n",
		        major_version, minor_version);
		struct qubes_output *output;
		wl_list_for_each (output, backend->views, link) {
			output->flags &= ~QUBES_OUTPUT_CREATED;
		}
		wl_list_for_each (output, backend->views, link) {
			assert(!(output->flags & QUBES_OUTPUT_CREATED));
			qubes_recreate_window(output);
		}
		return;
	}
	case 1:
		sd_notify(0, "STATUS=GUI daemon disconnected, trying to reconnect\n");
		wlr_log(WLR_INFO, "Must reconnect to GUI daemon");
		// GUI agent needs reconnection
		if (backend->source)
			wl_event_source_remove(backend->source);
		backend->source = NULL;
		if (!qubes_rust_reconnect(backend->rust_backend)) {
			sd_notify(0, "STATUS=Cound not reconnect to GUI daemon, exiting!\n");
			wlr_log(WLR_ERROR, "Fatal error: cannot reconnect to GUI daemon");
			wl_display_terminate(backend->display);
			return;
		}
		int fd = qubes_rust_backend_fd(backend->rust_backend);
		struct wl_event_loop *loop = wl_display_get_event_loop(backend->display);
		backend->source = wl_event_loop_add_fd(
		   loop, fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		   qubes_backend_on_fd, backend);
		if (!backend->source) {
			sd_notifyf(
			   0, "STATUS=Cannot re-register vchan file descriptor: %s\nERRNO=%d",
			   strerror(errno), errno);
			wlr_log(WLR_ERROR,
			        "Fatal error: Cannot re-register vchan file descriptor");
			wl_display_terminate(backend->display);
		}
		return;
	case 3:
		sd_notify(0,
		          "STATUS=Protocol error occurred, but no need to reconnect "
		          "(fatal)\nERRNO=%d",
		          EPROTO);
		wl_display_terminate(backend->display);
		return;
	default:
		abort();
	}
}

void qubes_parse_event(void *raw_backend, void *raw_view, uint32_t timestamp,
                       struct msg_hdr hdr, const uint8_t *ptr)
{
	struct qubes_backend *backend = raw_backend;
	QUBES_STATIC_ASSERT(offsetof(struct tinywl_view, output) == 0);
	struct qubes_output *output = raw_view;

	assert(raw_backend);
	if (!ptr) {
		// This is a fake message generated by the Rust code for internal
		// communication purposes.  It indicates that the status of the
		// connection to the GUI daemon has changed.
		assert(hdr.type == 0);
		qubes_reconnect(backend, hdr.untrusted_len, hdr.window);
		return;
	}

	if (!output) {
		if (hdr.type != MSG_KEYMAP_NOTIFY) {
			wlr_log(WLR_ERROR, "No window for message of type %" PRIu32, hdr.type);
			return;
		}
		struct wlr_keyboard *keyboard = backend->keyboard;
		assert(keyboard);
		for (int i = 0; i < 32; ++i) {
			for (int j = 0; j < 8; ++j) {
				bool is_pressed = ptr[i] & 1 << j;
				bool old_pressed = backend->keymap.keys[i] & 1 << j;
				if (!old_pressed || is_pressed)
					continue;
				backend->keymap.keys[i] ^= 1 << j;
				struct wlr_keyboard_key_event event = {
					.time_msec = timestamp,
					.keycode = i << 3 | j,
					.update_state = true,
					.state = is_pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
					                    : WL_KEYBOARD_KEY_STATE_RELEASED,
				};
				wlr_keyboard_notify_key(keyboard, &event);
			}
		}
		assert(hdr.untrusted_len == sizeof(struct msg_keymap_notify));
		static_assert(sizeof(backend->keymap) == sizeof(struct msg_keymap_notify),
		              "wrong size");
		memcpy(&backend->keymap, ptr, hdr.untrusted_len);
		return;
	}
	assert(hdr.window == output->window_id);
	switch (hdr.type) {
	case MSG_KEYPRESS:
		assert(hdr.untrusted_len == sizeof(struct msg_keypress));
		handle_keypress(output, timestamp, ptr);
		break;
	case MSG_CONFIGURE:
		assert(hdr.untrusted_len == sizeof(struct msg_configure));
		handle_configure(output, timestamp, ptr);
		break;
	case MSG_MAP:
		break;
	case MSG_BUTTON:
		assert(hdr.untrusted_len == sizeof(struct msg_button));
		handle_button(output->server->seat, timestamp, ptr);
		break;
	case MSG_MOTION:
		assert(hdr.untrusted_len == sizeof(struct msg_motion));
		handle_motion(output, timestamp, ptr);
		break;
	case MSG_CLOSE:
		assert(hdr.untrusted_len == 0);
		switch (output->magic) {
		case QUBES_VIEW_MAGIC: {
			struct tinywl_view *view = wl_container_of(output, view, output);
			if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
				wlr_xdg_toplevel_send_close(view->xdg_surface->toplevel);
			else if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP)
				wlr_xdg_popup_destroy(view->xdg_surface->popup);
			break;
		}
		case QUBES_XWAYLAND_MAGIC: {
			struct qubes_xwayland_view *view =
			   wl_container_of(output, view, output);
			wlr_xwayland_surface_close(view->xwayland_surface);
			break;
		default:
			assert(!"Invalid output type");
		}
		}
		break;
	case MSG_CROSSING:
		assert(hdr.untrusted_len == sizeof(struct msg_crossing));
		handle_crossing(output, timestamp, ptr);
		break;
	case MSG_FOCUS:
		assert(hdr.untrusted_len == sizeof(struct msg_focus));
		handle_focus(output, timestamp, ptr);
		break;
	case MSG_CLIPBOARD_REQ:
		assert(hdr.untrusted_len == 0);
		handle_clipboard_request(output);
		break;
	case MSG_CLIPBOARD_DATA:
		handle_clipboard_data(output, hdr.untrusted_len, ptr);
		break;
	case MSG_KEYMAP_NOTIFY:
		assert(hdr.untrusted_len == sizeof(struct msg_keymap_notify));
		break;
	case MSG_WINDOW_FLAGS:
		assert(hdr.untrusted_len == sizeof(struct msg_window_flags));
		handle_window_flags(output, ptr);
		break;
	case MSG_DESTROY:
		assert(0 && "handled by Rust code");
		break;
#define MSG_WINDOW_DUMP_ACK 149
	case MSG_WINDOW_DUMP_ACK: {
		if (output->server->protocol_version < 0x10007) {
			wlr_log(
			   WLR_ERROR,
			   "Daemon sent MSG_WINDOW_DUMP_ACK but protocol version is %" PRIu32
			   "(less than 0x10007)",
			   output->server->protocol_version);
			break;
		}
		struct qubes_link *link = output->server->queue_head;
		if (link == NULL) {
			wlr_log(WLR_ERROR,
			        "Daemon sent too many MSG_WINDOW_DUMP_ACK messages");
			break;
		}
		assert(output->server->queue_tail != NULL);
		if ((output->server->queue_head = link->next) == NULL) {
			assert(link == output->server->queue_tail);
			output->server->queue_tail = NULL;
		} else {
			assert(link != output->server->queue_tail);
		}
		qubes_buffer_destroy(&link->buffer->inner);
		free(link);
		break;
	}
	case MSG_RESIZE:
	case MSG_CREATE:
	case MSG_UNMAP:
	case MSG_MFNDUMP:
	case MSG_SHMIMAGE:
	case MSG_EXECUTE:
	case MSG_WMNAME:
	case MSG_WINDOW_DUMP:
	case MSG_CURSOR:
	default:
		/* unknown events */
		break;
	}
}

/* vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8: */
