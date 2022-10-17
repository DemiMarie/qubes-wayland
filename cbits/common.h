#ifndef QUBES_WAYLAND_COMPOSITOR_COMMON_H
#define QUBES_WAYLAND_COMPOSITOR_COMMON_H QUBES_WAYLAND_COMPOSITOR_COMMON_H
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#define QUBES_UNUSED __attribute__((unused))

#define QUBES_MAGIC(a, b, c, d) \
	((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

#define QUBES_MIN(a, b) __extension__({ \
	__typeof__(a) _x = (a); \
	__typeof__(b) _y = (b); \
	_x > _y ? _y : _x; \
})

#define QUBES_MAX(a, b) __extension__({ \
	__typeof__(a) _x = (a); \
	__typeof__(b) _y = (b); \
	_x < _y ? _y : _x; \
})

#define QUBES_STATIC_ASSERT(a) _Static_assert(a, #a)

enum {
	QUBES_VIEW_MAGIC = QUBES_MAGIC('v', 'i', 'e', 'w'),
	QUBES_KEYBOARD_MAGIC = QUBES_MAGIC('k', 'e', 'y', 'b'),
	QUBES_SERVER_MAGIC = QUBES_MAGIC('s', 'e', 'r', 'v'),
};

#undef QUBES_MAGIC
#endif /* !defined QUBES_WAYLAND_COMPOSITOR_COMMON_H */
