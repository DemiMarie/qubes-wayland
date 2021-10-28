#include "common.h"
#include "qubes_backend.h"

#include <stdlib.h>

#include <wlr/backend.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_input_device.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#ifdef BUILD_RUST
#include <vchan-xen/libvchan.h>
#include <qubes-gui-protocol.h>
#include "qubes_output.h"
#endif

static const struct wlr_backend_impl qubes_backend_impl;

#ifdef QUBES_BACKEND_CUSTOM_RENDERER
static struct wlr_renderer *
qubes_backend_get_renderer(struct wlr_backend *backend __attribute__((unused))) {
	return wlr_pixman_renderer_create();
}
#endif

static uint32_t
qubes_backend_get_buffer_caps(struct wlr_backend *backend __attribute__((unused))) {
	return WLR_BUFFER_CAP_DATA_PTR;
}

static void qubes_backend_destroy(struct qubes_backend *backend);

static void qubes_backend_handle_wlr_destroy(struct wlr_backend *raw_backend) {
	assert(raw_backend->impl == &qubes_backend_impl);
	struct qubes_backend *backend = wl_container_of(raw_backend, backend, backend);
	wl_list_remove(&backend->display_destroy.link);
	qubes_backend_destroy(backend);
}

static void qubes_backend_handle_display_destroy(struct wl_listener *listener, void *data __attribute__((unused))) {
	struct qubes_backend *backend = wl_container_of(listener, backend, display_destroy);
	qubes_backend_destroy(backend);
}

static bool qubes_backend_start(struct wlr_backend *raw_backend);
#ifdef BUILD_RUST
extern void qubes_rust_backend_free(void *ptr);
extern void *qubes_rust_backend_create(uint16_t domid);
extern int qubes_rust_backend_fd(struct qubes_rust_backend *backend);
typedef void (*qubes_parse_event_callback)(void *raw_view, uint32_t timestamp, struct msg_hdr hdr, const uint8_t *ptr);

extern void qubes_rust_backend_on_fd_ready(struct qubes_rust_backend *backend, bool, qubes_parse_event_callback);
static int qubes_backend_on_fd(int, uint32_t, void *);
#endif


static const struct wlr_backend_impl qubes_backend_impl = {
	.start = qubes_backend_start,
	.destroy = qubes_backend_handle_wlr_destroy,
#ifdef QUBES_BACKEND_CUSTOM_RENDERER
	.get_renderer = qubes_backend_get_renderer,
#else
	.get_renderer = NULL,
#endif
	.get_session = NULL,
	.get_presentation_clock = NULL,
	.get_drm_fd = NULL,
	.get_buffer_caps = qubes_backend_get_buffer_caps,
};

static bool qubes_backend_start(struct wlr_backend *raw_backend) {
	assert(raw_backend->impl == &qubes_backend_impl);
	struct qubes_backend *backend = wl_container_of(raw_backend, backend, backend);
#ifdef BUILD_RUST
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
#endif
	wl_signal_emit(&raw_backend->events.new_output, backend->output);
	wl_signal_emit(&raw_backend->events.new_input, backend->keyboard_input);
	wl_signal_emit(&raw_backend->events.new_input, backend->pointer_input);
	wlr_log(WLR_DEBUG, "Qubes backend started successfully");
	return true;
}

#ifdef BUILD_RUST
static int qubes_backend_on_fd(int fd __attribute__((unused)), uint32_t mask, void *data) {
	struct qubes_backend *backend = data;
	assert(!(mask & WL_EVENT_WRITABLE));
	qubes_rust_backend_on_fd_ready(backend->rust_backend, mask & WL_EVENT_READABLE, qubes_parse_event);
	return 0;
}
#endif

static void
qubes_backend_destroy(struct qubes_backend *backend) {
#ifdef BUILD_RUST
	// MUST come before freeing the Rust backend, as that closes the file descriptor.
	if (backend->source)
		wl_event_source_remove(backend->source);
	qubes_rust_backend_free(backend->rust_backend);
#endif
	wlr_output_destroy(backend->output);
	wlr_input_device_destroy(backend->keyboard_input);
	wlr_input_device_destroy(backend->pointer_input);
	wl_list_remove(&backend->display_destroy.link);
	free(backend);
}

