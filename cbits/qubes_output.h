#ifndef QUBES_WAYLAND_COMPOSITOR_OUTPUT_H
#define QUBES_WAYLAND_COMPOSITOR_OUTPUT_H                                      \
	_Pragma("GCC error \"double-include guard referenced\"")

#include "common.h"
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/util/box.h>

#include <qubes-gui-protocol.h>

struct qubes_output {
	struct wl_list link;
	struct wlr_output output;
	struct wl_listener buffer_destroy;
	struct wlr_buffer *buffer;   /* owned by the compositor */
	struct wlr_surface *surface; /* ditto */
	struct wl_listener frame;
	struct msg_keymap_notify keymap;
	const struct wlr_drm_format_set *formats; /* global */
	struct tinywl_server *server;
	struct wlr_scene *scene;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_tree *scene_subsurface_tree;
	char *name;

	int x, y, left, top;
	int last_width, last_height;
	uint32_t window_id;
	uint32_t magic;
	uint32_t flags;
};

struct qubes_link {
	struct qubes_link *next;
	struct qubes_buffer *buffer;
};

struct tinywl_server;
struct wlr_xdg_surface;
enum {
	QUBES_OUTPUT_CREATED = 1 << 0,
	QUBES_OUTPUT_MAPPED = 1 << 1,
	QUBES_OUTPUT_IGNORE_CLIENT_RESIZE = 1 << 2,
	QUBES_OUTPUT_OVERRIDE_REDIRECT = 1 << 3,
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

static inline bool qubes_output_override_redirect(struct qubes_output *output)
{
	return output->flags & QUBES_OUTPUT_OVERRIDE_REDIRECT;
}

/* Initialize a qubes_output */
bool qubes_output_init(struct qubes_output *output,
                       struct tinywl_server *server, bool override_redirect,
                       struct wlr_surface *surface, uint32_t magic)
   __attribute__((warn_unused_result));
void qubes_output_deinit(struct qubes_output *output);

void qubes_parse_event(void *raw_backend, void *raw_view, uint32_t timestamp,
                       struct msg_hdr hdr, const uint8_t *ptr);
void qubes_send_configure(struct qubes_output *output, uint32_t width,
                          uint32_t height);
void qubes_output_dump_buffer(struct qubes_output *output, struct wlr_box box,
                              const struct wlr_output_state *state);
bool qubes_output_ensure_created(struct qubes_output *output,
                                 struct wlr_box box);
void qubes_output_configure(struct qubes_output *output, struct wlr_box box);
void qubes_output_unmap(struct qubes_output *output);
void qubes_change_window_flags(struct qubes_output *output, uint32_t flags_set,
                               uint32_t flags_unset);
bool qubes_output_set_surface(struct qubes_output *const output,
                              struct wlr_surface *const surface);
void qubes_output_map(struct qubes_output *output,
                      uint32_t transient_for_window, bool override_redirect);
struct wlr_surface *qubes_output_surface(struct qubes_output *output);
void qubes_set_view_title(struct qubes_output *output, const char *const title);
void qubes_output_set_class(struct qubes_output *output, const char *class);
void qubes_output_move(struct qubes_output *output, int32_t x, int32_t y);

#endif /* !defined QUBES_WAYLAND_COMPOSITOR_OUTPUT_H */
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
