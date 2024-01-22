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
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "main.h"
#include "qubes_allocator.h"
#include "qubes_backend.h"
#include "qubes_output.h"
#include "qubes_wayland.h"
#include "qubes_xwayland.h"
#include <drm_fourcc.h>

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

static void qubes_output_damage(struct qubes_output *output,
                                const struct wlr_output_state *state)
{
	pixman_box32_t fake_rect = {
		.x1 = 0, .y1 = 0, .x2 = output->guest.width, .y2 = output->guest.height
	};
	pixman_box32_t *rects;
	int n_rects;
	if (state == NULL || (output->flags & QUBES_OUTPUT_DAMAGE_ALL) ||
		 (state->committed & WLR_OUTPUT_STATE_MODE) ||
		 (output->magic != QUBES_VIEW_MAGIC)) {
		wlr_log(WLR_DEBUG, "Damaging everything");
		n_rects = 1;
		rects = &fake_rect;
		output->flags &= ~QUBES_OUTPUT_DAMAGE_ALL;
	} else if (!(state->committed & WLR_OUTPUT_STATE_DAMAGE)) {
		return;
	} else {
		n_rects = 0;
		rects = pixman_region32_rectangles((pixman_region32_t *)&state->damage,
		                                   &n_rects);
		if (n_rects <= 0 || !rects) {
			wlr_log(WLR_DEBUG, "No damage!");
			return;
		}
	}
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

void qubes_output_dump_buffer(struct qubes_output *output,
                              const struct wlr_output_state *state)
{
	assert(output->buffer->impl == qubes_buffer_impl_addr);
	struct tinywl_server *server = output->server;
	struct qubes_buffer *buffer = wl_container_of(output->buffer, buffer, inner);
	if (server->backend->protocol_version >= 0x10007) {
		struct qubes_link *link = malloc(sizeof(*link));
		if (link == NULL) {
			wlr_log(WLR_ERROR, "Cannot allocate %zu bytes?", sizeof(*link));
			abort(); /* FIXME */
		}
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
	qubes_output_damage(output, state);
}

bool qubes_output_ensure_created(struct qubes_output *output)
{
	// implemented in Rust
	extern uint32_t qubes_rust_generate_id(void *backend, void *data)
	   __attribute__((warn_unused_result));
	if (((output->guest.width < 1) ||
	     (output->guest.width > MAX_WINDOW_WIDTH)) ||
	    ((output->guest.height < 1) ||
	     (output->guest.height > MAX_WINDOW_HEIGHT))) {
		return false;
	}
	if (qubes_output_created(output))
		return true;
	if (!output->window_id) {
		output->window_id =
		   qubes_rust_generate_id(output->server->backend->rust_backend, output);
	}
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
			.x = output->host.x = output->guest.x,
			.y = output->host.y = output->guest.y,
			.width = output->host.width = output->guest.width,
			.height = output->host.height = output->guest.height,
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
	if (!qubes_output_ensure_created(output))
		return false;

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		assert(state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
		assert(state->custom_mode.width > 0);
		assert(state->custom_mode.height > 0);
		if ((uint32_t)state->custom_mode.width != output->guest.width)
			wlr_log(WLR_ERROR, "BUG: size mismatch: %d vs %" PRIu32,
			        state->custom_mode.width, output->guest.width);
		if ((uint32_t)state->custom_mode.height != output->guest.height)
			wlr_log(WLR_ERROR, "BUG: size mismatch: %d vs %" PRIu32,
			        state->custom_mode.height, output->guest.height);
		wlr_output_set_custom_mode(raw_output, output->guest.width,
		                           output->guest.height,
		                           state->custom_mode.refresh);
		qubes_send_configure(output);
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
			qubes_output_dump_buffer(output, state);
		}
	}
	return true;
}

static const uint64_t modifiers[2] = { DRM_FORMAT_MOD_INVALID,
	                                    DRM_FORMAT_MOD_LINEAR };

static const struct wlr_drm_format global_pointer_array[2] = {
	{
	   .format = DRM_FORMAT_ARGB8888,
	   .len = 2,
	   .capacity = 0,
	   .modifiers = (uint64_t *)modifiers,
	},
	{
	   .format = DRM_FORMAT_XRGB8888,
	   .len = 2,
	   .capacity = 0,
	   .modifiers = (uint64_t *)modifiers,
	},
};

static const struct wlr_drm_format_set global_formats = {
	.len = 2,
	.capacity = 0,
	.formats = (struct wlr_drm_format *)global_pointer_array,
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

static bool qubes_wlr_scene_output_commit(
   struct wlr_scene_output *scene_output,
   uint32_t width, uint32_t height, uint32_t fps)
{
	if (!scene_output->output->needs_frame &&
	    !pixman_region32_not_empty(&scene_output->damage_ring.current)) {
		return true;
	}

	bool ok = false;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, width, height, fps);
	if (!wlr_scene_output_build_state(scene_output, &state, NULL)) {
		goto out;
	}

	ok = wlr_output_commit_state(scene_output->output, &state);
	if (!ok) {
		goto out;
	}

	wlr_damage_ring_rotate(&scene_output->damage_ring);

out:
	wlr_output_state_finish(&state);
	return ok;
}

static void qubes_output_frame(struct wl_listener *listener,
                               void *data __attribute__((unused)))
{
	struct qubes_output *output = wl_container_of(listener, output, frame);
	assert(QUBES_VIEW_MAGIC == output->magic ||
	       QUBES_XWAYLAND_MAGIC == output->magic);
	if (qubes_output_mapped(output)) {
		if (!qubes_wlr_scene_output_commit(output->scene_output, output->guest.width, output->guest.height, 60000))
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
                       struct wlr_surface *const surface, uint32_t const magic,
                       int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	assert(output);
	memset(output, 0, sizeof *output);

	assert(server);
	struct wlr_backend *const backend = &server->backend->backend;
	assert(magic == QUBES_VIEW_MAGIC || magic == QUBES_XWAYLAND_MAGIC);

	{
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_state_set_custom_mode(&state, 1280, 720, 60000);
		wlr_output_init(&output->output, backend, &qubes_wlr_output_impl,
		                server->wl_display, &state);
		wlr_output_state_finish(&state);
	}

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
	if (!(output->scene_layout = wlr_scene_attach_output_layout(
	         output->scene, server->output_layout)))
		return false;
	if (!qubes_output_set_surface(output, surface))
		return false;
	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	struct wlr_output_layout_output *l_output =
	   wlr_output_layout_add_auto(server->output_layout, &output->output);
	if (l_output == NULL)
		return false;
	wlr_scene_output_layout_add_output(output->scene_layout, l_output,
	                                   output->scene_output);
	return true;
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
	{
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, false);
		wlr_output_commit_state(&output->output, &state);
		wlr_output_state_finish(&state);
	}
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
		struct wlr_output_state state;

		output->flags |= QUBES_OUTPUT_MAPPED;
		wlr_scene_node_set_enabled(&output->scene_subsurface_tree->node, true);
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_commit_state(&output->output, &state);
		wlr_output_state_finish(&state);
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

bool qubes_output_configure(struct qubes_output *output, struct wlr_box box)
{
	if ((box.width > 0) && (box.height > 0)) {
		output->guest.x = box.x;
		output->guest.y = box.y;
		output->guest.width = (unsigned)box.width;
		output->guest.height = (unsigned)box.height;
		if (!qubes_output_ensure_created(output))
			return false;
		qubes_send_configure(output);
		wlr_output_send_frame(&output->output);
		return true;
	}
	return false;
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
