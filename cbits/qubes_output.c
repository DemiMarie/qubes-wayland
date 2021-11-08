#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <wayland-server-core.h>

#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include <drm/drm_fourcc.h>
#ifdef BUILD_RUST
#include <qubes-gui-protocol.h>
#endif
#include "qubes_output.h"
#include "qubes_allocator.h"
#include "qubes_backend.h"
#include "qubes_clipboard.h"
#include "qubes_data_source.h"
#include "main.h"

/* Qubes OS doesn’t support gamma LUTs */
static size_t qubes_get_gamma_size(
		struct wlr_output *output __attribute__((unused))) {
	return 0;
}

static void qubes_unlink_buffer(struct qubes_output *buffer) {
	if (buffer->buffer)
		wl_list_remove(&buffer->buffer_destroy.link);
	buffer->buffer = NULL;
}

static void qubes_unlink_buffer_listener(struct wl_listener *listener,
                                         void *data __attribute__((unused))) {
	struct qubes_output *output = wl_container_of(listener, output, buffer_destroy);
	qubes_unlink_buffer(output);
}

static const struct wlr_output_impl qubes_wlr_output_impl;

static void qubes_output_deinit(struct wlr_output *raw_output) {
	assert(raw_output->impl == &qubes_wlr_output_impl);
	struct qubes_output *output = wl_container_of(raw_output, output, output);
	wl_list_remove(&output->frame.link);
	qubes_unlink_buffer(output);
}

static bool qubes_output_test(struct wlr_output *raw_output) {
	assert(raw_output->impl == &qubes_wlr_output_impl);
	struct qubes_output *output = wl_container_of(raw_output, output, output);
	if ((raw_output->pending.committed & WLR_OUTPUT_STATE_BUFFER) &&
	    (raw_output->pending.buffer != NULL) &&
	    (raw_output->pending.buffer->impl != qubes_buffer_impl_addr))
		return false;
	return true;
}

#ifdef BUILD_RUST
static void qubes_output_damage(struct tinywl_view *view, struct wlr_box box) {
	struct qubes_output *output = &view->output;
	wlr_log(WLR_DEBUG, "X is %d Y is %d Width is %" PRIu32 " height is %" PRIu32, (int)box.x, (int)box.y, (uint32_t)box.width, (uint32_t)box.height);
	int n_rects = 0;
	if (!(output->output.pending.committed & WLR_OUTPUT_STATE_DAMAGE))
		return;
	pixman_box32_t *rects = pixman_region32_rectangles(&output->output.pending.damage, &n_rects);
	if (n_rects <= 0 || !rects) {
		wlr_log(WLR_DEBUG, "No damage!");
		return;
	}
	wlr_log(WLR_DEBUG, "Sending MSG_SHMIMAGE (0x%x) to window %" PRIu32, MSG_SHMIMAGE, view->window_id);
	for (int i = 0; i < n_rects; ++i) {
		int32_t width, height;
		// this is the correct approach ― the alternative leads to glitches
		static const bool use_delta = false;
		int32_t x1 = use_delta ? QUBES_MAX(rects[i].x1, box.x) : rects[i].x1;
		int32_t y1 = use_delta ? QUBES_MAX(rects[i].y1, box.y) : rects[i].y1;
		if (__builtin_sub_overflow(rects[i].x2, x1, &width) ||
		    __builtin_sub_overflow(rects[i].y2, y1, &height)) {
			wlr_log(WLR_ERROR, "Overflow in damage calc");
			return;
		}
		if (width <= 0 || height <= 0) {
			wlr_log(WLR_ERROR, "Negative width or height - skipping");
			continue;
		}
		int32_t const x = use_delta ? x1 - box.x : x1, y = use_delta ? y1 - box.y : y1;
		wlr_log(WLR_DEBUG, "Submitting damage to GUI daemon: x %" PRIi32 " y %" PRIi32 " width %" PRIu32 " height %" PRIu32, x, y, width, height);
		struct {
			struct msg_hdr header;
			struct msg_shmimage shmimage;
		} new_msg = {
			.header = {
				.type = MSG_SHMIMAGE,
				.window = view->window_id,
				.untrusted_len = sizeof(struct msg_shmimage),
			},
			.shmimage = { .x = x1, .y = y1, .width = width, .height = height },
		};
		// Created above
		qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&new_msg);
	}
}
#endif

