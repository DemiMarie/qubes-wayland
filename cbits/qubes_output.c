#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>
#include <wlr/render/drm_format_set.h>
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
	wlr_output_update_custom_mode(raw_output,
			raw_output->pending.custom_mode.width,
			raw_output->pending.custom_mode.height,
			raw_output->pending.custom_mode.refresh);
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
	abort();
}

void qubes_output_init(struct qubes_output *output, struct wlr_backend *backend, struct wl_display *display) {
	memset(output, 0, sizeof *output);
	wlr_output_init(&output->output, backend, &qubes_wlr_output_impl, display);
	output->buffer = NULL;
	output->buffer_destroy.notify = qubes_unlink_buffer_listener;
	output->formats = &global_formats;
	output->frame.notify = qubes_output_frame;
	wl_signal_add(&output->output.events.frame, &output->frame);
}

/* vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8: */
