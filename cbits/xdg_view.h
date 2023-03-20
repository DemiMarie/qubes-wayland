#pragma once

#include "common.h"
#include "qubes_output.h"
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct tinywl_view {
	struct qubes_output output;
	struct wlr_xdg_surface *xdg_surface;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;

	/* only initialized for toplevels */
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_show_window_menu;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener ack_configure;

	uint32_t configure_serial;
};
bool qubes_view_ensure_created(struct tinywl_view *view, struct wlr_box *box);
void qubes_view_map(struct tinywl_view *view);
void qubes_new_xdg_surface(struct wl_listener *listener, void *data);
