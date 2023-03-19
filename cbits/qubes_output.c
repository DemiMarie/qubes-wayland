// wlr_output implementation and redraw code

#include "common.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>

#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "main.h"
#include "qubes_allocator.h"
#include "qubes_backend.h"
#include "qubes_output.h"
#include "qubes_xwayland.h"
#include <drm/drm_fourcc.h>

/* Qubes OS doesn’t support gamma LUTs */
static size_t qubes_get_gamma_size(struct wlr_output *output
                                   __attribute__((unused)))
{
	return 0;
}

static void qubes_unlink_buffer(struct qubes_output *buffer)
{
	if (buffer->buffer)
		wl_list_remove(&buffer->buffer_destroy.link);
	buffer->buffer = NULL;
}

static void qubes_unlink_buffer_listener(struct wl_listener *listener,
                                         void *data __attribute__((unused)))
{
	struct qubes_output *output =
	   wl_container_of(listener, output, buffer_destroy);
	qubes_unlink_buffer(output);
}

static const struct wlr_output_impl qubes_wlr_output_impl;

static void qubes_output_deinit_raw(struct wlr_output *raw_output)
{
	assert(raw_output->impl == &qubes_wlr_output_impl);
	struct qubes_output *output = wl_container_of(raw_output, output, output);
	wl_list_remove(&output->frame.link);
	wlr_buffer_unlock(output->buffer);
	qubes_unlink_buffer(output);
}

static bool qubes_output_test(struct wlr_output *raw_output,
                              const struct wlr_output_state *state)
{
	assert(raw_output->impl == &qubes_wlr_output_impl);
	struct qubes_output *output = wl_container_of(raw_output, output, output);
	if ((state->committed & WLR_OUTPUT_STATE_BUFFER) &&
	    (state->buffer != NULL) &&
	    (state->buffer->impl != qubes_buffer_impl_addr))
		return false;
	return true;
}

static void qubes_output_damage(struct qubes_output *output, struct wlr_box box,
                                const struct wlr_output_state *state)
{
	if (0)
		wlr_log(
		   WLR_DEBUG, "X is %d Y is %d Width is %" PRIu32 " height is %" PRIu32,
		   (int)box.x, (int)box.y, (uint32_t)box.width, (uint32_t)box.height);
	if (state && !(state->committed & WLR_OUTPUT_STATE_DAMAGE))
		return;
	pixman_box32_t fake_rect = {
		.x1 = 0, .y1 = 0, .x2 = box.width, .y2 = box.height
	};
	pixman_box32_t *rects;
	int n_rects;
	if (state) {
		n_rects = 0;
		rects = pixman_region32_rectangles((pixman_region32_t *)&state->damage,
		                                   &n_rects);
		if (n_rects <= 0 || !rects) {
			wlr_log(WLR_DEBUG, "No damage!");
			return;
		}
	} else {
		n_rects = 1;
		rects = &fake_rect;
	}
	if (0)
		wlr_log(WLR_DEBUG, "Sending MSG_SHMIMAGE (0x%x) to window %" PRIu32,
		        MSG_SHMIMAGE, output->window_id);
	for (int i = 0; i < n_rects; ++i) {
		int32_t width, height;
		if (__builtin_sub_overflow(rects[i].x2, rects[i].x1, &width) ||
		    __builtin_sub_overflow(rects[i].y2, rects[i].y1, &height)) {
			wlr_log(WLR_ERROR, "Overflow in damage calc");
			return;
		}
		if (width <= 0 || height <= 0) {
			wlr_log(WLR_ERROR, "Negative width or height - skipping");
			continue;
		}
		if (0)
			wlr_log(WLR_DEBUG,
			        "Submitting damage to GUI daemon: window %" PRIu32
			        " x %" PRIi32 " y %" PRIi32 " width %" PRIu32
			        " height %" PRIu32,
			        output->window_id, rects[i].x1, rects[i].y1, width, height);
		// clang-format off
		struct {
			struct msg_hdr header;
			struct msg_shmimage shmimage;
		} new_msg = {
		   .header = {
				.type = MSG_SHMIMAGE,
				.window = output->window_id,
				.untrusted_len = sizeof(struct msg_shmimage),
			},
		   .shmimage = {
				.x = rects[i].x1,
				.y = rects[i].y1,
				.width = width,
				.height = height,
			},
		};
		// clang-format on
		QUBES_STATIC_ASSERT(sizeof new_msg ==
		                    sizeof new_msg.header + sizeof new_msg.shmimage);
		// Created above
		qubes_rust_send_message(output->server->backend->rust_backend,
		                        (struct msg_hdr *)&new_msg);
	}
}

