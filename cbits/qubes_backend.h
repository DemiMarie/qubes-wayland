#ifndef QUBES_WAYLAND_COMPOSITOR_BACKEND_H
#define QUBES_WAYLAND_COMPOSITOR_BACKEND_H _Pragma("GCC error \"double-include guard referenced\"")

#include "common.h"
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include <wayland-server-core.h>

#include <qubes-gui-protocol.h>

struct qubes_rust_backend;

/**
 * Qubes OS backend.  Owned by the wl_display.
 */
struct qubes_backend {
	struct wlr_backend backend;
	struct wl_display *display;
	struct wlr_output_mode mode;
	struct wlr_output *output;
	struct qubes_rust_backend *rust_backend;
	struct wl_event_source *source;
	struct msg_keymap_notify keymap;
	struct wl_list *views;
	struct wl_listener display_destroy;
	struct wlr_keyboard *keyboard;
	struct wlr_pointer *pointer;
};
extern int qubes_rust_backend_fd(struct qubes_rust_backend *backend);

struct qubes_backend * qubes_backend_create(struct wl_display *, uint16_t, struct wl_list *);
typedef void (*qubes_parse_event_callback)(void *raw_view, void *raw_backend, uint32_t timestamp, struct msg_hdr hdr, const uint8_t *ptr);
extern void qubes_rust_backend_on_fd_ready(struct qubes_rust_backend *, bool, qubes_parse_event_callback, void *);
int qubes_backend_on_fd(int, uint32_t, void *);

#endif
