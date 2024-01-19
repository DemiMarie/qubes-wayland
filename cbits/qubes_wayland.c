#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <err.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"

#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "main.h"
#include "qubes_backend.h"
#include "qubes_output.h"
#include "qubes_wayland.h"
#include "qubes_xwayland.h"

static void qubes_request_maximize(struct wl_listener *listener,
                                   void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to maximize window */
	struct tinywl_view *view = wl_container_of(listener, view, request_maximize);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
#ifdef WINDOW_FLAG_MAXIMIZE
	if (qubes_output_mapped(output)) {
		wlr_log(WLR_DEBUG, "Maximizing window " PRIu32, output->window_id);
		// Mapped implies created
		qubes_change_window_flags(&view->output, WINDOW_FLAG_MAXIMIZE, 0);
	}
#else
	wlr_log(WLR_ERROR, "window maximize: not implemented");
#endif
	wlr_xdg_surface_schedule_configure(view->xdg_surface);
}

static void qubes_request_minimize(struct wl_listener *listener,
                                   void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to minimize window */
	struct tinywl_view *view = wl_container_of(listener, view, request_minimize);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	if (qubes_output_mapped(output)) {
		wlr_log(WLR_DEBUG, "Marking window %" PRIu32 " minimized",
		        output->window_id);
		// Mapped implies created
		qubes_change_window_flags(&view->output, WINDOW_FLAG_MINIMIZE, 0);
	}
	wlr_xdg_surface_schedule_configure(view->xdg_surface);
}

/*
 * This requests a user-driven interactive move of the surface.  There is no way
 * to implement this in Qubes OS so this request is largely ignored.  It is
 * still necessary to send a configure event.
 */
static void qubes_request_move(struct wl_listener *listener, void *data)
{
	struct tinywl_view *view = wl_container_of(listener, view, request_move);
	struct qubes_output *output = &view->output;
	struct wlr_xdg_toplevel_move_event *event __attribute__((unused)) = data;

	assert(QUBES_VIEW_MAGIC == output->magic);
	wlr_xdg_surface_schedule_configure(view->xdg_surface);
}

/*
 * This is used to request an interactive resize of the surface.  There is no
 * way to implement this in Qubes OS so this request is largely ignored.  It is
 * still necessary to send a configure event.
 */
static void qubes_request_resize(struct wl_listener *listener, void *data)
{
	struct tinywl_view *view = wl_container_of(listener, view, request_resize);
	struct qubes_output *output = &view->output;
	struct wlr_xdg_toplevel_resize_event *event __attribute__((unused)) = data;

	assert(QUBES_VIEW_MAGIC == output->magic);
	wlr_xdg_surface_schedule_configure(view->xdg_surface);
}

/*
 * This is used to request that the compositor show the window menu, if any.
 * There is no way to implement this in Qubes OS so this request is largely
 * ignored.  It is still necessary to send a configure event.
 */
static void qubes_request_show_window_menu(struct wl_listener *listener,
                                           void *data)
{
	struct tinywl_view *view =
	   wl_container_of(listener, view, request_show_window_menu);
	struct qubes_output *output = &view->output;
	struct wlr_xdg_toplevel_show_window_menu_event *event
	   __attribute__((unused)) = data;

	assert(QUBES_VIEW_MAGIC == output->magic);
	wlr_xdg_surface_schedule_configure(view->xdg_surface);
}

static void qubes_request_fullscreen(struct wl_listener *listener,
                                     void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to fullscreen window */
	struct tinywl_view *view =
	   wl_container_of(listener, view, request_fullscreen);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	if (qubes_output_mapped(output)) {
		wlr_log(WLR_DEBUG, "Marking window %" PRIu32 " fullscreen",
		        output->window_id);
		// Mapped implies created
		qubes_change_window_flags(&view->output, WINDOW_FLAG_FULLSCREEN, 0);
	}
	wlr_xdg_surface_schedule_configure(view->xdg_surface);
}

static void qubes_set_title(struct wl_listener *listener,
                            void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WMNAME: ask GUI daemon to set window title */
	struct tinywl_view *view = wl_container_of(listener, view, set_title);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	assert(view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);
	if (qubes_output_mapped(output)) {
		// Mapped implies created
		qubes_set_view_title(&view->output, view->xdg_surface->toplevel->title);
	}
}

