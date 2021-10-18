#ifndef QUBES_WAYLAND_COMPOSITOR_ALLOCATOR_H
#define QUBES_WAYLAND_COMPOSITOR_ALLOCATOR_H _Pragma("GCC error \"double-include guard referenced\"")

#include "common.h"
#include <wlr/allocator/wlr_allocator.h>

struct wlr_allocator *qubes_allocator_create(uint16_t domid);

#endif
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
