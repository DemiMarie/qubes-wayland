#ifndef QUBES_WAYLAND_COMPOSITOR_ALLOCATOR_H
#define QUBES_WAYLAND_COMPOSITOR_ALLOCATOR_H _Pragma("GCC error \"double-include guard referenced\"")

#include "common.h"
#include <wlr/render/allocator.h>
#ifdef BUILD_RUST
#include <qubes-gui-protocol.h>
#include <xen/gntalloc.h>
#endif

/**
 * Creates an allocator, owned by main()
 */
struct wlr_allocator *qubes_allocator_create(uint16_t domid);
extern const struct wlr_buffer_impl *qubes_buffer_impl_addr;

/**
 * Qubes OS buffer.  Owned by wlroots.
 */
struct qubes_buffer {
	struct wlr_buffer inner;
	void *ptr;
	struct qubes_allocator *alloc;
	uint64_t index;
	size_t size;
#ifdef BUILD_RUST
	union {
		struct {
			uint32_t format;
			struct msg_hdr header;
		};
		uint64_t dummy[2];
	};
	union {
		struct ioctl_gntalloc_alloc_gref xen; /* only used during initialization */
		struct msg_window_dump_hdr qubes;
	};
#else
	uint32_t format, width, height;
#endif
};
#ifdef BUILD_RUST
_Static_assert(offsetof(struct qubes_buffer, xen) - offsetof(struct qubes_buffer, header) == sizeof(struct msg_hdr), "Struct not contiguous?");
#endif

#endif
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