static void qubes_set_app_id(struct wl_listener *listener,
                             void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WMNAME: ask GUI daemon to set window app id */
	struct tinywl_view *view = wl_container_of(listener, view, set_app_id);
	struct qubes_output *output = &view->output;
	struct wlr_xdg_surface *surface = view->xdg_surface;

	assert(QUBES_VIEW_MAGIC == output->magic);
	assert(surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);
	if (qubes_output_mapped(output)) {
		// Mapped implies created
		qubes_output_set_class(output, surface->toplevel->app_id);
	}
}

static void xdg_surface_map(struct wl_listener *listener,
                            void *data __attribute__((unused)))
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	/* QUBES HOOK: MSG_MAP: map the corresponding window */
	struct tinywl_view *view = wl_container_of(listener, view, map);
	assert(QUBES_VIEW_MAGIC == view->output.magic);
	qubes_view_map(view);
}

static void xdg_surface_unmap(struct wl_listener *listener,
                              void *data __attribute__((unused)))
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	/* QUBES HOOK: MSG_UNMAP: unmap the corresponding window */
	struct tinywl_view *view = wl_container_of(listener, view, unmap);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	wlr_scene_node_set_enabled(&output->scene_output->scene->tree.node, false);
	wlr_scene_node_set_enabled(&output->scene_subsurface_tree->node, false);
	qubes_output_unmap(&view->output);
}

static void xdg_surface_destroy(struct wl_listener *listener,
                                void *data __attribute__((unused)))
{
	/* Called when the surface is destroyed and should never be shown again. */
	struct tinywl_view *view = wl_container_of(listener, view, destroy);

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->commit.link);
	if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wl_list_remove(&view->request_maximize.link);
		wl_list_remove(&view->request_fullscreen.link);
		wl_list_remove(&view->request_minimize.link);
		wl_list_remove(&view->request_move.link);
		wl_list_remove(&view->request_resize.link);
		wl_list_remove(&view->request_show_window_menu.link);
		wl_list_remove(&view->set_title.link);
		wl_list_remove(&view->set_app_id.link);
		wl_list_remove(&view->ack_configure.link);
	}
	qubes_output_deinit(&view->output);
	free(view);
}

static void qubes_surface_commit(struct wl_listener *listener,
                                 void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_HINTS, plus do a bunch of actual rendering stuff :)
	 */
	struct tinywl_view *view = wl_container_of(listener, view, commit);
	struct qubes_output *output = &view->output;
	struct wlr_box box;
	struct wlr_xdg_surface *surface = view->xdg_surface;

	assert(QUBES_VIEW_MAGIC == output->magic);
	assert(output->scene_output);
	assert(output->scene_output->output == &output->output);
	wlr_xdg_surface_get_geometry(view->xdg_surface, &box);
	qubes_window_log(output, WLR_DEBUG, "Surface commit: width %" PRIu32 " height %" PRIu32
	                 " x %" PRIi32 " y %" PRIi32, box.width, box.height, box.x, box.y);
	if (!qubes_output_commit_size(output, box)) {
		qubes_window_log(output, WLR_DEBUG, "Ignoring because size commit failed");
		return;
	}
	if (surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		uint32_t flags =
		   ((surface->toplevel->current.min_width ? XCB_ICCCM_SIZE_HINT_P_MIN_SIZE
		                                          : 0) |
		    (surface->toplevel->current.min_height
		        ? XCB_ICCCM_SIZE_HINT_P_MIN_SIZE
		        : 0) |
		    (surface->toplevel->current.max_width ? XCB_ICCCM_SIZE_HINT_P_MAX_SIZE
		                                          : 0) |
		    (surface->toplevel->current.max_height
		        ? XCB_ICCCM_SIZE_HINT_P_MAX_SIZE
		        : 0));
		assert(output->window_id != 0);
		// clang-format off
		struct {
			struct msg_hdr header;
			struct msg_window_hints hints;
		} msg = {
			.header = {
				.type = MSG_WINDOW_HINTS,
				.window = output->window_id,
				.untrusted_len = sizeof(msg.hints),
			},
			.hints = {
				.flags = flags,
				.min_width = surface->toplevel->current.min_width,
				.min_height = surface->toplevel->current.min_height,
				.max_width = surface->toplevel->current.max_width,
				.max_height = surface->toplevel->current.max_height,
				.width_inc = 0,
				.height_inc = 0,
				.base_width = 0,
				.base_height = 0,
			},
		};
		// clang-format on
		QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.hints);
		qubes_rust_send_message(output->server->backend->rust_backend,
		                        (struct msg_hdr *)&msg);
	}
	wlr_output_send_frame(&output->output);
}