void qubes_output_dump_buffer(struct qubes_output *output, struct wlr_box box,
                              const struct wlr_output_state *state)
{
	assert(output->buffer->impl == qubes_buffer_impl_addr);
	struct tinywl_server *server = output->server;
	if (0)
		wlr_log(WLR_DEBUG, "Sending MSG_WINDOW_DUMP (0x%x) to window %" PRIu32,
		        MSG_WINDOW_DUMP, output->window_id);
	struct qubes_link *link = malloc(sizeof(*link));
	if (link == NULL) {
		wlr_log(WLR_ERROR, "Cannot allocate %zu bytes?", sizeof(*link));
		abort(); /* FIXME */
	}
	struct qubes_buffer *buffer = wl_container_of(output->buffer, buffer, inner);
	if (server->backend->protocol_version >= 0x10007) {
		assert(buffer->refcount != 0);
		assert(buffer->refcount < INT32_MAX);
		buffer->refcount++;
		link->next = NULL;
		link->buffer = buffer;
		if (server->queue_tail) {
			assert(server->queue_head != NULL);
			server->queue_tail->next = link;
		} else {
			assert(server->queue_head == NULL);
			server->queue_head = link;
		}
		output->server->queue_tail = link;
	}
	buffer->header.window = output->window_id;
	buffer->header.type = MSG_WINDOW_DUMP;
	buffer->header.untrusted_len =
	   sizeof(buffer->qubes) + NUM_PAGES(buffer->size) * SIZEOF_GRANT_REF;
	qubes_rust_send_message(output->server->backend->rust_backend,
	                        &buffer->header);
	qubes_output_damage(output, box, state);
}

void qubes_output_move(struct qubes_output *output, int32_t x, int32_t y)
{
	if ((output->x == x) && (output->y == y)) {
		return;
	}

	/* Output position has changed.  Update accordingly. */
	output->x = x;
	output->y = y;

	/*
	 * The Qubes GUI protocol uses coordinates relative to the top left of the
	 * screen, since those are the coordinates X11 uses.  Native Wayland
	 * windows, however, use output-relative coordinates.  Therefore,
	 * wlr_scene needs to be told of its position so that it can translate
	 * coordinates appropriately.  XWayland windows, however, are already in
	 * the coordinates used by the Qubes GUI protocol, so they must not be
	 * translated.  The symptom of getting this wrong is black bars near the
	 * edge of a maximized window, or non-maximized windows not displaying
	 * anything if its position on screen is wrong.
	 */
	if (output->magic == QUBES_VIEW_MAGIC) {
		wlr_scene_output_set_position(output->scene_output, x, y);
	} else {
		assert(output->magic == QUBES_XWAYLAND_MAGIC);
	}
}

bool qubes_output_ensure_created(struct qubes_output *output,
                                 struct wlr_box box)
{
	// implemented in Rust
	extern uint32_t qubes_rust_generate_id(void *backend, void *data)
	   __attribute__((warn_unused_result));
	if (box.width <= 0 || box.height <= 0 || box.width > MAX_WINDOW_WIDTH ||
	    box.height > MAX_WINDOW_HEIGHT) {
		return false;
	}
	qubes_output_move(output, box.x, box.y);
	if (qubes_output_created(output))
		return true;
	if (!output->window_id)
		output->window_id =
		   qubes_rust_generate_id(output->server->backend->rust_backend, output);
	// clang-format off
	struct {
		struct msg_hdr header;
		struct msg_create create;
	} msg = {
		.header = {
			.type = MSG_CREATE,
			.window = output->window_id,
			.untrusted_len = sizeof(struct msg_create),
		},
		.create = {
			.x = output->left,
			.y = output->top,
			.width = box.width,
			.height = box.height,
			.parent = 0,
			.override_redirect = (output->flags & QUBES_OUTPUT_OVERRIDE_REDIRECT) ? 1 : 0,
		},
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.create);
	// clang-format on
	// This is MSG_CREATE
	wlr_log(WLR_DEBUG, "Sending MSG_CREATE (0x%x) to window %" PRIu32,
	        MSG_CREATE, output->window_id);
	qubes_rust_send_message(output->server->backend->rust_backend,
	                        (struct msg_hdr *)&msg);
	output->flags |= QUBES_OUTPUT_CREATED;
	return true;
}

