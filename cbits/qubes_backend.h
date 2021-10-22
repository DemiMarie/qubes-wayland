#ifndef QUBES_WAYLAND_COMPOSITOR_BACKEND_H
#define QUBES_WAYLAND_COMPOSITOR_BACKEND_H _Pragma("GCC error \"double-include guard referenced\"")

#include "common.h"
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include <wayland-server-core.h>

struct qubes_rust_backend;
struct qubes_backend {
	struct wlr_backend backend;
	struct wl_display *display;
	struct wlr_output_mode mode;
	struct wlr_output *output;
	struct wlr_input_device *keyboard_input, *pointer_input;
	struct qubes_rust_backend *rust_backend;
	struct wl_event_source *source;

	struct wl_listener display_destroy;
};

struct qubes_backend * qubes_backend_create(struct wl_display *, uint16_t);

#endif