static void qubes_toplevel_ack_configure(struct wl_listener *listener,
                                         void *data)
{
	struct wlr_xdg_surface_configure *configure = data;
	struct tinywl_view *view = wl_container_of(listener, view, ack_configure);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	qubes_window_log(output, WLR_DEBUG, "Client acknowledged serial %" PRIu32, view->configure_serial);

	if ((output->flags & QUBES_OUTPUT_NEED_CONFIGURE_ACK) &&
	    (view->configure_serial == configure->serial)) {
		struct wlr_output_state state;
		output->flags &= ~QUBES_OUTPUT_NEED_CONFIGURE_ACK;
		wlr_output_state_init(&state);
		wlr_output_state_set_custom_mode(&state, output->host.width,
		                                 output->host.height, 60000);
		wlr_output_commit_state(&output->output, &state);
		wlr_output_state_finish(&state);
	}
}

void qubes_new_xdg_surface(struct wl_listener *listener, void *data)
{
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct tinywl_server *server =
	   wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	assert(QUBES_SERVER_MAGIC == server->magic);
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL &&
	    xdg_surface->role != WLR_XDG_SURFACE_ROLE_POPUP) {
		return;
	}
	/* QUBES HOOK: MSG_CREATE: create toplevel window */

	bool is_override_redirect = xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP;

	/* Allocate a tinywl_view for this surface */
	struct tinywl_view *view = calloc(1, sizeof(struct tinywl_view));
	if (!view)
		goto cleanup;

	struct qubes_output *output = &view->output;
	struct wlr_box geometry;
	wlr_xdg_surface_get_geometry(xdg_surface, &geometry);
	if (!qubes_output_init(output, server, is_override_redirect,
	                       xdg_surface->surface, QUBES_VIEW_MAGIC, geometry.x,
	                       geometry.y, geometry.width, geometry.height))
		goto cleanup;

	view->xdg_surface = xdg_surface;

	/* Listen to the various events it can emit */
	view->map.notify = xdg_surface_map;
	wl_signal_add(&xdg_surface->surface->events.map, &view->map);
	view->unmap.notify = xdg_surface_unmap;
	wl_signal_add(&xdg_surface->surface->events.unmap, &view->unmap);
	view->destroy.notify = xdg_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
	xdg_surface->data = view;

	/* And listen to the various emits the toplevel can emit */
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		struct wlr_xdg_toplevel *const toplevel = xdg_surface->toplevel;
		view->request_maximize.notify = qubes_request_maximize;
		wl_signal_add(&toplevel->events.request_maximize,
		              &view->request_maximize);

		view->request_fullscreen.notify = qubes_request_fullscreen;
		wl_signal_add(&toplevel->events.request_fullscreen,
		              &view->request_fullscreen);

		view->request_minimize.notify = qubes_request_minimize;
		wl_signal_add(&toplevel->events.request_minimize,
		              &view->request_minimize);

		view->request_move.notify = qubes_request_move;
		wl_signal_add(&toplevel->events.request_move, &view->request_move);

		view->request_resize.notify = qubes_request_resize;
		wl_signal_add(&toplevel->events.request_resize, &view->request_resize);

		view->request_show_window_menu.notify = qubes_request_show_window_menu;
		wl_signal_add(&toplevel->events.request_show_window_menu,
		              &view->request_show_window_menu);

		view->set_title.notify = qubes_set_title;
		wl_signal_add(&toplevel->events.set_title, &view->set_title);

		view->set_app_id.notify = qubes_set_app_id;
		wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);

		view->ack_configure.notify = qubes_toplevel_ack_configure;
		wl_signal_add(&xdg_surface->events.ack_configure, &view->ack_configure);
	} else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_popup *const popup = xdg_surface->popup;
		struct wlr_box geometry;
		wlr_xdg_positioner_rules_get_geometry(&popup->scheduled.rules, &geometry);
		struct tinywl_view *parent_view =
		   wlr_xdg_surface_try_from_wlr_surface(popup->parent)->data;
		assert(parent_view);
		// Use the parent's guest values in case changes have not propagated to
		// the host.
		output->guest.x = output->host.x =
		   geometry.x + parent_view->output.guest.x;
		output->guest.y = output->host.y =
		   geometry.y + parent_view->output.guest.y;
		output->guest.width = output->host.width = geometry.width;
		output->guest.height = output->host.height = geometry.height;
		wl_list_init(&view->request_maximize.link);
		wl_list_init(&view->request_fullscreen.link);
		wl_list_init(&view->request_minimize.link);
		wl_list_init(&view->request_move.link);
		wl_list_init(&view->request_resize.link);
		wl_list_init(&view->request_show_window_menu.link);
		wl_list_init(&view->set_title.link);
		wl_list_init(&view->ack_configure.link);
	} else {
		abort();
	}
	wlr_scene_output_set_position(output->scene_output, geometry.x, geometry.y);

	/* Listen to surface events */
	view->commit.notify = qubes_surface_commit;
	wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);

	/* Get the window ID */
	assert(output->window_id == 0);

	/* Tell GUI daemon to create window */
	wlr_output_set_custom_mode(&output->output, output->guest.width,
	                           output->guest.height, 60000);
	return;
