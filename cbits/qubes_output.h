#ifndef QUBES_WAYLAND_COMPOSITOR_OUTPUT_H
#define QUBES_WAYLAND_COMPOSITOR_OUTPUT_H _Pragma("GCC error \"double-include guard referenced\"")

#include "common.h"
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_output.h>

struct qubes_output {
	struct wlr_output output;
	struct wl_listener buffer_destroy;
	struct wlr_buffer *buffer; /* *not* owned by the compositor */
	struct wl_listener frame;
	const struct wlr_drm_format_set *formats; /* global */
};

struct tinywl_server;
struct wlr_xdg_surface;
struct tinywl_view {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene *scene;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_node *scene_subsurface_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener new_popup;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener set_title;
	struct wl_listener commit;
	struct wl_listener frame;
	struct qubes_output output;
	int x, y;
	int last_width, last_height;
	uint32_t window_id;
	uint32_t magic;
	bool mapped, need_configure;
};

void qubes_output_init(struct qubes_output *output, struct wlr_backend *backend,
                       struct wl_display *display);

void qubes_parse_event(void *raw_view, uint32_t timestamp, struct msg_hdr hdr, const uint8_t *ptr);

#endif /* !defined QUBES_WAYLAND_COMPOSITOR_OUTPUT_H */
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
