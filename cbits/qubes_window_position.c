// Window positioning routines

// Host-initiated resizing will work as follows:
//
// 1. If the new size is not valid, acknowledge it and return.
//
// 2. If this is an Xwayland window, send a configure event back to the GUI
//    daemon and return.
//
// 3. If the "ignore configure events" flag is set, return.
//
// 4. Record what has changed, so that subsequent ack_configure requests
//    can be handled appropriately.
//
// 5. If the size has _not_ changed (the whole window was dragged),
//    record the new X and Y coordinates, but do _not_ inform the client
//    _unless_ this is a popup surface.  *Do* move all child popups,
//    though.
//
// 6. Send a configure event to the client.  Tell the client that it
//    is being resized.  Indicate which edges are currently being resized.
//
// 7. Wait (asynchronously) for the client to acknowledge the configure event.
//
// 8. Wait (asynchronously) for the client to commit.
//
// 9. Compare the new size to the size chosen by the GUI daemon.  If the size
//    is the same, just ack the GUI daemon's resize message.  If the size is
//    _not_ the same, but the GUI daemon's resize message preserved the position
//    of a specific edge, keep the position of that edge unchanged.  Otherwise,
//    do something else (FIXME).
//
// 10. Clear the "ignore configure events" flag.
//
// 11. Send a new configure request to the GUI daemon.
//
// 12. Commit a new surface size.
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
#include "qubes_wayland.h"
#include "qubes_xwayland.h"
#include <drm_fourcc.h>

static void qubes_send_configure_msg(struct qubes_output *output,
                                     const struct msg_configure *configure)
{
	// clang-format off
	struct {
		struct msg_hdr header;
		struct msg_configure configure;
	} msg = {
		.header = {
			.type = MSG_CONFIGURE,
			.window = output->window_id,
			.untrusted_len = sizeof(msg.configure),
		},
		.configure = *configure,
	};
	QUBES_STATIC_ASSERT(sizeof msg == sizeof msg.header + sizeof msg.configure);
	// clang-format on
	qubes_window_log(output, WLR_DEBUG,
	                 "Sending MSG_CONFIGURE (0x%x): width %" PRIu32
	                 " height %" PRIu32 "x %" PRIi32 " y %" PRIi32,
	                 MSG_CONFIGURE, configure->width, configure->height,
	                 (int32_t)configure->x, (int32_t)configure->y);
	qubes_rust_send_message(output->server->backend->rust_backend,
	                        (struct msg_hdr *)&msg);
}

static void qubes_send_configure_raw(struct qubes_output *output)
{
	struct msg_configure configure = {
		.x = output->guest.x,
		.y = output->guest.y,
		.width = output->guest.width,
		.height = output->guest.height,
		.override_redirect =
		   ((output->flags & QUBES_OUTPUT_OVERRIDE_REDIRECT) ? 1 : 0),
	};
	qubes_send_configure_msg(output, &configure);
}

void qubes_send_configure(struct qubes_output *output)
{
	if (output->flags & QUBES_CHANGED_MASK) {
		qubes_send_configure_raw(output);
		output->host = output->guest;
		output->flags &= ~(__typeof__(output->flags))QUBES_CHANGED_MASK;
	}
	qubes_send_configure_raw(output);
}

void qubes_handle_configure(struct qubes_output *output, uint32_t timestamp,
                            struct msg_configure *configure)
{
	uint32_t const width = configure->width;
	uint32_t const height = configure->height;
	// Old GUI protocol headers incorrectly used uint32_t for x and y, so cast.
	int32_t const x = (int32_t)configure->x;
	int32_t const y = (int32_t)configure->y;

	// Resizing step 1: Validate the coordinates.
	if (((width < 1) || (width > MAX_WINDOW_WIDTH)) ||
	    ((height < 1) || (height > MAX_WINDOW_HEIGHT)) ||
	    ((x < -MAX_WINDOW_WIDTH) || (x > MAX_WINDOW_WIDTH)) ||
	    ((y < -MAX_WINDOW_HEIGHT) || (y > MAX_WINDOW_HEIGHT))) {
		// Invalid coordinates: bail out.
		qubes_window_log(output, WLR_ERROR,
		                 "Bad configure from GUI daemon: x %" PRIi32 " y %" PRIi32
		                 " width %" PRIu32 " height %" PRIu32 " window %" PRIu32,
		                 x, y, width, height, output->window_id);
		configure->override_redirect =
		   ((output->flags & QUBES_OUTPUT_OVERRIDE_REDIRECT) ? 1 : 0),
		qubes_send_configure_msg(output, configure);
		return;
	}

	output->host.width = width;
	output->host.height = height;
	output->host.x = x;
	output->host.y = y;

	// Step 2: Check for Xwayland window.
	if (QUBES_XWAYLAND_MAGIC == output->magic) {
		// Configure window and then return.
		struct qubes_xwayland_view *view = wl_container_of(output, view, output);
		wlr_xwayland_surface_configure(view->xwayland_surface, x, y, width,
		                               height);
		output->guest.x = x;
		output->guest.y = y;
		output->guest.width = width;
		output->guest.height = height;
		// There won’t be a configure event ACKd by the client, so
		// ACK early.  Neglecting this for Xwayland cost two weeks of debugging.
		qubes_send_configure_raw(output);
		return;
	}

	// Step 4: Check what has changed.
	output->flags &= ~QUBES_CHANGED_MASK;