static const struct wlr_keyboard_impl qubes_keyboard_impl = {
	.destroy = NULL,
	.led_update = NULL,
};

static const struct wlr_pointer_impl qubes_pointer_impl = {
	.destroy = NULL,
};

static const struct wlr_input_device_impl qubes_input_device_impl = {
	.destroy = NULL,
};

static bool qubes_backend_output_commit(struct wlr_output *unused __attribute__((unused))) {
	assert(1 && "BUG: qubes_backend_output_commit should never be called!");
	return true;
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

struct qubes_backend *
qubes_backend_create(struct wl_display *display, uint16_t domid) {
	struct qubes_backend *backend = calloc(sizeof(*backend), 1);
	struct wlr_keyboard *keyboard = calloc(sizeof(*keyboard), 1);
	struct wlr_output *output = calloc(sizeof(*output), 1);
	struct wlr_pointer *pointer = calloc(sizeof(*pointer), 1);
	struct wlr_input_device *keyboard_input = calloc(sizeof(*keyboard_input), 1);
	struct wlr_input_device *pointer_input = calloc(sizeof(*pointer_input), 1);

	if (backend == NULL || keyboard == NULL || output == NULL ||
	    pointer == NULL || keyboard_input == NULL || pointer_input == NULL)
		goto fail;
#ifdef BUILD_RUST
	if (!(backend->rust_backend = qubes_rust_backend_create(domid))) {
		wlr_log(WLR_ERROR, "Cannot create Rust backend for domain %" PRIu16, domid);
		goto fail;
	}
#else
	(void)domid;
#endif
	backend->mode.width = 1920;
	backend->mode.height = 1080;
	backend->mode.refresh = 60000;
	backend->mode.preferred = true;
	wl_list_init(&backend->mode.link);
	backend->output = output;
	backend->display = display;
	backend->keyboard_input = keyboard_input;
	backend->pointer_input = pointer_input;
	backend->backend.has_own_renderer = true;
	wlr_backend_init(&backend->backend, &qubes_backend_impl);
	strncpy(output->make, "Qubes OS Virtual Output", sizeof output->make - 1);
	strncpy(output->model, "GUI Agent", sizeof output->model - 1);
	strncpy(output->serial, "1.0", sizeof output->model - 1);
	output->phys_width = 344, output->phys_height = 194;
	wlr_output_init(output, &backend->backend, &qubes_backend_output_impl, display);
	wlr_output_set_description(output, "Qubes OS Virtual Output Device");
	assert(wl_list_empty(&output->modes));
	wlr_output_set_mode(output, &backend->mode);
	wlr_output_enable(output, true);
	wl_list_insert(&output->modes, &backend->mode.link);
	assert(wlr_output_commit(output));
	output->current_mode = &backend->mode;
	assert(!wl_list_empty(&output->modes));
	assert(output->current_mode);
	wlr_keyboard_init(keyboard, &qubes_keyboard_impl);
	wlr_pointer_init(pointer, &qubes_pointer_impl);
	wlr_input_device_init(keyboard_input, WLR_INPUT_DEVICE_KEYBOARD, &qubes_input_device_impl,
			"Qubes OS Virtual Keyboard", 0, 0);
	keyboard_input->keyboard = keyboard;
	pointer_input->pointer = pointer;
	wlr_input_device_init(pointer_input, WLR_INPUT_DEVICE_POINTER, &qubes_input_device_impl,
			"Qubes OS Virtual Pointer", 0, 0);
	backend->display_destroy.notify = qubes_backend_handle_display_destroy;
	wl_display_add_destroy_listener(display, &backend->display_destroy);
	return backend;
fail:
	free(backend);
	free(keyboard);
	free(output);
	free(pointer);
	free(keyboard_input);
	free(pointer_input);
	return NULL;
}

// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
