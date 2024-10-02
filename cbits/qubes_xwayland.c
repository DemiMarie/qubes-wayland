#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "main.h"
#include "qubes_backend.h"
#include "qubes_xwayland.h"

#ifndef WINDOW_FLAG_MAXIMIZE
#define WINDOW_FLAG_MAXIMIZE 0
#endif

static bool xwayland_get_box(struct wlr_xwayland_surface *surface,
                             struct wlr_box *box)
{
	if (surface->width <= 0 || surface->height <= 0 ||
	    surface->width > MAX_WINDOW_WIDTH || surface->height > MAX_WINDOW_HEIGHT)
		return false; /* cannot handle this */
	*box = (struct wlr_box){
		.x = surface->x,
		.y = surface->y,
		.width = surface->width,
		.height = surface->height,
	};
	return true;
}

static void xwayland_surface_destroy(struct wl_listener *listener,
                                     void *data __attribute__((unused)))
{
	struct qubes_xwayland_view *view = wl_container_of(listener, view, destroy);

	wlr_log(WLR_DEBUG, "freeing view at %p", view);

	assert(QUBES_XWAYLAND_MAGIC == view->output.magic);

	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_class.link);
	wl_list_remove(&view->set_hints.link);
	wl_list_remove(&view->set_override_redirect.link);
	wl_list_remove(&view->set_geometry.link);
	wl_list_remove(&view->set_parent.link);
	if (view->commit.link.next)
		wl_list_remove(&view->commit.link);
	qubes_output_deinit(&view->output);
	memset(view, 0xFF, sizeof *view);
	free(view);
}

void qubes_xwayland_surface_map(struct qubes_xwayland_view *view)
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	/* QUBES HOOK: MSG_MAP: map the corresponding window */
	struct wlr_xwayland_surface *surface = view->xwayland_surface;
	assert(surface);
	if (surface->surface == NULL)
		return;
	wlr_log(WLR_DEBUG, "mapping surface at %p", view);
	struct qubes_output *output = &view->output;
	struct wlr_box box;
	uint32_t parent_window_id;

	assert(QUBES_XWAYLAND_MAGIC == output->magic);
	if (!xwayland_get_box(surface, &box))
		return;
	qubes_output_configure(output, box);
	assert(surface->surface);
	if (view->commit.link.next)
		wl_list_remove(&view->commit.link);
	wl_signal_add(&surface->surface->events.commit, &view->commit);

	qubes_output_set_surface(output, surface->surface);

	if (surface->parent) {
		struct qubes_xwayland_view *parent_view = surface->parent->data;

		assert(parent_view);
		assert(parent_view->output.magic == QUBES_XWAYLAND_MAGIC);
		parent_window_id = parent_view->output.window_id;
	} else {
		parent_window_id = 0;
	}

	qubes_output_map(output, parent_window_id, surface->override_redirect);
}

static void xwayland_surface_map(struct wl_listener *listener, void *data)
{
	struct qubes_xwayland_view *view = wl_container_of(listener, view, map);
	if (wl_list_empty(&view->map.link))
		return;
	assert(data == view->xwayland_surface);
	qubes_xwayland_surface_map(view);
}

static void xwayland_surface_unmap(struct wl_listener *listener, void *data)
{
	struct qubes_xwayland_view *view = wl_container_of(listener, view, unmap);
	if (wl_list_empty(&view->map.link))
		return;

	wlr_log(WLR_DEBUG, "unmapping surface at %p", view);
	assert(QUBES_XWAYLAND_MAGIC == view->output.magic);
	qubes_output_set_surface(&view->output, NULL);
	qubes_output_unmap(&view->output);
	wl_list_remove(&view->commit.link);
}

static void xwayland_surface_set_size(struct qubes_xwayland_view *view,
                                      int32_t x, int32_t y, uint32_t width,
                                      uint32_t height)
{
	struct qubes_output *output = &view->output;

	assert(QUBES_XWAYLAND_MAGIC == view->output.magic);
	if (width <= 0 || height <= 0 || width > MAX_WINDOW_WIDTH ||
	    height > MAX_WINDOW_HEIGHT || x < -MAX_WINDOW_WIDTH ||
	    x > 2 * MAX_WINDOW_WIDTH || y < -MAX_WINDOW_HEIGHT ||
	    y > 2 * MAX_WINDOW_HEIGHT) {
		wlr_log(WLR_ERROR,
		        "Bad message from client: width %" PRIu16 " height %" PRIu16,
		        width, height);
		return; /* cannot handle this */
	}
	struct wlr_box box = {
		.width = width,
		.height = height,
		.x = x,
		.y = y,
	};
	qubes_output_configure(output, box);
}

