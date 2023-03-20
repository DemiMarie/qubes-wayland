#pragma once

#include <wlr/util/box.h>

struct tinywl_view;
void qubes_view_map(struct tinywl_view *view);
void qubes_new_xdg_surface(struct wl_listener *listener, void *data);
bool qubes_xdg_surface_ensure_created(struct tinywl_view *view, struct wlr_box *box);
