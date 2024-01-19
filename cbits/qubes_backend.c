// wlr_backend implementation

#include "qubes_backend.h"
#include "common.h"

#include <stdlib.h>

#include <wlr/backend.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "qubes_output.h"
#include <qubes-gui-protocol.h>
#include <vchan-xen/libvchan.h>

static const struct wlr_backend_impl qubes_backend_impl;

static uint32_t qubes_backend_get_buffer_caps(struct wlr_backend *backend
                                              __attribute__((unused)))
{
	return WLR_BUFFER_CAP_DATA_PTR;
}

static void qubes_backend_destroy(struct qubes_backend *backend);

static void qubes_backend_handle_wlr_destroy(struct wlr_backend *raw_backend)
{
	assert(raw_backend->impl == &qubes_backend_impl);
	struct qubes_backend *backend =
	   wl_container_of(raw_backend, backend, backend);
	wl_list_remove(&backend->display_destroy.link);
	qubes_backend_destroy(backend);
}

static void qubes_backend_handle_display_destroy(struct wl_listener *listener,
                                                 void *data
                                                 __attribute__((unused)))
{
	struct qubes_backend *backend =
	   wl_container_of(listener, backend, display_destroy);
	qubes_backend_destroy(backend);
}

static bool qubes_backend_start(struct wlr_backend *raw_backend);
extern void qubes_rust_backend_free(void *ptr);
extern void *qubes_rust_backend_create(uint16_t domid);
typedef void (*qubes_parse_event_callback)(void *raw_view, void *raw_backend,
                                           uint32_t timestamp,
                                           struct msg_hdr hdr,
                                           const uint8_t *ptr);

static const struct wlr_backend_impl qubes_backend_impl = {
	.start = qubes_backend_start,
	.destroy = qubes_backend_handle_wlr_destroy,
	.get_drm_fd = NULL,
	.get_buffer_caps = qubes_backend_get_buffer_caps,
};

static bool qubes_backend_start(struct wlr_backend *raw_backend)
{
	assert(raw_backend->impl == &qubes_backend_impl);
	struct qubes_backend *backend =
	   wl_container_of(raw_backend, backend, backend);
	int fd = qubes_rust_backend_fd(backend->rust_backend);
	struct wl_event_loop *loop = wl_display_get_event_loop(backend->display);
	struct wl_event_source *source = wl_event_loop_add_fd(
	   loop, fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
	   qubes_backend_on_fd, backend);
	if (!source) {
		wlr_log(WLR_ERROR, "Cannot insert event source");
		return false;
	}
	backend->source = source;
	assert(backend);
	assert(backend->keyboard);
	assert(backend->pointer);
	wl_signal_emit(&raw_backend->events.new_output, backend->output);
	wl_signal_emit(&raw_backend->events.new_input, &backend->keyboard->base);
	wl_signal_emit(&raw_backend->events.new_input, &backend->pointer->base);
	wlr_log(WLR_DEBUG, "Qubes backend started successfully");
	return true;
}

int qubes_backend_on_fd(int fd __attribute__((unused)), uint32_t mask,
                        void *data)
{
	struct qubes_backend *backend = data;
	assert(!(mask & WL_EVENT_WRITABLE));
	qubes_rust_backend_on_fd_ready(backend->rust_backend,
	                               mask & WL_EVENT_READABLE, qubes_parse_event,
	                               backend);
	return 0;
}

static void qubes_backend_destroy(struct qubes_backend *backend)
{
	wlr_keyboard_finish(backend->keyboard);
	free(backend->keyboard);
	backend->keyboard = NULL;
	wlr_pointer_finish(backend->pointer);
	free(backend->pointer);
	backend->pointer = NULL;
	// MUST come before freeing the Rust backend, as that closes the file
	// descriptor.
	if (backend->source)
		wl_event_source_remove(backend->source);
	qubes_rust_backend_free(backend->rust_backend);
	wlr_output_destroy(backend->output);
	if (backend->display_destroy.link.next)
		wl_list_remove(&backend->display_destroy.link);
	free(backend);
}

static const struct wlr_keyboard_impl qubes_keyboard_impl = {
	.led_update = NULL,
};

static const struct wlr_pointer_impl qubes_pointer_impl = {};

static bool qubes_backend_output_commit(struct wlr_output *unused
                                        __attribute__((unused)),
                                        const struct wlr_output_state *state
                                        __attribute__((unused)))
{
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

struct qubes_backend *qubes_backend_create(struct wl_display *display,
                                           uint16_t domid,
                                           struct wl_list *views)
{
	struct qubes_backend *backend = calloc(sizeof(*backend), 1);
	struct wlr_keyboard *keyboard = calloc(sizeof(*keyboard), 1);
	struct wlr_output *output = calloc(sizeof(*output), 1);
	struct wlr_pointer *pointer = calloc(sizeof(*pointer), 1);

	if (backend == NULL || keyboard == NULL || output == NULL || pointer == NULL)
		goto fail;
	if (!(backend->rust_backend = qubes_rust_backend_create(domid))) {
		wlr_log(WLR_ERROR, "Cannot create Rust backend for domain %" PRIu16,
		        domid);
		goto fail;
	}
	backend->mode.width = 1920;
	backend->mode.height = 1080;
	backend->mode.refresh = 60000;
	backend->mode.preferred = true;
	wl_list_init(&backend->mode.link);
	backend->views = views;
	backend->output = output;
	backend->display = display;
	wlr_backend_init(&backend->backend, &qubes_backend_impl);
	output->make = "Qubes OS Virtual Output";
	output->model = "GUI Agent";
	output->serial = "1.0";
	output->phys_width = 344, output->phys_height = 194;
	wlr_output_init(output, &backend->backend, &qubes_backend_output_impl,
	                display, NULL);
	wlr_output_set_description(output, "Qubes OS Virtual Output Device");
	wlr_output_set_name(output, "Qubes OS Virtual Output Device");
	assert(wl_list_empty(&output->modes));
	wlr_output_set_mode(output, &backend->mode);
	wlr_output_enable(output, true);
	wl_list_insert(&output->modes, &backend->mode.link);
	assert(wlr_output_commit(output));
	output->current_mode = &backend->mode;
	assert(!wl_list_empty(&output->modes));
	assert(output->current_mode);
	wlr_keyboard_init(keyboard, &qubes_keyboard_impl,
	                  "Qubes OS Virtual Keyboard");
	wlr_keyboard_set_repeat_info(keyboard, 0, 0);
	backend->keyboard = keyboard;
	wlr_pointer_init(pointer, &qubes_pointer_impl, "Qubes OS Virtual Pointer");
	backend->pointer = pointer;
	backend->display_destroy.notify = qubes_backend_handle_display_destroy;
	wl_display_add_destroy_listener(display, &backend->display_destroy);
	return backend;
fail:
	free(backend);
	free(keyboard);
	free(output);
	free(pointer);
	return NULL;
}

// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