static void xwayland_surface_request_configure(struct wl_listener *listener,
                                               void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, request_configure);
	if (wl_list_empty(&view->map.link))
		return;
	struct wlr_xwayland_surface_configure_event *event = data;
	struct qubes_output *output = &view->output;
	int32_t x = event->x, y = event->y, width = event->width,
	        height = event->height;
	assert(output->magic == QUBES_XWAYLAND_MAGIC);
	assert(view->xwayland_surface == event->surface);

	if (width <= 0 || height <= 0 || width > MAX_WINDOW_WIDTH ||
	    height > MAX_WINDOW_HEIGHT || x < -MAX_WINDOW_WIDTH ||
	    x > 2 * MAX_WINDOW_WIDTH || y < -MAX_WINDOW_HEIGHT ||
	    y > 2 * MAX_WINDOW_HEIGHT) {
		wlr_log(WLR_ERROR,
		        "Bad message from client: width %" PRIu16 " height %" PRIu16,
		        width, height);
		return; /* cannot handle this */
	}

	struct wlr_box box = {
		.width = width,
		.height = height,
		.x = x,
		.y = y,
	};
	qubes_output_configure(output, box);
}

static void xwayland_surface_request_minimize(struct wl_listener *listener,
                                              void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, request_minimize);
	if (wl_list_empty(&view->map.link))
		return;
	struct wlr_xwayland_minimize_event *event = data;

	assert(view->output.magic == QUBES_XWAYLAND_MAGIC);
	assert(view->destroy.link.next);
	if (qubes_output_mapped(&view->output)) {
		wlr_log(WLR_DEBUG, "Marking window %" PRIu32 " %sminimized",
		        view->output.window_id, event->minimize ? "" : "not ");
		// Mapped implies created
		qubes_change_window_flags(
		   &view->output, event->minimize ? WINDOW_FLAG_MINIMIZE : 0,
		   event->minimize ? WINDOW_FLAG_MAXIMIZE | WINDOW_FLAG_FULLSCREEN
		                   : WINDOW_FLAG_MINIMIZE);
	}
}

static void xwayland_surface_request_maximize(struct wl_listener *listener,
                                              void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, request_maximize);
	struct wlr_xwayland_surface *surface = data;
	if (surface->surface == NULL)
		return;

	assert(view->destroy.link.next);
#if WINDOW_FLAG_MAXIMIZE
	bool maximized = surface->maximized_vert && surface->maximized_horz;
	if (qubes_output_mapped(output)) {
		wlr_log(WLR_DEBUG, "Maximizing window " PRIu32, output->window_id);
		// Mapped implies created
		qubes_change_window_flags(
		   &view->output, maximized ? WINDOW_FLAG_MAXIMIZE : 0,
		   maximized ? WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_MINIMIZE
		             : WINDOW_FLAG_MAXIMIZE);
	}
#else
	(void)surface;
	wlr_log(WLR_ERROR, "window %" PRIu32 ": maximize: not implemented",
	        view->output.window_id);
#endif
}

static void xwayland_surface_request_fullscreen(struct wl_listener *listener,
                                                void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, request_fullscreen);
	struct wlr_xwayland_surface *surface = data;
	if (surface->surface == NULL)
		return;
	struct qubes_output *output = &view->output;

	assert(view->destroy.link.next);
	assert(QUBES_XWAYLAND_MAGIC == output->magic);
	if (qubes_output_mapped(output)) {
		wlr_log(WLR_DEBUG, "Marking window %" PRIu32 " fullscreen",
		        output->window_id);
		// Mapped implies created
		qubes_change_window_flags(
		   &view->output, surface->fullscreen ? WINDOW_FLAG_FULLSCREEN : 0,
		   surface->fullscreen ? WINDOW_FLAG_MINIMIZE : WINDOW_FLAG_FULLSCREEN);
	}
}

static void xwayland_surface_set_title(struct wl_listener *listener, void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, set_title);
	(void)data;
	if (wl_list_empty(&view->map.link))
		return;
	assert(view->destroy.link.next);
	if (view->xwayland_surface->title && qubes_output_created(&view->output)) {
		qubes_set_view_title(&view->output, view->xwayland_surface->title);
	}
}

static void xwayland_surface_set_geometry(struct wl_listener *listener,
                                          void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, set_geometry);
	(void)data; /* always NULL */
	if (wl_list_empty(&view->map.link))
		return;

	xwayland_surface_set_size(
	   view, view->xwayland_surface->x, view->xwayland_surface->y,
	   view->xwayland_surface->width, view->xwayland_surface->height);
}

static void xwayland_surface_set_class(struct wl_listener *listener, void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, set_class);
	struct wlr_xwayland_surface *surface = data;
	if (surface->surface == NULL)
		return;

	assert(view->output.magic == QUBES_XWAYLAND_MAGIC);
	assert(view->destroy.link.next);

	if (qubes_output_mapped(&view->output)) {
		qubes_output_set_class(&view->output, surface->class);
	}
}

