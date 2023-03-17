#ifndef QUBES_WAYLAND_COMPOSITOR_ALLOCATOR_H
#define QUBES_WAYLAND_COMPOSITOR_ALLOCATOR_H                                   \
	_Pragma("GCC error \"double-include guard referenced\"")

#include "common.h"
#include <qubes-gui-protocol.h>
#include <wlr/render/allocator.h>
#include <xen/gntalloc.h>

/**
 * Creates an allocator, owned by main()
 */
struct wlr_allocator *qubes_allocator_create(uint16_t domid);
extern const struct wlr_buffer_impl *qubes_buffer_impl_addr;
void qubes_buffer_destroy(struct wlr_buffer *buffer);

/**
 * Qubes OS buffer.  Owned by wlroots.
 */
struct qubes_buffer {
	uint64_t refcount;
	struct wlr_buffer inner;
	void *ptr;
	struct qubes_allocator *alloc;
	uint64_t index;
	size_t size;
	union {
		struct {
			uint32_t format;
			struct msg_hdr header;
		};
		uint64_t dummy[2];
	};
	union {
		/* only used during initialization */
		struct ioctl_gntalloc_alloc_gref xen;
		struct msg_window_dump_hdr qubes;
	};
};
_Static_assert((offsetof(struct qubes_buffer, xen) -
                offsetof(struct qubes_buffer, header)) ==
                  sizeof(struct msg_hdr),
               "Struct not contiguous?");
void qubes_buffer_destroy(struct wlr_buffer *raw_buffer);

#endif
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