static bool qubes_output_commit(struct wlr_output *raw_output,
                                const struct wlr_output_state *state)
{
	assert(raw_output->impl == &qubes_wlr_output_impl);
	assert(state);
	struct qubes_output *output = wl_container_of(raw_output, output, output);
	struct wlr_box box;
	if (QUBES_VIEW_MAGIC == output->magic) {
		struct tinywl_view *view = wl_container_of(output, view, output);
		wlr_xdg_surface_get_geometry(view->xdg_surface, &box);
	} else if (QUBES_XWAYLAND_MAGIC == output->magic) {
		struct qubes_xwayland_view *view = wl_container_of(output, view, output);
		struct wlr_xwayland_surface *surface = view->xwayland_surface;
		assert(surface);
		box.x = surface->x;
		box.y = surface->y;
		box.width = surface->width;
		box.height = surface->height;
	} else {
		assert(!"Bad magic in qubes_output_commit");
		abort();
	}
	if (!qubes_output_ensure_created(output, box))
		return false;

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		assert(state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
		wlr_output_update_custom_mode(raw_output, state->custom_mode.width,
		                              state->custom_mode.height,
		                              state->custom_mode.refresh);
	}

	if ((state->committed & WLR_OUTPUT_STATE_BUFFER) &&
	    (output->buffer != state->buffer)) {
		if (output->buffer) {
			wl_list_remove(&output->buffer_destroy.link);
			wlr_buffer_unlock(output->buffer);
		}

		if ((output->buffer = state->buffer)) {
			wlr_buffer_lock(output->buffer);
			wl_signal_add(&output->buffer->events.destroy,
			              &output->buffer_destroy);
			qubes_output_dump_buffer(output, box, state);
		}
	}
	if (state->committed & WLR_OUTPUT_STATE_ENABLED)
		wlr_output_update_enabled(raw_output, state->enabled);
	return true;
}

static const struct wlr_drm_format xrgb8888 = {
	.format = DRM_FORMAT_XRGB8888,
	.len = 2,
	.capacity = 0,
	.modifiers = { DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR },
};
static const struct wlr_drm_format argb8888 = {
	.format = DRM_FORMAT_ARGB8888,
	.len = 2,
	.capacity = 0,
	.modifiers = { DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR },
};

static const struct wlr_drm_format *const global_pointer_array[2] = {
	&xrgb8888,
	&argb8888,
};

static const struct wlr_drm_format_set global_formats = {
	.len = 2,
	.capacity = 0,
	.formats = (struct wlr_drm_format **)global_pointer_array,
};

static const struct wlr_drm_format_set *qubes_output_get_primary_formats(
   struct wlr_output *output __attribute__((unused)), uint32_t buffer_caps)
{
	return &global_formats;
}

static const struct wlr_output_impl qubes_wlr_output_impl = {
	.set_cursor = NULL,
	.move_cursor = NULL,
	.destroy = qubes_output_deinit_raw,
	.test = qubes_output_test,
	.commit = qubes_output_commit,
	.get_gamma_size = qubes_get_gamma_size,
	.get_cursor_formats = NULL,
	.get_cursor_size = NULL,
	.get_primary_formats = qubes_output_get_primary_formats,
};

