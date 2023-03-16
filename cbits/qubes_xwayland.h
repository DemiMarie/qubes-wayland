#include "common.h"

#include <wayland-server-core.h>
#include <wlr/xwayland.h>

#include "qubes_output.h"

struct qubes_xwayland_view {
	struct qubes_output output;
	struct wl_list link;
	struct wlr_xwayland_surface *xwayland_surface;

	struct wl_listener destroy;
	struct wl_listener request_configure;
	struct wl_listener request_minimize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_activate;
	struct wl_listener set_geometry;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener set_title;
	struct wl_listener set_class;
	struct wl_listener set_role;
	struct wl_listener set_hints;
	struct wl_listener set_override_redirect;
	struct wl_listener set_parent;
	struct wl_listener commit;
};
void qubes_xwayland_new_xwayland_surface(struct wl_listener *listener,
                                         void *data);
void qubes_xwayland_surface_map(struct qubes_xwayland_view *view);