static void qubes_output_dump_buffer(struct tinywl_view *view, struct wlr_box box)
{
	struct qubes_output *output = &view->output;
	assert(output->buffer->impl == qubes_buffer_impl_addr);
	wl_signal_add(&output->buffer->events.destroy, &output->buffer_destroy);
#ifdef BUILD_RUST
	wlr_log(WLR_DEBUG, "Sending MSG_WINDOW_DUMP (0x%x) to window %" PRIu32, MSG_WINDOW_DUMP, view->window_id);
	struct qubes_buffer *buffer = wl_container_of(output->buffer, buffer, inner);
	buffer->header.window = view->window_id;
	buffer->header.type = MSG_WINDOW_DUMP;
	buffer->header.untrusted_len = sizeof(buffer->qubes) + NUM_PAGES(buffer->size) * SIZEOF_GRANT_REF;
	qubes_rust_send_message(view->server->backend->rust_backend, &buffer->header);
	qubes_output_damage(view, box);
#endif
}

static bool qubes_output_commit(struct wlr_output *raw_output) {
	assert(raw_output->impl == &qubes_wlr_output_impl);
	struct qubes_output *output = wl_container_of(raw_output, output, output);
	struct tinywl_view *view = wl_container_of(output, view, output);
	assert(QUBES_VIEW_MAGIC == view->magic);
	struct wlr_box box;
	if (!qubes_output_ensure_created(view, &box))
		return false;

	if (raw_output->pending.committed & WLR_OUTPUT_STATE_MODE) {
		assert(raw_output->pending.mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
		wlr_output_update_custom_mode(raw_output,
			raw_output->pending.custom_mode.width,
			raw_output->pending.custom_mode.height,
			raw_output->pending.custom_mode.refresh);
#ifdef BUILD_RUST
		qubes_send_configure(
			view,
			raw_output->pending.custom_mode.width,
			raw_output->pending.custom_mode.height
		);
#endif
	}

	if ((raw_output->pending.committed & WLR_OUTPUT_STATE_BUFFER) &&
	    (output->buffer != raw_output->pending.buffer)) {
		if (output->buffer)
			wl_list_remove(&output->buffer_destroy.link);
		output->buffer = raw_output->pending.buffer;
		if (output->buffer) {
			qubes_output_dump_buffer(view, box);
		}
	}
	wlr_output_update_enabled(raw_output, raw_output->pending.enabled);
	return true;
}

static const struct wlr_drm_format xrgb8888 = {
	.format = DRM_FORMAT_XRGB8888,
	.len = 0,
	.cap = WLR_BUFFER_CAP_DATA_PTR,
};

static const struct wlr_drm_format *global_pointer = &xrgb8888;
static const struct wlr_drm_format_set global_formats = {
	.len = 1,
	.cap = WLR_BUFFER_CAP_DATA_PTR,
	.formats = (struct wlr_drm_format **)&global_pointer,
};

static const struct wlr_drm_format_set *qubes_output_get_primary_formats(
		struct wlr_output *output __attribute__((unused)), uint32_t buffer_caps) {
	if (!(buffer_caps & WLR_BUFFER_CAP_DATA_PTR))
		return NULL;
	return &global_formats;
}

static const struct wlr_output_impl qubes_wlr_output_impl = {
	.set_cursor = NULL,
	.move_cursor = NULL,
	.destroy = qubes_output_deinit,
	.test = qubes_output_test,
	.commit = qubes_output_commit,
	.get_gamma_size = qubes_get_gamma_size,
	.get_cursor_formats = NULL,
	.get_cursor_size = NULL,
	.get_primary_formats = qubes_output_get_primary_formats,
};

static void qubes_output_frame(struct wl_listener *listener, void *data __attribute__((unused))) {
	struct qubes_output *output = wl_container_of(listener, output, frame);
	struct tinywl_view *view = wl_container_of(output, view, output);
	assert(QUBES_VIEW_MAGIC == view->magic);
	if (wlr_scene_output_commit(view->scene_output)) {
		output->output.frame_pending = true;
		if (!view->server->frame_pending) {
			// Schedule another timer callback
			wl_event_source_timer_update(view->server->timer, 16);
			view->server->frame_pending = true;
		}
	}
}

void qubes_output_init(struct qubes_output *output, struct wlr_backend *backend, struct wl_display *display) {
	memset(output, 0, sizeof *output);

	wlr_output_init(&output->output, backend, &qubes_wlr_output_impl, display);
	wlr_output_update_custom_mode(&output->output, 1280, 720, 0);
	wlr_output_update_enabled(&output->output, true);
	wlr_output_set_description(&output->output, "Qubes OS virtual output");

	output->buffer = NULL;
	output->buffer_destroy.notify = qubes_unlink_buffer_listener;
	output->formats = &global_formats;
	output->frame.notify = qubes_output_frame;
	wl_signal_add(&output->output.events.frame, &output->frame);
}

static void handle_keypress(struct tinywl_view *view, uint32_t timestamp, const uint8_t *ptr)
{
	struct msg_keypress keypress;
	enum wl_keyboard_key_state state;
	struct wlr_seat *seat = view->server->seat;
	struct qubes_backend *backend = view->server->backend;

	memcpy(&keypress, ptr, sizeof(keypress));
	switch (keypress.type) {
	case 2: // KeyPress
		state = WL_KEYBOARD_KEY_STATE_PRESSED;
		break;
	case 3: // KeyRelease
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
	backend->keymap.keys[i] = (backend->keymap.keys[i] & ~(1 << j)) | (3 - keypress.type) << j;

	if (!was_pressed) {
		return;
	}
	struct wlr_event_keyboard_key event = {
		.time_msec = timestamp,
		.keycode = keypress.keycode - 8,
		.update_state = true,
		.state = state,
	};
	wlr_keyboard_notify_key(keyboard, &event);
}

static void handle_button(struct wlr_seat *seat, uint32_t timestamp, const uint8_t *ptr)
{
	struct msg_button button;
	enum wlr_button_state state;

	memcpy(&button, ptr, sizeof(button));
	switch (button.type) {
	case 4: // ButtonPress
		state = WLR_BUTTON_PRESSED;
		break;
	case 5: // ButtonRelease
		state = WLR_BUTTON_RELEASED;
		break;
	default:
		wlr_log(WLR_ERROR, "Bad button event type %" PRIu32, button.type);
		return; /* bad state */
	}

	switch (button.button) {
	case 1:
		wlr_seat_pointer_notify_button(seat, timestamp, 0x110, state);
		break;
	case 2:
		wlr_seat_pointer_notify_button(seat, timestamp, 0x112, state);
		break;
	case 3:
		wlr_seat_pointer_notify_button(seat, timestamp, 0x111, state);
		break;
	case 4:
		wlr_seat_pointer_notify_axis(
			seat, timestamp, WLR_AXIS_ORIENTATION_VERTICAL, -1.0, -1,
			WLR_AXIS_SOURCE_WHEEL);
		break;
	case 5:
		wlr_seat_pointer_notify_axis(
			seat, timestamp, WLR_AXIS_ORIENTATION_VERTICAL, 1.0, 1,
			WLR_AXIS_SOURCE_WHEEL);
		break;
	case 6:
		wlr_seat_pointer_notify_axis(
			seat, timestamp, WLR_AXIS_ORIENTATION_HORIZONTAL, -1.0, -1,
			WLR_AXIS_SOURCE_WHEEL);
		break;
	case 7:
		wlr_seat_pointer_notify_axis(
			seat, timestamp, WLR_AXIS_ORIENTATION_HORIZONTAL, 1.0, 1,
			WLR_AXIS_SOURCE_WHEEL);
		break;
	default:
		wlr_log(WLR_DEBUG, "Unknown button event type %" PRIu32, button.button);
		return;
	}
	wlr_seat_pointer_send_frame(seat);
}

static void handle_motion(struct tinywl_view *view, uint32_t timestamp, const uint8_t *ptr)
{
	struct wlr_seat *seat = view->server->seat;
	struct msg_motion motion;
	memcpy(&motion, ptr, sizeof motion);
	wlr_seat_pointer_send_motion(seat, timestamp,
	                            (double)motion.x + (double)view->x,
	                            (double)motion.y + (double)view->y);
	wlr_seat_pointer_send_frame(seat);
}

static void handle_crossing(struct tinywl_view *view, uint32_t timestamp __attribute__((unused)), const uint8_t *ptr)
{
	struct msg_crossing crossing;
	struct wlr_seat *seat = view->server->seat;
	int display_width = MAX_WINDOW_WIDTH;
	int display_height = MAX_WINDOW_HEIGHT;

	memcpy(&crossing, ptr, sizeof crossing);

	switch (crossing.type) {
	case 7: // EnterNotify
		break;
	case 8: // LeaveNotify
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	default:
		wlr_log(WLR_ERROR, "bad Crossing event type %" PRIu32, crossing.type);
		return;
	}

	assert(display_width > 0 && display_height > 0);
	assert(display_width <= MAX_WINDOW_WIDTH && display_height <= MAX_WINDOW_HEIGHT);
	wlr_seat_pointer_notify_enter(seat, view->xdg_surface->surface,
	                              (double)crossing.x + (double)view->x,
	                              (double)crossing.y + (double)view->y);
	wlr_seat_pointer_send_frame(seat);
}

static void handle_focus(struct tinywl_view *view, uint32_t timestamp, const uint8_t *ptr)
{
	/* This is specifically *keyboard* focus */
	struct msg_focus focus;
	struct wlr_seat *seat = view->server->seat;

	memcpy(&focus, ptr, sizeof focus);
	switch (focus.type) {
	case 9: // FocusIn
		qubes_give_view_keyboard_focus(view, view->xdg_surface->surface);
		break;
	case 10: // FocusOut
		if (seat->keyboard_state.focused_surface) {
			/*
			 * Deactivate the previously focused surface. This lets the client know
			 * it no longer has focus and the client will repaint accordingly, e.g.
			 * stop displaying a caret.
			 */
			struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
						seat->keyboard_state.focused_surface);
			if (previous->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
				wlr_xdg_toplevel_set_activated(previous, false);
			seat->keyboard_state.focused_surface = NULL;
		}
		wlr_seat_keyboard_notify_clear_focus(seat);
		break;
	default:
		wlr_log(WLR_ERROR, "Bad Focus event type %" PRIu32, focus.type);
		return;
	}
}

void qubes_send_configure(struct tinywl_view *view, uint32_t width, uint32_t height)
{
	if (!qubes_output_need_configure(view))
		return;
	if (width <= 0 || height <= 0)
		return;
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
			.x = view->left,
			.y = view->top,
			.width = width,
			.height = height,
			.override_redirect = 0,
		},
	};
	view->flags &= ~(__typeof__(view->flags))QUBES_OUTPUT_NEED_CONFIGURE;
	qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr*)&msg);
}

