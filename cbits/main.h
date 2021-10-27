#ifndef QUBES_WAYLAND_COMPOSITOR_MAIN_H
#define QUBES_WAYLAND_COMPOSITOR_MAIN_H _Pragma("GCC error \"double-include guard referenced\"")
#include "common.h"
#include <wlr/util/box.h>

#ifdef BUILD_RUST
#include <qubes-gui-protocol.h>
bool qubes_rust_send_message(void *backend, struct msg_hdr *header) __attribute__((warn_unused_result));
uint32_t qubes_rust_generate_id(void *backend, void *data) __attribute__((warn_unused_result));
void qubes_rust_delete_id(void *backend, uint32_t id);
#else
#define qubes_rust_send_message(a, b) (true)
#define qubes_rust_generate_id(a, b) (UINT32_C(0))
#define qubes_rust_destroy_id(a) ((void)0)
#endif

/* For brevity's sake, struct members are annotated where they are used. */
enum tinywl_cursor_mode {
	TINYWL_CURSOR_PASSTHROUGH,
	TINYWL_CURSOR_MOVE,
	TINYWL_CURSOR_RESIZE,
};

struct tinywl_server {
	struct wl_display *wl_display;
	struct qubes_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface;
	struct wl_list views;

	struct wlr_cursor *cursor;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum tinywl_cursor_mode cursor_mode;
	struct tinywl_view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
	uint32_t magic;
	uint16_t domid;
};

#endif
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
