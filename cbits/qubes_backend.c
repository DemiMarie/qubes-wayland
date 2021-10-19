#include "common.h"
#include "qubes_backend.h"

#include <stdlib.h>

#include <wlr/backend.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/log.h>

#include <vchan-xen/libvchan.h>

struct qubes_rust_backend;
struct qubes_backend {
	struct wlr_backend backend;
	struct wl_display *display;
	struct wlr_keyboard *keyboard;
	struct wlr_output *output;
	struct wlr_pointer *pointer;
	struct qubes_rust_backend *rust_backend;
	struct wl_event_source *source;
	libvchan_t *vchan;
};

static struct wlr_renderer *
qubes_backend_get_renderer(struct wlr_backend *backend __attribute__((unused))) {
	return wlr_pixman_renderer_create();
}

static uint32_t
qubes_backend_get_buffer_caps(struct wlr_backend *backend __attribute__((unused))) {
	return WLR_BUFFER_CAP_DATA_PTR;
}

static void qubes_backend_destroy(struct wlr_backend *raw_backend);
static bool qubes_backend_start(struct wlr_backend *raw_backend);
extern void qubes_rust_backend_free(void *ptr);
extern void *qubes_rust_backend_create(uint16_t domid);
extern int qubes_rust_backend_fd(struct qubes_rust_backend *backend);
extern void qubes_rust_backend_on_fd_ready(struct qubes_rust_backend *backend, bool);

static const struct wlr_backend_impl qubes_backend_impl = {
	.start = qubes_backend_start,
	.destroy = qubes_backend_destroy,
	.get_renderer = qubes_backend_get_renderer,
	.get_session = NULL,
	.get_presentation_clock = NULL,
	.get_drm_fd = NULL,
	.get_buffer_caps = qubes_backend_get_buffer_caps,
};

static int qubes_backend_on_fd(int, uint32_t, void *);

static bool qubes_backend_start(struct wlr_backend *raw_backend) {
	assert(raw_backend->impl == &qubes_backend_impl);
	struct qubes_backend *backend = wl_container_of(raw_backend, backend, backend);
	int fd = qubes_rust_backend_fd(backend->rust_backend);
	struct wl_event_loop *loop = wl_display_get_event_loop(backend->display);
	struct wl_event_source *source = wl_event_loop_add_fd(loop, fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
			qubes_backend_on_fd,
			backend);
	if (!source) {
		wlr_log(WLR_ERROR, "Cannot insert event source");
		return false;
	}
	backend->source = source;
	wlr_log(WLR_DEBUG, "Qubes backend started successfully");
	return true;
}

static int qubes_backend_on_fd(int fd __attribute__((unused)), uint32_t mask, void *data) {
	struct qubes_backend *backend = data;
	qubes_rust_backend_on_fd_ready(backend->rust_backend, mask & WL_EVENT_READABLE);
	return 0;
}

static void
qubes_backend_destroy(struct wlr_backend *raw_backend) {
	assert(raw_backend->impl == &qubes_backend_impl);
	struct qubes_backend *backend = wl_container_of(raw_backend, backend, backend);
	// MUST come before freeing the Rust backend, as that closes the file descriptor.
	if (backend->source)
		wl_event_source_remove(backend->source);
	wlr_keyboard_destroy(backend->keyboard);
	wlr_pointer_destroy(backend->pointer);
	wlr_output_destroy(backend->output);
	qubes_rust_backend_free(backend->rust_backend);
	free(backend);
}

static const struct wlr_keyboard_impl qubes_keyboard_impl = {
	.destroy = NULL,
	.led_update = NULL,
};

static const struct wlr_pointer_impl qubes_pointer_impl = {
	.destroy = NULL,
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
qubes_backend_create(struct wl_display *display, uint16_t domid) {
	struct qubes_backend *backend = calloc(sizeof(*backend), 1);
	struct wlr_keyboard *keyboard = calloc(sizeof(*keyboard), 1);
	struct wlr_output *output = calloc(sizeof(*output), 1);
	struct wlr_pointer *pointer = calloc(sizeof(*pointer), 1);

	if (backend == NULL || keyboard == NULL || output == NULL || pointer == NULL)
		goto fail;
	if (!(backend->rust_backend = qubes_rust_backend_create(domid))) {
		wlr_log(WLR_ERROR, "Cannot create Rust backend for domain %" PRIu16, domid);
		goto fail;
	}
	backend->output = output;
	backend->keyboard = keyboard;
	backend->pointer = pointer;
	backend->display = display;
	wlr_backend_init(&backend->backend, &qubes_backend_impl);
	wlr_output_init(backend->output, &backend->backend, &qubes_backend_output_impl, display);
	wlr_keyboard_init(backend->keyboard, &qubes_keyboard_impl);
	wlr_pointer_init(backend->pointer, &qubes_pointer_impl);
	return (struct wlr_backend *)backend;
fail:
	if (backend && backend->rust_backend)
		qubes_rust_backend_free(backend->rust_backend);
	free(backend);
	free(keyboard);
	free(output);
	free(pointer);
	return NULL;
}

// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
