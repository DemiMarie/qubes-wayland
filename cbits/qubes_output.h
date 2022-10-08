#ifndef QUBES_WAYLAND_COMPOSITOR_OUTPUT_H
#define QUBES_WAYLAND_COMPOSITOR_OUTPUT_H _Pragma("GCC error \"double-include guard referenced\"")

#include "common.h"
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_output.h>

#include <qubes-gui-protocol.h>

struct qubes_output {
	struct wlr_output output;
	struct wl_listener buffer_destroy;
	struct wlr_buffer *buffer; /* owned by the compositor */
	struct wl_listener frame;
	struct msg_keymap_notify keymap;
	const struct wlr_drm_format_set *formats; /* global */

	int x, y, left, top;
	int last_width, last_height;
	uint32_t window_id;
	uint32_t magic;
	uint32_t flags;
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
	struct wl_listener commit;

	/* only initialized for toplevels */
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener ack_configure;

	struct qubes_output output;
	uint32_t configure_serial;
	struct msg_wmname last_title;
};

enum {
	QUBES_OUTPUT_CREATED = 1 << 0,
	QUBES_OUTPUT_MAPPED = 1 << 1,
	QUBES_OUTPUT_IGNORE_CLIENT_RESIZE = 1 << 2,
};

static inline bool qubes_output_created(struct qubes_output *output)
{
	return output->flags & QUBES_OUTPUT_CREATED;
}

static inline bool qubes_output_mapped(struct qubes_output *output)
{
	if (!(output->flags & QUBES_OUTPUT_CREATED))
		return false;
	return output->flags & QUBES_OUTPUT_MAPPED;
}

void qubes_output_init(struct qubes_output *output, struct wlr_backend *backend,
                       struct wl_display *display);

void qubes_parse_event(void *raw_backend, void *raw_view, uint32_t timestamp, struct msg_hdr hdr, const uint8_t *ptr);
void qubes_send_configure(struct tinywl_view *view, uint32_t width, uint32_t height);
void qubes_output_dump_buffer(struct tinywl_view *view, struct wlr_box box);

#endif /* !defined QUBES_WAYLAND_COMPOSITOR_OUTPUT_H */
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
