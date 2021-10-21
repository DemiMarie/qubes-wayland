#include "common.h"
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <wlr/util/log.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/allocator/interface.h>
#include <wlr/allocator/wlr_allocator.h>
#include <wlr/types/wlr_buffer.h>
#include <xen/gntalloc.h>
#include <qubes-gui-protocol.h>
#include <drm/drm_fourcc.h>

#include "qubes_allocator.h"

struct qubes_allocator;
static struct wlr_buffer *qubes_buffer_create(struct wlr_allocator *alloc,
		const int width, const int height, const struct wlr_drm_format *format);
static void qubes_allocator_destroy(struct wlr_allocator *allocator);
static void qubes_buffer_destroy(struct wlr_buffer *buffer);
static bool qubes_buffer_begin_data_ptr_access(struct wlr_buffer *buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride);
static void qubes_allocator_decref(struct qubes_allocator *allocator);

static const struct wlr_allocator_interface qubes_allocator_impl = {
	.create_buffer = qubes_buffer_create,
	.destroy = qubes_allocator_destroy,
};

static void qubes_buffer_end_data_ptr_access(struct wlr_buffer *raw_buffer __attribute__((unused))) {}

static const struct wlr_buffer_impl qubes_buffer_impl = {
	.destroy = qubes_buffer_destroy,
	.get_dmabuf = NULL,
	.get_shm = NULL,
	.begin_data_ptr_access = qubes_buffer_begin_data_ptr_access,
	.end_data_ptr_access = qubes_buffer_end_data_ptr_access,
};
const struct wlr_buffer_impl *qubes_buffer_impl_addr = &qubes_buffer_impl;

struct qubes_allocator {
	struct wlr_allocator inner;
	uint64_t refcount;
	int xenfd;
	uint16_t domid;
};

static void qubes_allocator_destroy(struct wlr_allocator *allocator) {
	struct qubes_allocator *qubes = wl_container_of(allocator, qubes, inner);
	assert(close(qubes->xenfd) == 0 && "Closing a gntalloc handle always succeeds");
	qubes->xenfd = -1;
	qubes_allocator_decref(qubes);
}

static void qubes_allocator_decref(struct qubes_allocator *allocator) {
	assert(allocator->refcount > 0 && "use after free???");
	allocator->refcount--;
	if (allocator->refcount == 0) {
		assert(allocator->xenfd == -1 && "Xen FD wasn’t closed by qubes_allocator_destroy?");
		free(allocator);
	}
}

struct wlr_allocator *qubes_allocator_create(uint16_t domid) {
	struct qubes_allocator *qubes = calloc(sizeof(*qubes), 1);
	if (!qubes)
		return NULL;
	qubes->domid = domid;
	if ((qubes->xenfd = open("/dev/xen/gntalloc", O_RDWR | O_CLOEXEC | O_NOCTTY)) < 0) {
		assert(qubes->xenfd == -1);
		free(qubes);
		return NULL;
	} else {
		assert(qubes->xenfd > 2 && "FD 0, 1, or 2 got closed earlier?");
		qubes->refcount = 1;
		wlr_allocator_init(&qubes->inner, &qubes_allocator_impl, WLR_BUFFER_CAP_DATA_PTR);
		return &qubes->inner;
	}
}
#ifndef XC_PAGE_SIZE
#define XC_PAGE_SIZE 4096
#endif

