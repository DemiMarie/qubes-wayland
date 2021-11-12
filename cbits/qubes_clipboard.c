#include "common.h"

#include <stdlib.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <wayland-server-core.h>

#include <wlr/util/log.h>

#include <qubes-gui-protocol.h>

#include "main.h"
#include "qubes_backend.h"


struct qubes_clipboard_handler {
	struct wl_array clipboard_data; /**< Store the incoming data; of type uint8_t * */
	struct wl_listener display_destroy; /**< Listener for display destruction */
	struct tinywl_server *server; /**< Pointer to the server */
	struct wl_event_source *source; /**< Event source */
	int fd; /**< File descriptor */
};

static void qubes_clipboard_handler_destroy(struct qubes_clipboard_handler *handler)
{
	wl_array_release(&handler->clipboard_data);
	wl_list_remove(&handler->display_destroy.link);
	wl_event_source_remove(handler->source);
	assert(close(handler->fd) == 0 && "Closing a pipe always succeeds");
	memset(handler, 0, sizeof(*handler));
	free(handler);
}

static void qubes_clipboard_handler_on_display_destroy(struct wl_listener *listener, void *dummy QUBES_UNUSED)
{
	struct qubes_clipboard_handler *handler = wl_container_of(listener, handler, display_destroy);
	qubes_clipboard_handler_destroy(handler);
}

enum {
	MAX_CLIPBOARD_MESSAGE_SIZE = MAX_CLIPBOARD_SIZE + sizeof(struct msg_hdr),
};

static int qubes_on_clipboard_data(int const fd, uint32_t const mask, void *data)
{
	struct qubes_clipboard_handler *const handler = data;
	struct wl_array *const clipboard_data = &handler->clipboard_data;
	void *ptr;

	assert(fd == handler->fd && "Wrong file descriptor");
	wlr_log(WLR_DEBUG, "Processing clipboard data from client");
	assert(clipboard_data->size <= clipboard_data->alloc && "corrupt wl_array");
retry:;
	size_t const size = clipboard_data->size;
	assert(clipboard_data->size <= (size_t)MAX_CLIPBOARD_MESSAGE_SIZE && "already made array too large?");
	if (clipboard_data->alloc <= size) {
		size_t size = clipboard_data->size;
		if (!(ptr = wl_array_add(clipboard_data, QUBES_MIN(size + 0xFF, (size_t)MAX_CLIPBOARD_MESSAGE_SIZE + 1))))
			goto done;
		assert(ptr == (char *)clipboard_data->data + size);
	} else {
		ptr = (char *)clipboard_data->data + size;
	}
	assert(clipboard_data->size <= clipboard_data->alloc && "corrupt wl_array");
	size_t const max_data = QUBES_MIN(clipboard_data->alloc, (size_t)MAX_CLIPBOARD_MESSAGE_SIZE + 1);
	size_t const to_read = max_data - size;
	ssize_t const res = read(fd, ptr, to_read);
	if (res == 0) {
		assert(size >= sizeof(struct msg_hdr));
		struct msg_hdr header = {
			.type = MSG_CLIPBOARD_DATA,
			.window = 0,
			.untrusted_len = size - sizeof header,
		};
		assert(clipboard_data->size >= sizeof header && "Data too small???");
		memcpy(clipboard_data->data, &header, sizeof header);
		qubes_rust_send_message(handler->server->backend->rust_backend, clipboard_data->data);
		goto done;
	} else if (res == -1) {
		int err = errno;
		switch (err) {
		case EAGAIN:
#if EAGAIN != EWOULDBLOCK
		case EWOULDBLOCK:
#endif
			return 0;
		case 0:
		case EBADF:
		case EFAULT:
			abort();
		default:
			wlr_log(WLR_ERROR, "Error reading from pipe");
			goto done;
		}
	} else {
		assert(res > 0 && (size_t)res <= to_read && "Bad return from read()!");
		assert(clipboard_data->alloc >= size && "uh oh!");
		assert(clipboard_data->alloc - size >= (size_t)res && "corrupted wl_array?");
		assert(max_data >= (size_t)res + size && "already read too much?");

		clipboard_data->size = (size_t)res + size;
		if (clipboard_data->size > MAX_CLIPBOARD_MESSAGE_SIZE) {
			wlr_log(WLR_ERROR, "Clipboard data size %zu is too large, sorry", clipboard_data->size);
			goto done;
		}
		assert(clipboard_data->size <= clipboard_data->alloc && "corrupt wl_array");
		goto retry;
	}
done:
	qubes_clipboard_handler_destroy(handler);
	return 0;
}

struct qubes_clipboard_handler *qubes_clipboard_handler_create(struct tinywl_server *server, int fd)
{
	struct qubes_clipboard_handler *handler = calloc(sizeof(*handler), 1);
	if (!handler)
		goto fail;
	wl_array_init(&handler->clipboard_data);
	struct msg_hdr header = {
		.type = MSG_CLIPBOARD_DATA,
		.window = 0,
		.untrusted_len = 0,
	};
	void *ptr = wl_array_add(&handler->clipboard_data, sizeof header);
	if (!ptr)
		goto fail;
	assert(handler->clipboard_data.alloc >= handler->clipboard_data.size && "corrupted wl_array?");
	memcpy(ptr, &header, sizeof header);
	handler->source = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display), fd,
		WL_EVENT_READABLE|WL_EVENT_HANGUP|WL_EVENT_ERROR, qubes_on_clipboard_data,
		handler);
	if (!handler->source)
		goto fail;
	handler->display_destroy.notify = qubes_clipboard_handler_on_display_destroy;
	handler->server = server;
	handler->fd = fd;
	wl_display_add_destroy_listener(server->wl_display, &handler->display_destroy);
	return handler;
fail:
	if (handler) {
		wl_array_release(&handler->clipboard_data);
		free(handler);
	}
	close(fd);
	return NULL;
}