static void qubes_output_frame(struct wl_listener *listener,
                               void *data __attribute__((unused)))
{
	struct qubes_output *output = wl_container_of(listener, output, frame);
	// HACK HACK
	//
	// This fixes a *nasty* bug: without it, really fast resizes can cause the
	// wlr_output to lose sync with the qubes_output, causing parts of the
	// window to *never* be displayed until the next window resize.  This bug
	// took more than three days to fix.
	if (output->last_width && output->last_height)
		wlr_output_update_custom_mode(&output->output, output->last_width,
		                              output->last_height, 60000);
	assert(QUBES_VIEW_MAGIC == output->magic ||
	       QUBES_XWAYLAND_MAGIC == output->magic);
	if (qubes_output_mapped(output) &&
	    !wlr_scene_output_commit(output->scene_output)) {
		return;
	}
	output->output.frame_pending = true;
	if (!output->server->frame_pending) {
		// Schedule another timer callback
		wl_event_source_timer_update(output->server->timer, 16);
		output->server->frame_pending = true;
	}
}

static void qubes_output_clear_surface(struct qubes_output *const output)
{
	wlr_log(WLR_DEBUG, "Surface clear for window %" PRIu32, output->window_id);
	if (output->scene_subsurface_tree)
		wlr_scene_node_destroy(&output->scene_subsurface_tree->node);
	output->scene_subsurface_tree = NULL;
	output->surface = NULL;
}

bool qubes_output_set_surface(struct qubes_output *const output,
                              struct wlr_surface *const surface)
{
	if (surface == output->surface)
		return true; /* nothing to do */

	qubes_output_clear_surface(output);
	if (!surface)
		return true;

	if (!(output->scene_subsurface_tree = wlr_scene_subsurface_tree_create(
	         &output->scene_output->scene->tree, surface)))
		return false;
	output->surface = surface;
	wlr_scene_node_raise_to_top(&output->scene_subsurface_tree->node);
	return true;
}

bool qubes_output_init(struct qubes_output *const output,
                       struct tinywl_server *const server,
                       bool const is_override_redirect,
                       struct wlr_surface *const surface, uint32_t const magic)
{
	assert(output);
	memset(output, 0, sizeof *output);

	assert(server);
	struct wlr_backend *const backend = &server->backend->backend;
	assert(magic == QUBES_VIEW_MAGIC || magic == QUBES_XWAYLAND_MAGIC);

	wlr_output_init(&output->output, backend, &qubes_wlr_output_impl,
	                server->wl_display);
	wlr_output_update_custom_mode(&output->output, 1280, 720, 0);
	wlr_output_update_enabled(&output->output, true);

	if (asprintf(&output->name, "Virtual Output %" PRIu64,
	             server->output_counter++) < 0) {
		output->name = NULL;
		return false;
	}
	wlr_output_set_name(&output->output, output->name);
	wlr_output_set_description(&output->output, "Qubes OS virtual output");

	output->buffer = NULL;
	output->buffer_destroy.notify = qubes_unlink_buffer_listener;
	output->formats = &global_formats;
	output->frame.notify = qubes_output_frame;
	output->magic = magic;
	output->flags = is_override_redirect ? QUBES_OUTPUT_OVERRIDE_REDIRECT : 0,
	output->server = server;
	wl_signal_add(&output->output.events.frame, &output->frame);

	wl_list_insert(&server->views, &output->link);
	assert(output->output.allocator == NULL);
	assert(server->allocator != NULL);
	/* Add wlr_output */
	wlr_output_init_render(&output->output, server->allocator, server->renderer);
	assert(output->output.allocator);
	if (!(output->scene = wlr_scene_create()))
		return false;
	if (!(output->scene_output =
	         wlr_scene_output_create(output->scene, &output->output)))
		return false;
	return qubes_output_set_surface(output, surface);
}

