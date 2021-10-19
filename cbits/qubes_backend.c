#include "common.h"

#include <stdlib.h>

#include <wlr/backend.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_buffer.h>

struct qubes_backend {
	struct wlr_backend backend;
	struct wlr_keyboard *keyboard;
	struct wlr_output *output;
};

static struct wlr_renderer *
qubes_backend_get_renderer(struct wlr_backend *backend __attribute__((unused))) {
	return wlr_pixman_renderer_create();
}

static uint32_t
qubes_backend_get_buffer_caps(struct wlr_backend *backend __attribute__((unused))) {
	return WLR_BUFFER_CAP_DATA_PTR;
}

static const struct wlr_backend_impl qubes_backend_impl = {
	.start = NULL,
	.destroy = NULL,
	.get_renderer = qubes_backend_get_renderer,
	.get_session = NULL,
	.get_presentation_clock = NULL,
	.get_drm_fd = NULL,
	.get_buffer_caps = qubes_backend_get_buffer_caps,
};

static const struct wlr_keyboard_impl qubes_keyboard_impl = {
	.destroy = NULL,
	.led_update = NULL,
};

static bool qubes_backend_output_commit(struct wlr_output *unused __attribute__((unused))) {
	assert(0 && "BUG: qubes_backend_output_commit should never be called!");
}

static const struct wlr_output_impl qubes_backend_output_impl = {
	.set_cursor = NULL,
	.move_cursor = NULL,
	.destroy = NULL,
	.test = NULL,
	.commit = qubes_backend_output_commit,
	.get_gamma_size = NULL,
	.get_cursor_formats = NULL,
	.get_cursor_size = NULL,
};

struct wlr_backend *
qubes_backend_create(struct wl_display *display) {
	struct qubes_backend *backend = calloc(sizeof(*backend), 1);
	struct wlr_keyboard *keyboard = calloc(sizeof(*keyboard), 1);
	struct wlr_output *output = calloc(sizeof(*output), 1);

	if (backend == NULL || keyboard == NULL || output == NULL) {
		free(backend);
		free(keyboard);
		free(output);
		return NULL;
	}
	backend->output = output;
	backend->keyboard = keyboard;
	wlr_backend_init(&backend->backend, &qubes_backend_impl);
	wlr_output_init(backend->output, &backend->backend, &qubes_backend_output_impl, display);
	wlr_keyboard_init(backend->keyboard, &qubes_keyboard_impl);
	return (struct wlr_backend *)backend;
}

// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
