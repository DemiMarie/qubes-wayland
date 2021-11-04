#ifndef QUBES_WAYLAND_COMPOSITOR_QUBES_CLIPBOARD_H
#define QUBES_WAYLAND_COMPOSITOR_QUBES_CLIPBOARD_H _Pragma("GCC error \"double-include guard referenced\"")
#include "common.h"

#include "main.h"

struct qubes_clipboard_handler *qubes_clipboard_handler_create(struct tinywl_server *server, int fd);
#endif
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
