// #define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE 1
#define _DEFAULT_SOURCE 1
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#error bug
#endif
#include "common.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <wlr/util/log.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/allocator/interface.h>
#include <wlr/allocator/wlr_allocator.h>
#include <wlr/types/wlr_buffer.h>
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
	qubes_allocator_decref(qubes);
}

static void qubes_allocator_decref(struct qubes_allocator *allocator) {
	assert(allocator->refcount > 0 && "use after free???");
	allocator->refcount--;
	if (allocator->refcount == 0) {
		free(allocator);
	}
}

struct wlr_allocator *qubes_allocator_create(uint16_t domid __attribute__((unused))) {
	struct qubes_allocator *qubes = calloc(sizeof(*qubes), 1);
	if (!qubes)
		return NULL;
	qubes->refcount = 1;
	wlr_allocator_init(&qubes->inner, &qubes_allocator_impl, WLR_BUFFER_CAP_DATA_PTR);
	return &qubes->inner;
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
	if (format->format != DRM_FORMAT_XRGB8888 &&
	    format->format != DRM_FORMAT_ARGB8888) {
		wlr_log(WLR_ERROR, "Refusing allocation because format %" PRIu32 " is not supported", format->format);
		return NULL;
	}
	/* Format modifiers aren’t supported */
	if (format->len) {
		wlr_log(WLR_ERROR, "Refusing allocation because of format modifiers");
		return NULL;
	}
	/* Check for excessive sizes */
	if (width <= 0 || width > (1 << 14) ||
	    height <= 0 || height > (2048 * 3)) {
		wlr_log(WLR_ERROR, "Refusing allocation because width %d and/or height %d is bad", width, height);
		return NULL;
	}
	wlr_log(WLR_DEBUG, "Allocating array of dimensions %dx%d", width, height);
	/* the remaining computations cannot overflow */
	const int32_t pixels = (int32_t)width * (int32_t)height;
	const int32_t bytes = pixels * sizeof(uint32_t);

	struct qubes_buffer *buffer = calloc(sizeof(*buffer), 1);
	if (!buffer)
		return NULL;
	buffer->size = (size_t)bytes;
	buffer->width = (uint32_t)width;
	buffer->height = (uint32_t)height;
	buffer->format = format->format;
	buffer->ptr = mmap(NULL, (size_t)bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (buffer->ptr != MAP_FAILED) {
		wlr_buffer_init(&buffer->inner, &qubes_buffer_impl, width, height);
		qalloc->refcount++;
		assert(qalloc->refcount);
		buffer->alloc = qalloc;
		return &buffer->inner;
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
		*stride = buffer->width * sizeof(uint32_t);
	if (data)
		*data = buffer->ptr;
	if (format)
		*format = buffer->format;
	return true;
}

static void qubes_buffer_destroy(struct wlr_buffer *raw_buffer) {
	assert(raw_buffer->impl == &qubes_buffer_impl);
	struct qubes_buffer *buffer = wl_container_of(raw_buffer, buffer, inner);
	assert(munmap(buffer->ptr, buffer->size) == 0);
	qubes_allocator_decref(buffer->alloc);
	free(buffer);
}

// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
