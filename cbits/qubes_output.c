#include "common.h"
#include <string.h>
#include <wlr/types/wlr_output.h>
#include "qubes_output.h"

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

static void qubes_output_deinit(struct wlr_output *raw_output) {
	struct qubes_output *output = wl_container_of(raw_output, output, output);
	qubes_unlink_buffer(output);
}

static bool qubes_output_commit(struct wlr_output *raw_output) {
	struct qubes_output *output = wl_container_of(raw_output, output, output);
	if ((raw_output->pending.committed & WLR_OUTPUT_STATE_BUFFER) &&
	    (output->buffer != raw_output->pending.buffer)) {
		if (output->buffer)
			wl_list_remove(&output->buffer_destroy.link);
		output->buffer = raw_output->pending.buffer;
		if (output->buffer) {
			// assert(output->buffer->impl == qubes_buffer_impl);
			wl_signal_add(&output->buffer->events.destroy, &output->buffer_destroy);
		}
	}
	return true;
}

static const struct wlr_output_impl qubes_wlr_output_impl = {
	.set_cursor = NULL,
	.move_cursor = NULL,
	.destroy = qubes_output_deinit,
	.test = NULL,
	.commit = qubes_output_commit,
	.get_gamma_size = qubes_get_gamma_size,
	.get_cursor_formats = NULL,
	.get_cursor_size = NULL,
};

void qubes_output_init(struct qubes_output *output, struct wlr_backend *backend, struct wl_display *display) {
	memset(output, 0, sizeof *output);
	wlr_output_init(&output->output, backend, &qubes_wlr_output_impl, display);
	output->buffer = NULL;
	output->buffer_destroy.notify = qubes_unlink_buffer_listener;
}

// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
