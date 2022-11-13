#ifndef QUBES_WAYLAND_COMPOSITOR_MAIN_H
#define QUBES_WAYLAND_COMPOSITOR_MAIN_H _Pragma("GCC error \"double-include guard referenced\"")
#include "common.h"
#include <wlr/util/box.h>

#include <qubes-gui-protocol.h>
void qubes_rust_send_message(void *backend, struct msg_hdr *header);
void qubes_rust_delete_id(void *backend, uint32_t id);

struct wlr_surface;
struct tinywl_view;

/* For brevity's sake, struct members are annotated where they are used. */
enum tinywl_cursor_mode {
	TINYWL_CURSOR_PASSTHROUGH,
	TINYWL_CURSOR_MOVE,
	TINYWL_CURSOR_RESIZE,
};

/**
 * Qubes OS server.  Freed at end of main().
 */
struct tinywl_server {
	struct wl_display *wl_display;
	struct qubes_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface, new_xwayland_surface;
	struct wl_list views;

	struct wlr_seat *seat;
	struct wl_listener new_input;
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
	struct wlr_server_decoration_manager *old_manager;
	struct wlr_xdg_decoration_manager_v1 *new_manager;
	struct wl_listener new_decoration;
	struct wl_event_source *timer, *listener;
	struct wlr_compositor *compositor;
	struct wlr_data_device_manager *data_device;
	struct wlr_xwayland *xwayland;
	uint32_t magic;
	uint16_t domid;
	bool frame_pending, vchan_error;
	int listening_socket;
};

bool qubes_view_ensure_created(struct tinywl_view *view, struct wlr_box *box);
void qubes_view_map(struct tinywl_view *view);

#endif
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
