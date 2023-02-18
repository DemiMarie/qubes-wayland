#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_scene.h>

#include "qubes_backend.h"
#include "qubes_xwayland.h"
#include "main.h"

static bool xwayland_get_box(struct wlr_xwayland_surface *surface, struct wlr_box *box)
{
	if (surface->width <= 0 ||
		 surface->height <= 0 ||
		 surface->width > MAX_WINDOW_WIDTH ||
		 surface->height > MAX_WINDOW_HEIGHT)
		return false; /* cannot handle this */
	*box = (struct wlr_box) {
		.x = surface->x,
		.y = surface->y,
		.width = surface->width,
		.height = surface->height,
	};
	return true;
}

static void xwayland_surface_destroy(struct wl_listener *listener, void *data __attribute__((unused)))
{
	struct qubes_xwayland_view *view = wl_container_of(listener, view, destroy);

	wlr_log(WLR_DEBUG, "freeing view at %p", view);

	assert(QUBES_XWAYLAND_MAGIC == view->output.magic);

	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_class.link);
	wl_list_remove(&view->set_parent.link);
	wl_list_remove(&view->set_hints.link);
	wl_list_remove(&view->set_override_redirect.link);
	if (view->commit.link.next)
		wl_list_remove(&view->commit.link);
	qubes_output_deinit(&view->output);
	memset(view, 0xFF, sizeof *view);
	free(view);
}

void qubes_xwayland_surface_map(struct qubes_xwayland_view *view) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	/* QUBES HOOK: MSG_MAP: map the corresponding window */
	wlr_log(WLR_DEBUG, "mapping surface at %p", view);
	struct wlr_xwayland_surface *surface = view->xwayland_surface;
	assert(surface);
	assert(surface->surface);
	struct qubes_output *output = &view->output;
	struct wlr_box box;

	assert(QUBES_XWAYLAND_MAGIC == output->magic);
	if (!xwayland_get_box(surface, &box))
		return;
	qubes_output_configure(output, box);
	output->flags |= QUBES_OUTPUT_MAPPED;
	assert(surface->surface);
	if (view->commit.link.next)
		wl_list_remove(&view->commit.link);
	wl_signal_add(&surface->surface->events.commit, &view->commit);

	qubes_output_set_surface(output, surface->surface);
	qubes_output_map(output, 0, surface->override_redirect);
}

static void xwayland_surface_map(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, map);
	assert(data == view->xwayland_surface);
	qubes_xwayland_surface_map(view);
}

static void xwayland_surface_unmap(struct wl_listener *listener, void *data)
{
	struct qubes_xwayland_view *view = wl_container_of(listener, view, unmap);

	wlr_log(WLR_DEBUG, "unmapping surface at %p", view);
	assert(QUBES_XWAYLAND_MAGIC == view->output.magic);
	qubes_output_set_surface(&view->output, NULL);
	qubes_output_unmap(&view->output);
	wl_list_remove(&view->commit.link);
}

static void xwayland_surface_set_size(struct qubes_xwayland_view *view,
                                      int32_t x, int32_t y,
                                      uint32_t width, uint32_t height) {
	struct qubes_output *output = &view->output;
	wlr_log(WLR_DEBUG, "configuring surface at %p", view);
	assert(QUBES_XWAYLAND_MAGIC == view->output.magic);
	if (width <= 0 || height <= 0 ||
	    width > MAX_WINDOW_WIDTH || height > MAX_WINDOW_HEIGHT ||
	    x < -MAX_WINDOW_WIDTH || x > 2 * MAX_WINDOW_WIDTH ||
	    y < -MAX_WINDOW_HEIGHT || y > 2 * MAX_WINDOW_HEIGHT) {
	    wlr_log(WLR_ERROR, "Bad message from client: width %" PRIu16 " height %" PRIu16, width, height);
		return; /* cannot handle this */
	}
	qubes_send_configure(output, width, height);
}

static void xwayland_surface_request_configure(struct wl_listener *listener, void *data)
{
	struct qubes_xwayland_view *view = wl_container_of(listener, view, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;

	wlr_xwayland_surface_configure(view->xwayland_surface, event->x, event->y, event->width, event->height);
}

static void xwayland_surface_request_move(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, request_move);
	struct wlr_xwayland_move_event *event = data;
	(void)event, (void)view;
	assert(view->destroy.link.next);
	wlr_log(WLR_ERROR, "Move request for Xwayland window %" PRIu32 " not yet implemented",
	        view->output.window_id);
}

static void xwayland_surface_request_resize(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xwayland_resize_event *event = data;
	(void)event, (void)view;
	assert(view->destroy.link.next);
	wlr_log(WLR_ERROR, "Resize request for Xwayland window %" PRIu32 " not yet implemented",
	        view->output.window_id);
}

static void xwayland_surface_request_minimize(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, request_minimize);
	struct wlr_xwayland_minimize_event *event = data;
	(void)event, (void)view;
	assert(view->destroy.link.next);
	wlr_log(WLR_ERROR, "Minimize request for Xwayland window %" PRIu32 " not yet implemented",
	        view->output.window_id);
}
static void xwayland_surface_request_maximize(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, request_maximize);
	struct wlr_xwayland_surface *surface = data;
	(void)view, (void)surface;
	assert(view->destroy.link.next);
	wlr_log(WLR_ERROR, "Maximize request for Xwayland window %" PRIu32 " not yet implemented",
	        view->output.window_id);
}
static void xwayland_surface_request_fullscreen(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, request_fullscreen);
	struct wlr_xwayland_surface *surface = data;
	(void)view, (void)surface;
	assert(view->destroy.link.next);
	wlr_log(WLR_ERROR, "Fullscreen request for Xwayland window %" PRIu32 " not yet implemented",
	        view->output.window_id);
}
static void xwayland_surface_set_title(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, set_title);
	(void)data;
	assert(view->destroy.link.next);
	if (view->xwayland_surface->title)
		qubes_set_view_title(&view->output, view->xwayland_surface->title);
}

