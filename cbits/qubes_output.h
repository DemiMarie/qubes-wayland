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

	struct {
		int32_t x, y;
		uint32_t width, height;
	} host, guest;
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
	QUBES_OUTPUT_NEED_CONFIGURE = 1 << 4,
	QUBES_OUTPUT_DAMAGE_ALL     = 1 << 5,
	QUBES_OUTPUT_LEFT_CHANGED   = 1 << 6,
	QUBES_OUTPUT_TOP_CHANGED    = 1 << 7,
	QUBES_OUTPUT_RIGHT_CHANGED  = 1 << 8,
	QUBES_OUTPUT_BOTTOM_CHANGED = 1 << 9,
	QUBES_OUTPUT_WIDTH_CHANGED  = 1 << 10,
	QUBES_OUTPUT_HEIGHT_CHANGED = 1 << 11,
	QUBES_OUTPUT_NEED_CONFIGURE_ACK = 1 << 12,
};
#define QUBES_CHANGED_MASK (QUBES_OUTPUT_LEFT_CHANGED|QUBES_OUTPUT_RIGHT_CHANGED|QUBES_OUTPUT_TOP_CHANGED|QUBES_OUTPUT_BOTTOM_CHANGED|QUBES_OUTPUT_WIDTH_CHANGED|QUBES_OUTPUT_HEIGHT_CHANGED)
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

static inline bool
qubes_output_override_redirect(struct qubes_output *output)
{
	return output->flags & QUBES_OUTPUT_OVERRIDE_REDIRECT;
}

static inline bool
qubes_output_ignore_client_resize(struct qubes_output *output)
{
	return output->flags & QUBES_OUTPUT_IGNORE_CLIENT_RESIZE;
}

static inline bool
qubes_output_need_configure(struct qubes_output *output)
{
	return output->flags & QUBES_OUTPUT_NEED_CONFIGURE;
}

static inline bool
qubes_output_resized(struct qubes_output *output)
{
	return output->flags & (QUBES_OUTPUT_WIDTH_CHANGED | QUBES_OUTPUT_HEIGHT_CHANGED);
}

/* Initialize a qubes_output */
bool qubes_output_init(struct qubes_output *output,
                       struct tinywl_server *server, bool override_redirect,
                       struct wlr_surface *surface, uint32_t magic, int32_t x,
                       int32_t y, uint32_t width, uint32_t height)
   __attribute__((warn_unused_result));
void qubes_output_deinit(struct qubes_output *output);

void qubes_parse_event(void *raw_backend, void *raw_view, uint32_t timestamp,
                       struct msg_hdr hdr, const uint8_t *ptr);
void qubes_send_configure(struct qubes_output *output);
void qubes_output_dump_buffer(struct qubes_output *output,
                              const struct wlr_output_state *state);
bool qubes_output_ensure_created(struct qubes_output *output);
bool qubes_output_configure(struct qubes_output *output, struct wlr_box box);
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
bool qubes_output_move(struct qubes_output *output, int32_t x, int32_t y);
bool qubes_output_commit_size(struct qubes_output *output, struct wlr_box box);

#define qubes_window_log(output, loglevel, fmt, ...) \
	do wlr_log((loglevel), "Window %" PRIu32 ": " fmt, (output)->window_id,## __VA_ARGS__); while (0)

#endif /* !defined QUBES_WAYLAND_COMPOSITOR_OUTPUT_H */
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