void qubes_send_configure(struct qubes_output *output, uint32_t width,
                          uint32_t height)
{
	if (!qubes_output_created(output))
		return;
	if (width <= 0 || height <= 0)
		return;
	// Refuse excessive sizes
	if (width > MAX_WINDOW_WIDTH)
		width = MAX_WINDOW_WIDTH;
	if (height > MAX_WINDOW_HEIGHT)
		height = MAX_WINDOW_HEIGHT;
	if (output->left < -MAX_WINDOW_WIDTH)
		output->left = -MAX_WINDOW_WIDTH;
	if (output->top < -MAX_WINDOW_HEIGHT)
		output->top = -MAX_WINDOW_HEIGHT;
	if (output->left > MAX_WINDOW_WIDTH)
		output->left = MAX_WINDOW_WIDTH;
	if (output->top > MAX_WINDOW_HEIGHT)
		output->top = MAX_WINDOW_HEIGHT;

	// clang-format off
	struct {
		struct msg_hdr header;
		struct msg_configure configure;
	} msg = {
		.header = {
			.type = MSG_CONFIGURE,
			.window = output->window_id,
			.untrusted_len = sizeof(struct msg_configure),
		},
		.configure = {
			.x = output->left,
			.y = output->top,
			.width = width,
			.height = height,
			.override_redirect = output->flags & QUBES_OUTPUT_OVERRIDE_REDIRECT ? 1 : 0,
		},
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.configure);
	// clang-format on
	wlr_log(WLR_DEBUG, "Sending MSG_CONFIGURE (0x%x) to window %" PRIu32,
	        MSG_CONFIGURE, output->window_id);
	qubes_rust_send_message(output->server->backend->rust_backend,
	                        (struct msg_hdr *)&msg);
}

void qubes_set_view_title(struct qubes_output *output, const char *const title)
{
	assert(qubes_output_created(output));
	assert(output->window_id);
	wlr_log(WLR_DEBUG, "Sending MSG_WMNAME (0x%x) to window %" PRIu32,
	        MSG_WMNAME, output->window_id);
	struct {
		struct msg_hdr header;
		struct msg_wmname title;
	} msg;
	msg.header = (struct msg_hdr){
		.type = MSG_WMNAME,
		.window = output->window_id,
		.untrusted_len = sizeof(struct msg_wmname),
	};
	strncpy(msg.title.data, title, sizeof msg.title.data - 1);
	msg.title.data[sizeof msg.title.data - 1] = 0;
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.title);
	// Asserted above, checked at call sites
	qubes_rust_send_message(output->server->backend->rust_backend,
	                        (struct msg_hdr *)&msg);
}

struct wlr_surface *qubes_output_surface(struct qubes_output *output)
{
	switch (output->magic) {
	case QUBES_VIEW_MAGIC: {
		struct tinywl_view *view = wl_container_of(output, view, output);
		return view->xdg_surface->surface;
	}
	case QUBES_XWAYLAND_MAGIC: {
		struct qubes_xwayland_view *view = wl_container_of(output, view, output);
		return view->xwayland_surface->surface;
	}
	default:
		assert(!"Bad magic in qubes_output_surface!");
		abort();
	}
}

void qubes_output_deinit(struct qubes_output *output)
{
	if (output->scene_subsurface_tree)
		wlr_scene_node_destroy(&output->scene_subsurface_tree->node);
	wl_list_remove(&output->link);
	assert(output->magic == QUBES_VIEW_MAGIC ||
	       output->magic == QUBES_XWAYLAND_MAGIC);
	struct msg_hdr header = {
		.type = MSG_DESTROY,
		.window = output->window_id,
		.untrusted_len = 0,
	};
	if (qubes_output_created(output)) {
		wlr_log(WLR_DEBUG, "Sending MSG_DESTROY (0x%x) to window %" PRIu32,
		        MSG_DESTROY, output->window_id);
		qubes_rust_send_message(output->server->backend->rust_backend, &header);
	}
	if (output->scene_output) {
		wlr_scene_output_destroy(output->scene_output);
	}
	if (output->scene) {
		wlr_scene_node_destroy(&output->scene->tree.node);
	}
	wlr_output_destroy(&output->output);
	free(output->name);
}

