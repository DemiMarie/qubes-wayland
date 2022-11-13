#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <err.h>

#include "config.h"

#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#include <wayland-server-core.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "qubes_output.h"
#include "qubes_backend.h"
#include "qubes_xwayland.h"
#include "main.h"

static void qubes_request_maximize(
	struct wl_listener *listener, void *data __attribute__((unused)))
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
}

static void qubes_request_minimize(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to minimize window */
	struct tinywl_view *view = wl_container_of(listener, view, request_minimize);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	if (qubes_output_mapped(output)) {
		wlr_log(WLR_DEBUG, "Marking window %" PRIu32 " minimized", output->window_id);
		// Mapped implies created
		qubes_change_window_flags(&view->output, WINDOW_FLAG_MINIMIZE, 0);
	}
}

static void qubes_request_fullscreen(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_FLAGS: ask GUI daemon to fullscreen window */
	struct tinywl_view *view = wl_container_of(listener, view, request_fullscreen);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	if (qubes_output_mapped(output)) {
		wlr_log(WLR_DEBUG, "Marking window %" PRIu32 " fullscreen", output->window_id);
		// Mapped implies created
		qubes_change_window_flags(&view->output, WINDOW_FLAG_FULLSCREEN, 0);
	}
}

static void qubes_set_view_title(struct tinywl_view *view)
{
	struct qubes_output *output = &view->output;

	if (!strncmp(output->last_title.data,
	             view->xdg_surface->toplevel->title,
	             sizeof(output->last_title.data) - 1)) {
		return;
	}
	assert(qubes_output_created(output));
	assert(output->window_id);
	strncpy(output->last_title.data,
	        view->xdg_surface->toplevel->title,
	        sizeof(output->last_title.data) - 1);
	output->last_title.data[sizeof(output->last_title.data) - 1] = '\0';
	wlr_log(WLR_DEBUG, "Sending MSG_WMNAME (0x%x) to window %" PRIu32, MSG_WMNAME, output->window_id);
	struct {
		struct msg_hdr header;
		struct msg_wmname title;
	} msg;
	msg.header = (struct msg_hdr) {
		.type = MSG_WMNAME,
		.window = output->window_id,
		.untrusted_len = sizeof(struct msg_wmname),
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.title);
	strncpy(msg.title.data,
	        view->xdg_surface->toplevel->title,
	        sizeof(msg.title.data) - 1);
	msg.title.data[sizeof(msg.title.data) - 1] = '\0';
	// Asserted above, checked at call sites
	qubes_rust_send_message(output->server->backend->rust_backend, (struct msg_hdr *)&msg);
}

static void qubes_set_title(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WMNAME: ask GUI daemon to set window title */
	struct tinywl_view *view = wl_container_of(listener, view, set_title);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	assert(view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);
	if (qubes_output_mapped(output)) {
		// Mapped implies created
		qubes_set_view_title(view);
	}
}

static void qubes_set_view_app_id(struct tinywl_view *view)
{
	struct qubes_output *output = &view->output;

	assert(qubes_output_created(output));
	assert(output->window_id);
	wlr_log(WLR_DEBUG, "Sending MSG_WMCLASS (0x%x) to window %" PRIu32, MSG_WMCLASS, output->window_id);
	struct {
		struct msg_hdr header;
		struct msg_wmclass class;
	} msg = {
		.header = {
			.type = MSG_WMCLASS,
			.window = output->window_id,
			.untrusted_len = sizeof(struct msg_wmclass),
		},
		.class = {
			.res_class = { 0 },
			.res_name = { 0 },
		},
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.class);
	strncpy(msg.class.res_class, view->xdg_surface->toplevel->app_id, sizeof(msg.class.res_class) - 1);
	// Asserted above, checked at call sites
	qubes_rust_send_message(output->server->backend->rust_backend, (struct msg_hdr *)&msg);
}

static void qubes_set_app_id(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WMNAME: ask GUI daemon to set window app id */
	struct tinywl_view *view = wl_container_of(listener, view, set_app_id);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	assert(view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);
	if (qubes_output_mapped(output)) {
		// Mapped implies created
		qubes_set_view_app_id(view);
	}
}

static void xdg_surface_map(struct wl_listener *listener, void *data __attribute__((unused))) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	/* QUBES HOOK: MSG_MAP: map the corresponding window */
	struct tinywl_view *view = wl_container_of(listener, view, map);
	assert(QUBES_VIEW_MAGIC == view->output.magic);
	qubes_view_map(view);
}

