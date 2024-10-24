#include "wlroots-subproject/include/backend/headless.h"
#ifndef QUBES_WAYLAND_COMPOSITOR_MAIN_H
#define QUBES_WAYLAND_COMPOSITOR_MAIN_H                                        \
	_Pragma("GCC error \"double-include guard referenced\"")
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/box.h>

#include <qubes-gui-protocol.h>
#include <qubesdb-client.h>
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

struct tinywl_keyboard {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_keyboard *keyboard;
	struct xkb_context *context;

	struct wl_listener modifiers;
	struct wl_listener key;
	uint32_t magic;
};

/**
 * Qubes OS server.  Freed at end of main().
 */
struct tinywl_server {
	struct wl_display *wl_display;
	struct qubes_backend *backend;
	struct qubes_link *queue_head;
	struct qubes_link *queue_tail;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_xwayland_surface;
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
	struct wl_event_source *timer, *qubesdb_watcher;
	struct wlr_compositor *compositor;
	struct wlr_subcompositor *subcompositor;
	struct wlr_data_device_manager *data_device;
	struct wlr_xwayland *xwayland;
	struct wlr_output *headless_output;
	struct tinywl_keyboard keyboard;
	qdb_handle_t qubesdb_connection;
	uint32_t magic;
	uint16_t domid;
	bool frame_pending, vchan_error;
	uint64_t output_counter;
	int listening_socket;
	uint8_t exit_status;
	bool keymap_errors_fatal;
};

#endif
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