void qubes_change_window_flags(struct qubes_output *output, uint32_t flags_set,
                               uint32_t flags_unset)
{
	assert(qubes_output_created(output));
	// clang-format off
	struct {
		struct msg_hdr header;
		struct msg_window_flags flags;
	} msg = {
	   .header = {
			.type = MSG_WINDOW_FLAGS,
			.window = output->window_id,
			.untrusted_len = sizeof(struct msg_window_flags),
		},
		.flags = {
			.flags_set = flags_set,
			.flags_unset = flags_unset,
		},
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.flags);
	// clang-format on
	// Asserted above, checked at call sites
	wlr_log(WLR_DEBUG, "Sending MSG_WINDOW_FLAGS (0x%x) to window %" PRIu32,
	        MSG_DESTROY, output->window_id);
	qubes_rust_send_message(output->server->backend->rust_backend,
	                        (struct msg_hdr *)&msg);
}

void qubes_output_unmap(struct qubes_output *output)
{
	output->flags &= ~(__typeof__(output->flags))QUBES_OUTPUT_MAPPED;
	wlr_output_enable(&output->output, false);
	struct msg_hdr header = {
		.type = MSG_UNMAP,
		.window = output->window_id,
		.untrusted_len = 0,
	};
	if (qubes_output_created(output)) {
		wlr_log(WLR_DEBUG, "Sending MSG_UNMAP (0x%x) to window %" PRIu32,
		        MSG_UNMAP, output->window_id);
		qubes_rust_send_message(output->server->backend->rust_backend, &header);
	}
}

void qubes_output_map(struct qubes_output *output,
                      uint32_t transient_for_window, bool override_redirect)
{
	if (!qubes_output_mapped(output)) {
		output->flags |= QUBES_OUTPUT_MAPPED;
		wlr_scene_node_set_enabled(&output->scene_subsurface_tree->node, true);
		wlr_output_enable(&output->output, true);
	}

	// clang-format off
	struct {
		struct msg_hdr header;
		struct msg_map_info info;
	} msg = {
	   .header = {
			.type = MSG_MAP,
			.window = output->window_id,
			.untrusted_len = sizeof(struct msg_map_info),
		},
		.info = {
			.transient_for = transient_for_window,
			.override_redirect = override_redirect,
		},
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.info);
	// clang-format on
	// Surface created above
	wlr_log(WLR_DEBUG,
	        "Sending MSG_MAP (0x%x) to window %u (transient_for = %u)", MSG_MAP,
	        output->window_id, transient_for_window);
	qubes_rust_send_message(output->server->backend->rust_backend,
	                        (struct msg_hdr *)&msg);
}

void qubes_output_configure(struct qubes_output *output, struct wlr_box box)
{
	if (!box.width || !box.height)
		return;
	bool need_configure = output->magic == QUBES_XWAYLAND_MAGIC;
	qubes_output_ensure_created(output, box);
	if ((output->last_width != box.width || output->last_height != box.height) &&
	    (!(output->flags & QUBES_OUTPUT_IGNORE_CLIENT_RESIZE))) {
		wlr_log(WLR_DEBUG, "Resized window %u: old size %u %u, new size %u %u",
		        (unsigned)output->window_id, output->last_width,
		        output->last_height, box.width, box.height);
		wlr_output_set_custom_mode(&output->output, box.width, box.height, 60000);
		need_configure = true;
	}
	if (need_configure) {
		qubes_send_configure(output, box.width, box.height);
		output->last_width = box.width;
		output->last_height = box.height;
		output->x = box.x;
		output->y = box.y;
	}
	wlr_output_send_frame(&output->output);
}

void qubes_output_set_class(struct qubes_output *output, const char *class)
{
	assert(qubes_output_created(output));
	assert(output->window_id);
	wlr_log(WLR_DEBUG, "Sending MSG_WMCLASS (0x%x) to window %" PRIu32,
	        MSG_WMCLASS, output->window_id);
	// clang-format off
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
			.res_class = {0},
			.res_name = {0},
		},
	};
	// clang-format on
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.class);
	strncpy(msg.class.res_class, class, sizeof(msg.class.res_class) - 1);
	// Asserted above, checked at call sites
	qubes_rust_send_message(output->server->backend->rust_backend,
	                        (struct msg_hdr *)&msg);
}

/* vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8: */