static struct wlr_buffer *qubes_buffer_create(struct wlr_allocator *alloc,
		const int width, const int height, const struct wlr_drm_format *format) {
	assert(alloc->impl == &qubes_allocator_impl);
	struct qubes_allocator *qalloc = wl_container_of(alloc, qalloc, inner);
	assert(qalloc->refcount > 0);

	/* DMA-BUFs aren’t supported */
	if (format->cap & ~(size_t)WLR_BUFFER_CAP_DATA_PTR)
		return NULL;
	/* Only ARGB8888 and XRGB8888 are supported */
	if (format->format != DRM_FORMAT_XRGB8888) {
		wlr_log(WLR_ERROR, "Refusing allocation because format %" PRIu32 " is not supported", format->format);
		return NULL;
	}
	/* Format modifiers aren’t supported */
	if (format->len) {
		wlr_log(WLR_ERROR, "Refusing allocation because of format modifiers");
		return NULL;
	}
	/* Check for excessive sizes */
	if (width <= 0 || width > MAX_WINDOW_WIDTH ||
	    height <= 0 || height > MAX_WINDOW_HEIGHT) {
		wlr_log(WLR_ERROR, "Refusing allocation because width %d and/or height %d is bad", width, height);
		return NULL;
	}
	wlr_log(WLR_DEBUG, "Allocating array of dimensions %dx%d", width, height);
	/* the remaining computations cannot overflow */
	const int32_t pixels = (int32_t)width * (int32_t)height;
	const int32_t bytes = pixels * sizeof(uint32_t);
	const int32_t pages = NUM_PAGES(bytes);

	struct qubes_buffer *buffer = malloc((size_t)pages * SIZEOF_GRANT_REF +
	                                     offsetof(struct qubes_buffer, qubes) +
													 sizeof(buffer->qubes));
	if (!buffer)
		return NULL;
	buffer->xen.domid = qalloc->domid;
	buffer->xen.flags = GNTALLOC_FLAG_WRITABLE;
	buffer->xen.count = pages;
	buffer->format = format->format;
	int res = ioctl(qalloc->xenfd, IOCTL_GNTALLOC_ALLOC_GREF, &buffer->xen);
	if (res) {
		assert(res == -1);
		goto fail;
	}
	buffer->index = buffer->xen.index;
	buffer->size = (size_t)bytes;
	buffer->qubes.type = 0; /* WINDOW_DUMP_TYPE_GRANT_REFS */
	buffer->qubes.width = (uint32_t)width;
	buffer->qubes.height = (uint32_t)height;
	buffer->qubes.bpp = 24;
	buffer->ptr = mmap(NULL, (size_t)bytes, PROT_READ|PROT_WRITE, MAP_SHARED, qalloc->xenfd, (off_t)buffer->index);
	if (buffer->ptr != MAP_FAILED) {
		wlr_buffer_init(&buffer->inner, &qubes_buffer_impl, width, height);
		qalloc->refcount++;
		assert(qalloc->refcount);
		buffer->alloc = qalloc;
		return &buffer->inner;
	}
fail:
	if (buffer->size) {
		struct ioctl_gntalloc_dealloc_gref dealloc = {
			.index = buffer->index,
			.count = pages,
		};
		assert(ioctl(qalloc->xenfd, IOCTL_GNTALLOC_DEALLOC_GREF, &dealloc) == 0);
	}
	free(buffer);
	return NULL;
}

static bool qubes_buffer_begin_data_ptr_access(struct wlr_buffer *raw_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	assert(raw_buffer->impl == &qubes_buffer_impl);
	if (flags & ~((uint32_t)WLR_BUFFER_DATA_PTR_ACCESS_READ | (uint32_t)WLR_BUFFER_DATA_PTR_ACCESS_WRITE))
		return false;
	struct qubes_buffer *buffer = wl_container_of(raw_buffer, buffer, inner);
	if (stride)
		*stride = buffer->qubes.width * sizeof(uint32_t);
	if (data)
		*data = buffer->ptr;
	if (format)
		*format = buffer->format;
	return true;
}

static void qubes_buffer_destroy(struct wlr_buffer *raw_buffer) {
	assert(raw_buffer->impl == &qubes_buffer_impl);
	struct qubes_buffer *buffer = wl_container_of(raw_buffer, buffer, inner);
	struct ioctl_gntalloc_dealloc_gref dealloc = {
		.index = buffer->index,
		.count = NUM_PAGES(buffer->size),
	};
	assert(munmap(buffer->ptr, buffer->size) == 0);
	if (buffer->alloc->xenfd != -1)
		assert(ioctl(buffer->alloc->xenfd, IOCTL_GNTALLOC_DEALLOC_GREF, &dealloc) == 0);
	qubes_allocator_decref(buffer->alloc);
	free(buffer);
}

// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
