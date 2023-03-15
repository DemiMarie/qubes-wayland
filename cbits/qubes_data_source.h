#ifndef QUBES_WAYLAND_COMPOSITOR_DATA_SOURCE_H
#define QUBES_WAYLAND_COMPOSITOR_DATA_SOURCE_H                                 \
	_Pragma("GCC error \"double-include guard used\"")
#include "common.h"

struct qubes_data_source *qubes_data_source_create(struct wl_display *display,
                                                   uint32_t len,
                                                   const uint8_t *ptr);
#endif