static void xdg_surface_unmap(struct wl_listener *listener, void *data __attribute__((unused))) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	/* QUBES HOOK: MSG_UNMAP: unmap the corresponding window */
	struct tinywl_view *view = wl_container_of(listener, view, unmap);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);
	wlr_scene_node_set_enabled(&output->scene_output->scene->node, false);
	wlr_scene_node_set_enabled(output->scene_subsurface_tree, false);
	qubes_output_unmap(&view->output);
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data __attribute__((unused))) {
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
		wl_list_remove(&view->set_title.link);
		wl_list_remove(&view->set_app_id.link);
		wl_list_remove(&view->ack_configure.link);
	}
	qubes_output_deinit(&view->output);
	free(view);
}

static void qubes_surface_commit(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_HINTS, plus do a bunch of actual rendering stuff :) */
	struct tinywl_view *view = wl_container_of(listener, view, commit);
	struct qubes_output *output = &view->output;
	struct wlr_box box;

	assert(QUBES_VIEW_MAGIC == output->magic);
	assert(output->scene_output);
	assert(output->scene_output->output == &output->output);
	if (!qubes_view_ensure_created(view, &box))
		return;
	qubes_output_configure(output, box);
}

static void qubes_toplevel_ack_configure(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_surface_configure *configure = data;
	struct tinywl_view *view = wl_container_of(listener, view, ack_configure);
	struct qubes_output *output = &view->output;

	assert(QUBES_VIEW_MAGIC == output->magic);

	if (output->flags & QUBES_OUTPUT_IGNORE_CLIENT_RESIZE &&
	    view->configure_serial == configure->serial) {
		output->flags &= ~QUBES_OUTPUT_IGNORE_CLIENT_RESIZE;
		qubes_send_configure(output, output->last_width, output->last_height);
	}
}

void qubes_new_xdg_surface(struct wl_listener *listener, void *data) {
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
	if (!qubes_output_init(output, server,
	                       is_override_redirect, xdg_surface->surface,
	                       QUBES_VIEW_MAGIC))
		goto cleanup;

	view->xdg_surface = xdg_surface;

	/* Listen to the various events it can emit */
	view->map.notify = xdg_surface_map;
	wl_signal_add(&xdg_surface->events.map, &view->map);
	view->unmap.notify = xdg_surface_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
	view->destroy.notify = xdg_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
	xdg_surface->data = view;

	/* And listen to the various emits the toplevel can emit */
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		struct wlr_xdg_toplevel *const toplevel = xdg_surface->toplevel;
		view->request_maximize.notify = qubes_request_maximize;
		wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
		view->request_minimize.notify = qubes_request_minimize;
		wl_signal_add(&toplevel->events.request_minimize, &view->request_minimize);
		view->request_fullscreen.notify = qubes_request_fullscreen;
		wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
		view->set_title.notify = qubes_set_title;
		wl_signal_add(&toplevel->events.set_title, &view->set_title);
		view->set_app_id.notify = qubes_set_app_id;
		wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);
		view->ack_configure.notify = qubes_toplevel_ack_configure;
		wl_signal_add(&xdg_surface->events.ack_configure, &view->ack_configure);
	} else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_popup *const popup = xdg_surface->popup;
		struct wlr_box geometry = wlr_xdg_positioner_get_geometry(&popup->positioner);
		struct tinywl_view *parent_view = wlr_xdg_surface_from_wlr_surface(popup->parent)->data;
		assert(parent_view);
		output->left = geometry.x + parent_view->output.left;
		output->top = geometry.y + parent_view->output.top;
		output->last_width = geometry.width, output->last_height = geometry.height;
	} else {
		abort();
	}

	/* Listen to surface events */
	view->commit.notify = qubes_surface_commit;
	wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);

	/* Get the window ID */
	assert(output->window_id == 0);

	/* Tell GUI daemon to create window */
	struct wlr_box box;
	wlr_xdg_surface_get_geometry(xdg_surface, &box);
	if (box.width <= 0)
		box.width = 1;
	if (box.height <= 0)
		box.height = 1;
	wlr_output_set_custom_mode(&output->output, box.width, box.height, 60000);
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
	if (!qubes_view_ensure_created(view, &box))
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
			qubes_set_view_title(view);
		}
		if (xdg_surface->toplevel->app_id) {
			// Window created above, so this is safe
			qubes_set_view_app_id(view);
		}
		if (xdg_surface->toplevel->parent) {
			const struct qubes_output *parent_output = xdg_surface->toplevel->parent->data;
			transient_for_window = parent_output->window_id;
		}
	} else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_popup *popup = xdg_surface->popup;
		if (popup->parent) {
			const struct wlr_xdg_surface *parent_surface = wlr_xdg_surface_from_wlr_surface(popup->parent);
			transient_for_window = ((struct qubes_output *)parent_surface->data)->window_id;
		}
	} else {
		return;
	}
	qubes_output_map(output, transient_for_window, view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP ? 1 : 0);
}