cleanup:
	wl_resource_post_no_memory(xdg_surface->resource);
	if (view) {
		qubes_output_deinit(output);
		free(view);
	}
	return;
}

void qubes_view_map(struct tinywl_view *view)
{
	struct wlr_box box;
	struct qubes_output *output = &view->output;

	struct wlr_xdg_surface *xdg_surface = view->xdg_surface;
	wlr_xdg_surface_get_geometry(view->xdg_surface, &box);
	wlr_scene_output_set_position(view->output.scene_output, box.x, box.y);
	box.x = output->guest.x;
	box.y = output->guest.y;
	if (!qubes_output_configure(output, box))
		return;
	uint32_t transient_for_window = 0;
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		uint32_t flags_to_set = 0, flags_to_unset = 0;
		if (xdg_surface->toplevel->requested.minimized) {
			flags_to_set = WINDOW_FLAG_MINIMIZE;
			flags_to_unset = WINDOW_FLAG_FULLSCREEN;
		} else if (xdg_surface->toplevel->requested.fullscreen) {
			flags_to_set = WINDOW_FLAG_FULLSCREEN;
			flags_to_unset = WINDOW_FLAG_MINIMIZE;
		}
		if (flags_to_set || flags_to_unset) {
			// Window created above, so this is safe
			qubes_change_window_flags(&view->output, flags_to_set, flags_to_unset);
		}
		if (xdg_surface->toplevel->title) {
			// Window created above, so this is safe
			qubes_set_view_title(&view->output, xdg_surface->toplevel->title);
		}
		if (xdg_surface->toplevel->app_id) {
			// Window created above, so this is safe
			qubes_output_set_class(output, xdg_surface->toplevel->app_id);
		}
		if (xdg_surface->toplevel->parent) {
			const struct qubes_output *parent_output =
			   xdg_surface->toplevel->parent->base->data;
			transient_for_window = parent_output->window_id;
		}
	} else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_popup *popup = xdg_surface->popup;
		if (popup->parent) {
			const struct wlr_xdg_surface *parent_surface =
			   wlr_xdg_surface_try_from_wlr_surface(popup->parent);
			assert(parent_surface != NULL);
			transient_for_window =
			   ((struct qubes_output *)parent_surface->data)->window_id;
		}
	} else {
		return;
	}
	qubes_output_map(output, transient_for_window,
	                 view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP ? 1
	                                                                       : 0);
}
