#ifndef QUBES_WAYLAND_COMPOSITOR_COMMON_H
#define QUBES_WAYLAND_COMPOSITOR_COMMON_H _Pragma("GCC error \"double-include guard referenced\"")
#ifdef NDEBUG
#error Cannot build with assertions disabled
#endif
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
static_assert(sizeof(short) == 2, "wrong sizeof(short)");
static_assert(sizeof(int) == 4, "wrong sizeof(int)");
static_assert(sizeof(long) == sizeof(void *), "wrong size of long");
static_assert(sizeof(long long) == 8, "wrong sizeof(long long)");
static_assert(sizeof(uintptr_t) == sizeof(size_t), "wrong size of size_t");
static_assert(CHAR_BIT == 8, "char is not 8 bits???");
static_assert(SCHAR_MAX == INT8_MAX, "wrong CHAR_MAX");
static_assert(SHRT_MAX == INT16_MAX, "wrong SHRT_MAX");
static_assert(INT_MAX == INT32_MAX, "wrong INT_MAX");
static_assert(LONG_MAX == INTPTR_MAX, "wrong UINTPTR_MAX");
static_assert(LLONG_MAX == INT64_MAX, "wrong LLONG_MAX");
static_assert(SCHAR_MIN == INT8_MIN, "wrong CHAR_MIN");
static_assert(SHRT_MIN == INT16_MIN, "wrong SHRT_MIN");
static_assert(INT_MIN == INT32_MIN, "wrong INT_MIN");
static_assert(LONG_MIN == INTPTR_MIN, "wrong UINTPTR_MIN");
static_assert(LLONG_MIN == INT64_MIN, "wrong LLONG_MIN");
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