static void xwayland_surface_set_hints(struct wl_listener *listener, void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, set_hints);
	struct wlr_xwayland_surface *surface = data;
	if (surface->surface == NULL)
		return;

	assert(view == surface->data);
	assert(view->output.magic == QUBES_XWAYLAND_MAGIC);
	assert(view->destroy.link.next);

	xcb_size_hints_t *hints = surface->size_hints;
	if (!qubes_output_ensure_created(&view->output))
		return;
	wlr_output_send_frame(&view->output.output);
	const uint32_t allowed_flags =
	   (XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION |
	    XCB_ICCCM_SIZE_HINT_P_MIN_SIZE | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE |
	    XCB_ICCCM_SIZE_HINT_P_RESIZE_INC | XCB_ICCCM_SIZE_HINT_BASE_SIZE);
	assert(view->output.window_id != 0);
	// clang-format off
	struct {
		struct msg_hdr header;
		struct msg_window_hints hints;
	} msg = {
		.header = {
			 .type = MSG_WINDOW_HINTS,
			 .window = view->output.window_id,
			 .untrusted_len = sizeof(msg.hints),
		},
		.hints = hints ? (struct msg_window_hints) {
			.flags = hints->flags & allowed_flags,
			.min_width = hints->min_width,
			.min_height = hints->min_height,
			.max_width = hints->max_width,
			.max_height = hints->max_height,
			.width_inc = hints->width_inc,
			.height_inc = hints->height_inc,
			.base_width = hints->base_width,
			.base_height = hints->base_height,
		 } : (struct msg_window_hints) { 0 },
	};
	// clang-format on
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.hints);
	qubes_rust_send_message(view->output.server->backend->rust_backend,
				(struct msg_hdr *)&msg);
}

static void xwayland_surface_set_override_redirect(struct wl_listener *listener,
                                                   void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, set_override_redirect);
	struct wlr_xwayland_surface *surface = data;
	if (surface->surface == NULL)
		return;
	assert(view->destroy.link.next);
	assert(view->output.magic == QUBES_XWAYLAND_MAGIC);
	if (surface->override_redirect)
		view->output.flags |= QUBES_OUTPUT_OVERRIDE_REDIRECT;
	else
		view->output.flags &= ~QUBES_OUTPUT_OVERRIDE_REDIRECT;
}

static void qubes_xwayland_surface_commit(struct wl_listener *listener,
                                          void *data __attribute__((unused)))
{
	/* QUBES HOOK: MSG_WINDOW_HINTS, plus do a bunch of actual rendering stuff :)
	 */
	struct qubes_xwayland_view *view = wl_container_of(listener, view, commit);
	struct qubes_output *output = &view->output;
	struct wlr_box box;
	struct wlr_xwayland_surface *surface;

	assert(QUBES_XWAYLAND_MAGIC == output->magic);
	assert(output->scene_output);
	assert(output->scene_output->output == &output->output);
	surface = view->xwayland_surface;
	if (!xwayland_get_box(surface, &box)) {
		wlr_log(WLR_ERROR, "NO BOX");
		return;
	}
	qubes_output_configure(output, box);
}

static void qubes_xwayland_surface_set_parent(struct wl_listener *listener,
                                              void *data)
{
	struct qubes_xwayland_view *view =
	   wl_container_of(listener, view, set_parent);
	struct qubes_xwayland_view *parent_view;
	struct wlr_xwayland_surface *surface = data;
	struct wlr_xwayland_surface *parent;

	if (surface->surface == NULL)
		return;
	assert(!wl_list_empty(&view->map.link));
	struct qubes_output *output = &view->output;

	assert(QUBES_XWAYLAND_MAGIC == output->magic);
	assert(surface);
	assert(surface == view->xwayland_surface);
	parent = surface->parent;
	parent_view = parent->data;

	if (parent) {
		wlr_log(WLR_DEBUG,
		        "Setting parent of surface %p (%dx%d) to %p (coordinates %dx%d)",
		        view, surface->x, surface->y, parent_view, parent->x, parent->y);
		struct wlr_box box = {
			.x = surface->x,
			.y = surface->y,
			.width = surface->width,
			.height = surface->height,
		};
		qubes_output_configure(output, box);
	} else {
		wlr_log(WLR_DEBUG, "Unsetting parent of surface %p (coordinates %dx%d)",
		        view, surface->x, surface->y);
	}
}