static void handle_configure(struct tinywl_view *view, uint32_t timestamp __attribute__((unused)), const uint8_t *ptr)
{
	struct msg_configure configure;
	memcpy(&configure, ptr, sizeof(configure));
	view->left = configure.x;
	view->top = configure.y;
	// Just ACK the configure
	view->flags |= (__typeof__(view->flags))QUBES_OUTPUT_NEED_CONFIGURE;
	qubes_send_configure(view, configure.width, configure.height);
	if (configure.width == (uint32_t)view->last_width &&
	    configure.height == (uint32_t)view->last_height) {
		return;
	}
	if (configure.width <= 0 || configure.height <= 0) {
		wlr_log(WLR_ERROR,
		        "Bad configure from GUI daemon: width %" PRIu32 " height %" PRIu32 " window %" PRIu32,
		        configure.x,
		        configure.y,
		        view->window_id);
		return;
	}
	wlr_output_update_custom_mode(&view->output.output, configure.width, configure.height, 0);
	if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		wlr_xdg_toplevel_set_size(view->xdg_surface, configure.width, configure.height);
	else
		assert(0 && "not implemented");
}

static void handle_clipboard_data(struct tinywl_view *view, uint32_t len, const uint8_t *ptr)
{
	struct tinywl_server *server = view->server;
	assert(server);
	struct wlr_seat *seat = server->seat;
	assert(seat);
	struct qubes_data_source *source = qubes_data_source_create(server->wl_display, len, ptr);
	wlr_seat_set_selection(server->seat, (struct wlr_data_source *)source, wl_display_get_serial(server->wl_display));
}

