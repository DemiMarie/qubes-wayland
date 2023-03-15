// Wayland data source for data from global clipboard

#include "common.h"

#include <stdlib.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include <wlr/types/wlr_data_device.h>
#include <wlr/util/log.h>

#include <qubes-gui-protocol.h>

#include "main.h"
#include "qubes_data_source.h"

struct qubes_data_source {
	struct wlr_data_source inner;
	struct qubes_clipboard_data *data;
	struct wl_display *display;
};

static const struct wlr_data_source_impl qubes_data_source_impl;

struct qubes_clipboard_data {
	uint64_t refcount; /**< Reference count, to prevent use-after-free */
	uint32_t size;     /**< Size of this data */
	uint8_t data[];    /**< The actual data */
};

struct qubes_clipboard_writer {
	struct wl_listener
	   display_destroy;             /**< Free this when the display goes away */
	struct wl_event_source *source; /**< Event source */
	struct qubes_clipboard_data *data; /**< Pointer to the actual data */
	uint32_t bytes_remaining;          /**< Bytes remaining to write */
	int fd;                            /**< File descriptor */
};

static struct qubes_clipboard_data *
qubes_clipboard_data_retain(struct qubes_clipboard_data *data)
{
	assert(data);
	assert(data->refcount > 0);
	data->refcount++;
	return data;
}

static void qubes_clipboard_data_release(struct qubes_clipboard_data *data)
{
	if (!data)
		return;
	assert(data->refcount > 0);
	if (data->refcount == 1)
		free(data);
	else
		data->refcount--;
}

static void
qubes_clipboard_writer_destroy(struct qubes_clipboard_writer *source)
{
	assert(source && source->source && source->data);
	wl_list_remove(&source->display_destroy.link);
	wl_event_source_remove(source->source);
	qubes_clipboard_data_release(source->data);
	close(source->fd);
	free(source);
}

static void
qubes_clipboard_writer_on_display_destroy(struct wl_listener *listener,
                                          void *dummy QUBES_UNUSED)
{
	struct qubes_clipboard_writer *source =
	   wl_container_of(listener, source, display_destroy);
	qubes_clipboard_writer_destroy(source);
}

static int qubes_data_writer_write_data(int const fd, uint32_t const mask,
                                        void *raw_handler)
{
	struct qubes_clipboard_writer *const handler = raw_handler;
	assert(handler);
	assert(handler->source);
	assert(handler->data);
	assert(fd == handler->fd);
	struct qubes_clipboard_data *data = handler->data;
	assert(data->refcount > 0 && "Use after free!");
	wlr_log(WLR_DEBUG, "Sending clipboard data to client");
retry:
	assert(handler->bytes_remaining <= data->size && "Wrote too many bytes!");
	ssize_t res = write(fd, data->data + (data->size - handler->bytes_remaining),
	                    handler->bytes_remaining);
	if (res == -1) {
		switch (errno) {
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
			wlr_log(WLR_ERROR, "Error writing to pipe");
		}
	} else {
		assert(res > 0 && (size_t)res <= (size_t)handler->bytes_remaining &&
		       "Bad return from write()!");
		handler->bytes_remaining -= (uint32_t)res;
		if (handler->bytes_remaining)
			goto retry;
	}
	qubes_clipboard_writer_destroy(handler);
	return 0;
}

static struct qubes_data_source *
qubes_data_source_from_wlr_data_source(struct wlr_data_source *source)
{
	assert(source);
	assert(source->impl == &qubes_data_source_impl);
	struct qubes_data_source *dummy;
	return wl_container_of(source, dummy, inner);
}

static void qubes_data_source_send(struct wlr_data_source *raw_source,
                                   const char *mime_type, int32_t fd)
{
	wlr_log(WLR_DEBUG,
	        "Sending global clipboard to client.  Selected MIME type is %s",
	        mime_type);
	struct qubes_data_source *source =
	   qubes_data_source_from_wlr_data_source(raw_source);
	struct qubes_clipboard_writer *writer = calloc(sizeof(*writer), 1);
	if (!writer)
		goto fail;
	writer->source =
	   wl_event_loop_add_fd(wl_display_get_event_loop(source->display), fd,
	                        WL_EVENT_WRITABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
	                        qubes_data_writer_write_data, writer);
	if (!writer->source)
		goto fail;
	writer->bytes_remaining = source->data->size;
	writer->data = qubes_clipboard_data_retain(source->data);
	writer->display_destroy.notify = qubes_clipboard_writer_on_display_destroy;
	writer->fd = fd;
	wl_display_add_destroy_listener(source->display, &writer->display_destroy);
	qubes_data_writer_write_data(fd, WL_EVENT_WRITABLE, writer);
	return;
fail:
	close(fd);
	free(writer);
}

static void qubes_data_source_destroy(struct wlr_data_source *raw_source)
{
	wlr_log(WLR_DEBUG, "Destroying global clipboard sender");
	struct qubes_data_source *source =
	   qubes_data_source_from_wlr_data_source(raw_source);
	qubes_clipboard_data_release(source->data);
	free(source);
}

static const struct wlr_data_source_impl qubes_data_source_impl = {
	.send = qubes_data_source_send,
	.destroy = qubes_data_source_destroy,
};

struct qubes_data_source *qubes_data_source_create(struct wl_display *display,
                                                   uint32_t len,
                                                   const uint8_t *ptr)
{
	struct qubes_data_source *source;
	struct qubes_clipboard_data *data;
	char *mime_types[] = {
		strdup("UTF8_STRING"),
		strdup("COMPOUND_TEXT"),
		strdup("TEXT"),
		strdup("STRING"),
		strdup("text/plain;charset=utf-8"),
		strdup("text/plain"),
	};
	for (size_t i = 0; i < sizeof(mime_types) / sizeof(mime_types[0]); ++i)
		if (!mime_types[i])
			goto destroy_mime;
	if (!(source = calloc(sizeof(*source), 1)))
		goto destroy_mime;
	if (!(data = malloc(offsetof(__typeof__(*data), data) + len)))
		goto free_source;
	data->refcount = 1;
	data->size = len;
	memcpy(data->data, (char *)ptr, (size_t)len);
	source->data = data;

	wlr_data_source_init(&source->inner, &qubes_data_source_impl);
	source->display = display;
	char **mime_ptr =
	   wl_array_add(&source->inner.mime_types, sizeof(mime_types));
	if (!mime_ptr) {
		wlr_data_source_destroy(&source->inner);
		goto destroy_mime;
	}
	memcpy(mime_ptr, mime_types, sizeof(mime_types));
	wlr_log(WLR_DEBUG, "Creating data source for %" PRIu32 " bytes of data",
	        len);
	return source;
free_source:
	free(source);
destroy_mime:
	for (size_t i = 0; i < sizeof(mime_types) / sizeof(mime_types[0]); ++i)
		free(mime_types[i]);
	return NULL;
}