static void
qubes_xwayland_xwayland_surface_associate(struct wl_listener *listener,
                                          void *data)
{
	struct qubes_xwayland_view *view = wl_container_of(listener, view, associate);
	assert(data == NULL);
	assert(wl_list_empty(&view->map.link));
	struct wlr_xwayland_surface *surface = view->xwayland_surface;
	struct tinywl_server *server = view->server;
	if (!qubes_output_init(&view->output, server, surface->override_redirect,
	                       surface->surface, QUBES_XWAYLAND_MAGIC, surface->x,
	                       surface->y, surface->width, surface->height)) {
		/* TODO: disconnect xwayland with no_memory */
		abort();
	}
	wl_signal_add(&surface->surface->events.map, &view->map);
	wl_signal_add(&surface->surface->events.unmap, &view->unmap);
	wl_signal_add(&surface->surface->events.commit, &view->commit);
}

static void
qubes_xwayland_xwayland_surface_dissociate(struct wl_listener *listener,
                                           void *data)
{
	struct qubes_xwayland_view *view = wl_container_of(listener, view, associate);
	assert(data == NULL);
	assert(!wl_list_empty(&view->map.link));
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_init(&view->map.link);
	wl_list_init(&view->unmap.link);
	wl_list_init(&view->commit.link);
	qubes_output_deinit(&view->output);
}



void qubes_xwayland_new_xwayland_surface(struct wl_listener *listener,
                                         void *data)
{
	struct tinywl_server *server =
	   wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *surface = data;

	assert(surface);
	assert(surface->surface == NULL && "wlroots already allocated surface");
	assert(QUBES_SERVER_MAGIC == server->magic);

	struct qubes_xwayland_view *view = calloc(1, sizeof(*view));
	if (!view) {
		wlr_log(WLR_ERROR, "Could not allocate view for Xwayland surface");
		return;
	}

	wlr_log(WLR_DEBUG,
	        "New Xwayland surface: coordinates %dx%d w %d h %d%s pointer %p",
	        surface->x, surface->y, surface->width, surface->height,
	        surface->override_redirect ? " (override-redirect)" : "", view);

	struct qubes_output *output = &view->output;

	if (surface->surface != NULL &&
	    !qubes_output_init(output, server, surface->override_redirect,
	                       surface->surface, QUBES_XWAYLAND_MAGIC, surface->x,
	                       surface->y, surface->width, surface->height)) {
		goto cleanup;
	}

	view->xwayland_surface = surface;

	view->destroy.notify = xwayland_surface_destroy;
	wl_signal_add(&surface->events.destroy, &view->destroy);
	view->request_configure.notify = xwayland_surface_request_configure;
	wl_signal_add(&surface->events.request_configure, &view->request_configure);
	view->request_minimize.notify = xwayland_surface_request_minimize;
	wl_signal_add(&surface->events.request_minimize, &view->request_minimize);
	view->request_maximize.notify = xwayland_surface_request_maximize;
	wl_signal_add(&surface->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = xwayland_surface_request_fullscreen;
	wl_signal_add(&surface->events.request_fullscreen,
	              &view->request_fullscreen);
	view->set_title.notify = xwayland_surface_set_title;
	wl_signal_add(&surface->events.set_title, &view->set_title);
	view->set_class.notify = xwayland_surface_set_class;
	wl_signal_add(&surface->events.set_class, &view->set_class);
	view->set_hints.notify = xwayland_surface_set_hints;
	wl_signal_add(&surface->events.set_hints, &view->set_hints);
	view->set_override_redirect.notify = xwayland_surface_set_override_redirect;
	wl_signal_add(&surface->events.set_override_redirect,
	              &view->set_override_redirect);
	view->set_geometry.notify = xwayland_surface_set_geometry;
	wl_signal_add(&surface->events.set_geometry, &view->set_geometry);
	view->set_parent.notify = qubes_xwayland_surface_set_parent;
	wl_signal_add(&surface->events.set_parent, &view->set_parent);
	view->associate.notify = qubes_xwayland_xwayland_surface_associate;
	wl_signal_add(&surface->events.associate, &view->associate);
	view->dissociate.notify = qubes_xwayland_xwayland_surface_dissociate;
	wl_signal_add(&surface->events.dissociate, &view->dissociate);
	view->commit.notify = qubes_xwayland_surface_commit;
	view->map.notify = xwayland_surface_map;
	view->unmap.notify = xwayland_surface_unmap;
	if (surface->surface) {
		wl_signal_add(&surface->surface->events.map, &view->map);
		wl_signal_add(&surface->surface->events.unmap, &view->unmap);
		wl_signal_add(&surface->surface->events.commit, &view->commit);
	} else {
		wl_list_init(&view->map.link);
		wl_list_init(&view->unmap.link);
		wl_list_init(&view->commit.link);
	}
	wlr_log(WLR_DEBUG, "created surface at %p", view);
	surface->data = view;
	view->server = server;
	return;

cleanup:
	if (view) {
		qubes_output_deinit(&view->output);
		free(view);
	}
}