	if (x != output->guest.x) {
		qubes_window_log(output, WLR_DEBUG,
		                 "Left position changed: was %" PRIi32 ", now %" PRIi32,
		                 output->guest.x, x);
		output->flags |= QUBES_OUTPUT_LEFT_CHANGED;
	}

	if (y != output->guest.y) {
		qubes_window_log(output, WLR_DEBUG,
		                 "Top position changed: was %" PRIi32 ", now %" PRIi32,
		                 output->guest.y, y);
		output->flags |= QUBES_OUTPUT_TOP_CHANGED;
	}

	if (x + width != output->guest.x + output->guest.width) {
		qubes_window_log(output, WLR_DEBUG,
		                 "Right position changed: %" PRIi32 ", now %" PRIi32,
		                 (int32_t)(output->guest.x + output->guest.width),
		                 (int32_t)(x + width));
		output->flags |= QUBES_OUTPUT_RIGHT_CHANGED;
	}

	if (y + height != output->guest.y + output->guest.height) {
		qubes_window_log(output, WLR_DEBUG,
		                 "Bottom position changed: %" PRIi32 ", now %" PRIi32,
		                 (int32_t)(output->guest.y + output->guest.height),
		                 (int32_t)(y + height));
		output->flags |= QUBES_OUTPUT_BOTTOM_CHANGED;
	}

	if (width != output->guest.width) {
		qubes_window_log(output, WLR_DEBUG,
		                 "Width changed: was %" PRIu32 ", now %" PRIu32,
		                 output->guest.width, width);
		output->flags |= QUBES_OUTPUT_WIDTH_CHANGED;
	}

	if (height != output->guest.height) {
		qubes_window_log(output, WLR_DEBUG,
		                 "Height changed: was %" PRIu32 ", now %" PRIu32,
		                 output->guest.height, height);
		output->flags |= QUBES_OUTPUT_HEIGHT_CHANGED;
	}

	if ((output->flags & QUBES_CHANGED_MASK) == 0) {
		// Just ACK without doing anything.  If this is stale the daemon
		// will resend new information.
		qubes_send_configure_raw(output);
		return;
	}
	output->guest.x = x;
	output->guest.y = y;

	// Step 4: Record what has changed.
	output->flags |= QUBES_OUTPUT_NEED_CONFIGURE;
	if (!qubes_output_resized(output)) {
		// Step 5: Do _not_ inform client.
		// FIXME: move popups.
		qubes_window_log(output, WLR_DEBUG, "NOT resized");
		assert(output->host.width == output->guest.width);
		assert(output->host.height == output->guest.height);
		qubes_send_configure_raw(output);
		return;
	}

	// Step 6:
	// Ignore client-initiated resizes until this configure is ACKd, to
	// avoid racing against the GUI daemon.
	output->flags |= QUBES_OUTPUT_IGNORE_CLIENT_RESIZE | QUBES_OUTPUT_DAMAGE_ALL;
	wlr_output_update_custom_mode(&output->output, width, height, 60000);
	wlr_output_schedule_frame(&output->output);
	struct tinywl_view *view = wl_container_of(output, view, output);
	if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		// Step 7: Wait for client to acknowledge configure event.
		output->flags |= QUBES_OUTPUT_NEED_CONFIGURE_ACK;
		view->configure_serial =
		   wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, width, height);
		qubes_window_log(
		   output, WLR_DEBUG,
		   "Will ACK configure from GUI daemon (width %u, height %u)"
		   " when client ACKs configure with serial %u",
		   width, height, view->configure_serial);
	} else {
		// There won’t be a configure event ACKd by the client, so
		// ACK on next commit.
		qubes_window_log(
		   output, WLR_DEBUG,
		   "Got a configure event for non-toplevel window; returning early");
		output->guest = output->host;
	}
}

static bool qubes_box_valid(struct wlr_box *box)
{
	return ((box->width > 0 && box->width <= MAX_WINDOW_WIDTH) &&
	        (box->height > 0 && box->height <= MAX_WINDOW_HEIGHT));
}

bool qubes_output_commit_size(struct qubes_output *output, struct wlr_box box)
{
	if (!qubes_box_valid(&box)) {
		qubes_window_log(output, WLR_ERROR, "Refusing to commit invalid size");
		return false; /* refuse to commit invalid size */
	}

	if ((output->flags & QUBES_OUTPUT_NEED_CONFIGURE_ACK) != 0 &&
	    ((uint32_t)box.width != output->host.width ||
	     (uint32_t)box.height != output->host.height)) {
		qubes_window_log(output, WLR_DEBUG,
		                 "Ignoring client surface commit because configure has "
		                 "not been acknowledged");
		return false; // Outstanding configure event
	}
	output->flags &= ~QUBES_OUTPUT_NEED_CONFIGURE_ACK;

	wlr_scene_output_set_position(output->scene_output, box.x, box.y);
	if (!qubes_output_ensure_created(output))
		return false;

	// Only honor size changes if there is no configure from the daemon
	// that must be ACKd, or if the
	if (((unsigned)box.width != output->guest.width) ||
	    ((unsigned)box.height != output->guest.height)) {
		// ACK the original configure
		// Honor the client's resize request.
		//
		// Set the surface mode
		output->guest.width = (unsigned)box.width;
		output->guest.height = (unsigned)box.height;
		wlr_output_update_custom_mode(&output->output, output->guest.width,
		                              output->guest.height, 60000);
		wlr_output_schedule_frame(&output->output);
	}
	// Send the new configure
	qubes_send_configure(output);
	return true;
}