static void handle_clipboard_request(struct tinywl_view *view)
{
	struct tinywl_server *server = view->server;
	assert(server);
	struct wlr_seat *seat = server->seat;
	assert(seat);
	if (!seat->selection_source)
		return; /* Nothing to do */
	struct wlr_data_source *const source = seat->selection_source;
	char **mime_type;
	wl_array_for_each(mime_type, &source->mime_types) {
		// MIME types are already sanitized against injection attacks
		wlr_log(WLR_DEBUG, "Received event of MIME type %s", *mime_type);
		if (!strcmp(*mime_type, "text/plain")) {
			int pipefds[2], res = pipe2(pipefds, O_CLOEXEC|O_NONBLOCK), ctrl = 0;
			if (res == -1)
				return;
			assert(res == 0);
			res = ioctl(pipefds[1], FIONBIO, &ctrl);
			assert(res == 0);
			struct qubes_clipboard_handler *handler = qubes_clipboard_handler_create(server, pipefds[0]);
			if (handler) {
				wlr_data_source_send(source, *mime_type, pipefds[1]);
			} else {
				assert(close(pipefds[1]) == 0);
			}
			return;
		}
	}
}

// Called when the GUI agent has reconnected to the daemon.
static void qubes_recreate_window(struct tinywl_view *view)
{
	struct wlr_box box;
	if (!qubes_output_ensure_created(view, &box)) {
		return;
	}
	view->last_width = box.width, view->last_height = box.height;
	qubes_send_configure(view, box.width, box.height);
	if (view->output.buffer) {
		// qubes_output_dump_buffer assumes this
		wl_list_remove(&view->output.buffer_destroy.link);
		qubes_output_dump_buffer(view, box);
	}
	if (qubes_output_mapped(view)) {
		qubes_view_map(view);
	}
}