static void xwayland_surface_set_geometry(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, set_geometry);
	(void)data; /* always NULL */

	xwayland_surface_set_size(view,
	                          view->xwayland_surface->x,
	                          view->xwayland_surface->y,
	                          view->xwayland_surface->width,
	                          view->xwayland_surface->height);
}

static void xwayland_surface_set_class(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, set_class);
	struct wlr_xwayland_surface *surface = data;
	(void)view, (void)surface;
	assert(view->destroy.link.next);
	wlr_log(WLR_ERROR, "Set-class request for Xwayland window %" PRIu32 " not yet implemented",
	        view->output.window_id);
}

static void xwayland_surface_set_parent(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, set_parent);
	struct wlr_xwayland_surface *surface = data;
	(void)view, (void)surface;
	assert(view->destroy.link.next);
	wlr_log(WLR_ERROR, "Set-parent request for Xwayland window %" PRIu32 " not yet implemented",
	        view->output.window_id);
}

static void xwayland_surface_set_hints(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, set_hints);
	struct wlr_xwayland_surface *surface = data;
	(void)view, (void)surface;
	assert(view->destroy.link.next);
	wlr_log(WLR_ERROR, "Set-hints request for Xwayland window %" PRIu32 " not yet implemented",
	        view->output.window_id);
}

static void xwayland_surface_set_override_redirect(struct wl_listener *listener, void *data) {
	struct qubes_xwayland_view *view = wl_container_of(listener, view, set_override_redirect);
	struct wlr_xwayland_surface *surface = data;
	assert(view->destroy.link.next);
	assert(view->output.magic == QUBES_XWAYLAND_MAGIC);
	if (surface->override_redirect)
		view->output.flags |= QUBES_OUTPUT_OVERRIDE_REDIRECT;
	else
		view->output.flags &= ~QUBES_OUTPUT_OVERRIDE_REDIRECT;
}

static void qubes_xwayland_surface_commit(
		struct wl_listener *listener, void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_HINTS, plus do a bunch of actual rendering stuff :) */
	struct qubes_xwayland_view *view = wl_container_of(listener, view, commit);
	struct qubes_output *output = &view->output;
	struct wlr_box box;

	assert(QUBES_XWAYLAND_MAGIC == output->magic);
	assert(output->scene_output);
	assert(output->scene_output->output == &output->output);
	if (!xwayland_get_box(view->xwayland_surface, &box)) {
		wlr_log(WLR_ERROR, "NO BOX");
		return;
	}
	qubes_output_configure(output, box);
}

void qubes_xwayland_new_xwayland_surface(struct wl_listener *listener, void *data)
{
	struct tinywl_server *server = wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *surface = data;
	assert(surface);

	assert(QUBES_SERVER_MAGIC == server->magic);

	struct qubes_xwayland_view *view = calloc(1, sizeof(*view));
	if (!view)
		return;

	struct qubes_output *output = &view->output;

	if (!qubes_output_init(output, server,
	                       surface->override_redirect, surface->surface,
	                       QUBES_XWAYLAND_MAGIC))
		goto cleanup;

	output->left = surface->x;
	output->top = surface->y;
	output->last_width = surface->width;
	output->last_height = surface->height;

	view->xwayland_surface = surface;

	view->destroy.notify = xwayland_surface_destroy;
	wl_signal_add(&surface->events.destroy, &view->destroy);
	view->map.notify = xwayland_surface_map;
	wl_signal_add(&surface->events.map, &view->map);
	view->unmap.notify = xwayland_surface_unmap;
	wl_signal_add(&surface->events.unmap, &view->unmap);
	view->request_configure.notify = xwayland_surface_request_configure;
	wl_signal_add(&surface->events.request_configure, &view->request_configure);
	view->request_move.notify = xwayland_surface_request_move;
	wl_signal_add(&surface->events.request_move, &view->request_move);
	view->request_resize.notify = xwayland_surface_request_resize;
	wl_signal_add(&surface->events.request_resize, &view->request_resize);
	view->request_minimize.notify = xwayland_surface_request_minimize;
	wl_signal_add(&surface->events.request_minimize, &view->request_minimize);
	view->request_maximize.notify = xwayland_surface_request_maximize;
	wl_signal_add(&surface->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = xwayland_surface_request_fullscreen;
	wl_signal_add(&surface->events.request_fullscreen, &view->request_fullscreen);
	view->set_title.notify = xwayland_surface_set_title;
	wl_signal_add(&surface->events.set_title, &view->set_title);
	view->set_class.notify = xwayland_surface_set_class;
	wl_signal_add(&surface->events.set_class, &view->set_class);
	view->set_parent.notify = xwayland_surface_set_parent;
	wl_signal_add(&surface->events.set_parent, &view->set_parent);
	view->set_hints.notify = xwayland_surface_set_hints;
	wl_signal_add(&surface->events.set_hints, &view->set_hints);
	view->set_override_redirect.notify = xwayland_surface_set_override_redirect;
	wl_signal_add(&surface->events.set_override_redirect, &view->set_override_redirect);
	view->set_geometry.notify = xwayland_surface_set_geometry;
	wl_signal_add(&surface->events.set_geometry, &view->set_geometry);
	view->commit.notify = qubes_xwayland_surface_commit;
	if (surface->surface)
		wl_signal_add(&surface->surface->events.commit, &view->commit);
	wlr_log(WLR_DEBUG, "created surface at %p", view);
	return;

cleanup:
	if (view) {
		qubes_output_deinit(&view->output);
		free(view);
	}
}
