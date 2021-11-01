#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/drm_format_set.h>
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
			assert(output->buffer->impl == qubes_buffer_impl_addr);
			wl_signal_add(&output->buffer->events.destroy, &output->buffer_destroy);
#ifdef BUILD_RUST
			wlr_log(WLR_DEBUG, "Sending MSG_WINDOW_DUMP (0x%x) to window %" PRIu32, MSG_WINDOW_DUMP, view->window_id);
			struct qubes_buffer *buffer = wl_container_of(output->buffer, buffer, inner);
			buffer->header.window = view->window_id;
			buffer->header.type = MSG_WINDOW_DUMP;
			buffer->header.untrusted_len = sizeof(buffer->qubes) + NUM_PAGES(buffer->size) * SIZEOF_GRANT_REF;
			assert(qubes_rust_send_message(view->server->backend->rust_backend, &buffer->header));
#endif
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
#ifdef BUILD_RUST
	struct wlr_box box;
	if (!qubes_output_ensure_created(view, &box))
		return;
	wlr_log(WLR_DEBUG, "Width is %" PRIu32 " height is %" PRIu32, (uint32_t)box.width, (uint32_t)box.height);
	qubes_send_configure(view, box.width, box.height);
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
	// Created above
	assert(qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr *)&new_msg));
#endif
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
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	assert(keyboard);

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
		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
		assert(keyboard);
		for (int i = 0; i < 32; ++i) {
			for (int j = 0; j < 8; ++j) {
				struct wlr_event_keyboard_key event = {
					.time_msec = timestamp,
					.keycode = i << 3 | j,
					.update_state = true,
					.state = (view->output.keymap.keys[i] & 1 << j) ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
				};
				wlr_keyboard_notify_key(keyboard, &event);
			}
		}
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
	assert(qubes_rust_send_message(view->server->backend->rust_backend, (struct msg_hdr*)&msg));
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
	wlr_output_update_custom_mode(&view->output.output, configure.width, configure.height, 0);
	if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		wlr_xdg_toplevel_set_size(view->xdg_surface, configure.width, configure.height);
	else
		assert(0 && "not implemented");
}

void qubes_parse_event(void *raw_view, uint32_t timestamp, struct msg_hdr hdr, const uint8_t *ptr)
{
	struct tinywl_view *view = raw_view;
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
		break;
	case MSG_CLIPBOARD_DATA:
		break;
	case MSG_KEYMAP_NOTIFY:
		assert(hdr.untrusted_len == sizeof(struct msg_keymap_notify));
		memcpy(&view->output.keymap, ptr, sizeof(struct msg_keymap_notify));
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