extern bool qubes_rust_reconnect(struct qubes_rust_backend *backend);

void qubes_parse_event(void *raw_backend, void *raw_view, uint32_t timestamp, struct msg_hdr hdr, const uint8_t *ptr)
{
	struct qubes_backend *backend = raw_backend;
	struct tinywl_view *view = raw_view;

	assert(raw_backend);
	if (!ptr) {
		if (hdr.untrusted_len == 2) {
			wlr_log(WLR_DEBUG, "Reconnecting to GUI daemon");
			struct tinywl_view *view;
			wl_list_for_each(view, backend->views, link) {
				assert(QUBES_VIEW_MAGIC == view->magic);
				view->flags &= ~QUBES_OUTPUT_CREATED;
				view->flags |= QUBES_OUTPUT_NEED_CONFIGURE;
			}
			wl_list_for_each(view, backend->views, link) {
				assert(QUBES_VIEW_MAGIC == view->magic);
				assert(!(view->flags & QUBES_OUTPUT_CREATED));
				qubes_recreate_window(view);
			}
			return;
		} else if (hdr.untrusted_len == 1) {
			wlr_log(WLR_DEBUG, "Must reconnect to GUI daemon");
		} else {
			assert(hdr.untrusted_len == 3);
			wl_display_terminate(backend->display);
			return;
		}
		// GUI agent needs reconnection
		wl_event_source_remove(backend->source);
		backend->source = NULL;
		if (!qubes_rust_reconnect(backend->rust_backend)) {
			wlr_log(WLR_ERROR, "Fatal error: cannot reconnect to GUI daemon");
			wl_display_terminate(backend->display);
			return;
		}
		int fd = qubes_rust_backend_fd(backend->rust_backend);
		struct wl_event_loop *loop = wl_display_get_event_loop(backend->display);
		backend->source = wl_event_loop_add_fd(loop, fd,
				WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
				qubes_backend_on_fd,
				backend);
		if (!backend->source) {
			wlr_log(WLR_ERROR, "Fatal error: Cannot re-register vchan file descriptor");
			wl_display_terminate(backend->display);
		}
		return;
	}
	if (!view) {
		if (hdr.type != MSG_KEYMAP_NOTIFY) {
			wlr_log(WLR_ERROR, "No window for message of type %" PRIu32, hdr.type);
			return;
		}
		struct wlr_keyboard *keyboard = backend->keyboard_input->keyboard;
		assert(keyboard);
		for (int i = 0; i < 32; ++i) {
			for (int j = 0; j < 8; ++j) {
				bool is_pressed = ptr[i] & 1 << j;
				bool old_pressed = backend->keymap.keys[i] & 1 << j;
				if (!old_pressed || is_pressed)
					continue;
				backend->keymap.keys[i] ^= 1 << j;
				struct wlr_event_keyboard_key event = {
					.time_msec = timestamp,
					.keycode = i << 3 | j,
					.update_state = true,
					.state = is_pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
				};
				wlr_keyboard_notify_key(keyboard, &event);
			}
		}
		assert(hdr.untrusted_len == sizeof(struct msg_keymap_notify));
		_Static_assert(sizeof(backend->keymap) == sizeof(struct msg_keymap_notify), "wrong size");
		memcpy(&backend->keymap, ptr, hdr.untrusted_len);
		return;
	}
	assert(hdr.window == view->window_id);
	switch (hdr.type) {
	case MSG_KEYPRESS:
		assert(hdr.untrusted_len == sizeof(struct msg_keypress));
		handle_keypress(view, timestamp, ptr);
		break;
	case MSG_CONFIGURE:
		assert(hdr.untrusted_len == sizeof(struct msg_configure));
		handle_configure(view, timestamp, ptr);
		break;
	case MSG_MAP:
		break;
	case MSG_BUTTON:
		assert(hdr.untrusted_len == sizeof(struct msg_button));
		handle_button(view->server->seat, timestamp, ptr);
		break;
	case MSG_MOTION:
		assert(hdr.untrusted_len == sizeof(struct msg_motion));
		handle_motion(view, timestamp, ptr);
		break;
	case MSG_CLOSE:
		assert(hdr.untrusted_len == 0);
		if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
			wlr_xdg_toplevel_send_close(view->xdg_surface);
		else if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP)
			wlr_xdg_popup_destroy(view->xdg_surface);
		break;
	case MSG_CROSSING:
		assert(hdr.untrusted_len == sizeof(struct msg_crossing));
		handle_crossing(view, timestamp, ptr);
		break;
	case MSG_FOCUS:
		assert(hdr.untrusted_len == sizeof(struct msg_focus));
		handle_focus(view, timestamp, ptr);
		break;
	case MSG_CLIPBOARD_REQ:
		assert(hdr.untrusted_len == 0);
		handle_clipboard_request(view);
		break;
	case MSG_CLIPBOARD_DATA:
		handle_clipboard_data(view, hdr.untrusted_len, ptr);
		break;
	case MSG_KEYMAP_NOTIFY:
		assert(hdr.untrusted_len == sizeof(struct msg_keymap_notify));
		break;
	case MSG_WINDOW_FLAGS:
		assert(hdr.untrusted_len == sizeof(struct msg_window_flags));
		// handle_window_flags(view, timestamp, ptr);
		break;
	case MSG_RESIZE:
	case MSG_CREATE:
	case MSG_DESTROY:
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
