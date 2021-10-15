#include "common.h"

#include <string.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/render/interface.h>

struct qubes_shell_listener {
   struct wl_listener new_surface;
   struct wl_listener destroy;
   struct wl_display *display;
   struct wlr_compositor *compositor;
   struct wlr_seat *seat;
   struct wlr_data_device_manager *data_device_manager;
   struct wlr_xdg_shell *shell;
   struct wlr_server_decoration_manager *server_decoration_manager;
   struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager_v1;
   struct wlr_xdg_activation_v1 *xdg_activation_v1;
};

struct qubes_surface {
   struct wl_listener commit;
   struct wl_listener new_subsurface;
   struct wl_listener destroy;
   struct wlr_surface *surface;
};

struct qubes_subsurface {
   struct wl_listener destroy;
   struct wl_listener map;
   struct wl_listener unmap;
   struct wlr_subsurface *subsurface;
};

static void qubes_subsurface_destroy(struct wl_listener *listener, void *data);
static void qubes_subsurface_map(struct wl_listener *listener, void *data);
static void qubes_subsurface_unmap(struct wl_listener *listener, void *data);

extern void *qubes_rust_allocate_renderer_data(void);

#ifdef READ_PIXELS
extern bool qubes_wlr_renderer_read_pixels(struct wlr_renderer *renderer, uint32_t fmt,
		uint32_t *flags, uint32_t stride, uint32_t width, uint32_t height,
		uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y,
		void *data);
#endif

static const uint32_t *qubes_wlr_renderer_get_shm_texture_formats(
      struct wlr_renderer *renderer __attribute__((unused)), size_t *len) {
   static const uint32_t formats [2] = {
      WL_SHM_FORMAT_ARGB8888,
      WL_SHM_FORMAT_XRGB8888,
   };
   if (len)
      *len = 2;
   return formats;
}

static void qubes_surface_new_subsurface(struct wl_listener *listener, void *data)
{
   struct qubes_surface *surface = wl_container_of(listener, surface, new_subsurface);
   struct wlr_subsurface *subsurface = data;
   struct qubes_subsurface *subsurface_impl = calloc(sizeof(*subsurface_impl), 1);

   if (!subsurface_impl) {
      wl_resource_post_no_memory(subsurface->resource);
      return;
   }

   subsurface_impl->subsurface = subsurface;
   subsurface_impl->destroy.notify = qubes_subsurface_destroy;
   subsurface_impl->map.notify = qubes_subsurface_map;
   subsurface_impl->unmap.notify = qubes_subsurface_unmap;
   wl_signal_add(&subsurface->events.destroy, &subsurface_impl->destroy);
   wl_signal_add(&subsurface->events.map, &subsurface_impl->map);
   wl_signal_add(&subsurface->events.unmap, &subsurface_impl->unmap);
}

static void qubes_subsurface_map(struct wl_listener *listener, void *data __attribute__((unused)))
{
   extern void qubes_rust_subsurface_map(struct qubes_subsurface *listener);
   struct qubes_subsurface *q = wl_container_of(listener, q, map);
   qubes_rust_subsurface_map(q);
}

static void qubes_subsurface_unmap(struct wl_listener *listener, void *data __attribute__((unused)))
{
   extern void qubes_rust_subsurface_unmap(struct qubes_subsurface *listener);
   struct qubes_subsurface *q = wl_container_of(listener, q, unmap);
   qubes_rust_subsurface_unmap(q);
}

static void qubes_surface_commit(struct wl_listener *listener, void *data __attribute__((unused)))
{
   extern void qubes_rust_surface_commit(struct qubes_surface *listener);
   struct qubes_surface *q = wl_container_of(listener, q, commit);
   qubes_rust_surface_commit(q);
}

static void qubes_surface_destroy(struct wl_listener *listener, void *data)
{
   struct qubes_surface *surface_impl;
   struct wlr_surface *surface __attribute__((unused)) = data;

   surface_impl = wl_container_of(listener, surface_impl, destroy);
   free(surface_impl);
}

static void qubes_subsurface_destroy(struct wl_listener *listener, void *data)
{
   struct qubes_subsurface *subsurface_impl =
      wl_container_of(listener, subsurface_impl, destroy);
   struct wlr_subsurface *subsurface __attribute__((unused)) = data;

   free(subsurface_impl);
}

static void qubes_new_surface(struct wl_listener *listener, void *data)
{
   struct qubes_shell_listener *q = wl_container_of(listener, q, new_surface);
   struct wlr_surface *const surface = data;
   struct qubes_surface *surface_impl = calloc(sizeof(*surface_impl), 1);

   if (!surface_impl) {
      wl_resource_post_no_memory(surface->resource);
      return;
   }

   surface_impl->surface = surface;
   surface_impl->commit.notify = qubes_surface_commit;
   surface_impl->new_subsurface.notify = qubes_surface_new_subsurface;
   surface_impl->destroy.notify = qubes_surface_destroy;
   wl_signal_add(&surface->events.commit, &surface_impl->commit);
   wl_signal_add(&surface->events.new_subsurface, &surface_impl->new_subsurface);
   wl_signal_add(&surface->events.destroy, &surface_impl->destroy);
   /* do something with q */
}

static void qubes_destroy_shell(struct wl_listener *listener, void *data)
{
   struct qubes_surface *q;
   struct wlr_xdg_shell *shell __attribute__((unused)) = data;

   q = wl_container_of(listener, q, destroy);
   free(q);
}

struct qubes_renderer {
   struct wlr_renderer renderer;
   void *qubes_rust_renderer_data; /* Rust-side implementation */
};

struct qubes_texture {
   struct wlr_texture inner;
   uint32_t fmt;
   uint32_t stride;
   struct wlr_buffer *buffer;
};

static void qubes_wlr_destroy_texture(struct wlr_texture *texture)
{
   free(texture);
}

static const struct wlr_texture_impl qubes_wlr_texture_impl = {
   .is_opaque = NULL,
   .write_pixels = NULL,
   .destroy = qubes_wlr_destroy_texture,
};

#define QUBES_MAX_WINDOW_WIDTH 16384
#define QUBES_MAX_WINDOW_HEIGHT 6144

static struct wlr_texture *qubes_wlr_renderer_texture_from_buffer(
      struct wlr_renderer *raw_renderer,
      struct wlr_buffer *buffer) {
   struct wlr_shm_attributes attribs;
   memset(&attribs, 0, sizeof(attribs));
   if (!wlr_buffer_get_shm(buffer, &attribs))
      return NULL;
   if (attribs.width <= 0 ||
       attribs.height <= 0 ||
       attribs.stride <= 0 ||
       attribs.width > QUBES_MAX_WINDOW_WIDTH ||
       attribs.height > QUBES_MAX_WINDOW_HEIGHT ||
       attribs.width > (attribs.stride >> 2))
      return NULL;
   if (attribs.format != WL_SHM_FORMAT_ARGB8888 &&
       attribs.format != WL_SHM_FORMAT_XRGB8888)
      return NULL;
   struct qubes_renderer *renderer = wl_container_of(raw_renderer, renderer, renderer);
   struct qubes_texture *texture = calloc(sizeof(*texture), 1);
   if (!texture)
      return NULL;
   *texture = (struct qubes_texture) {
      .inner = {
         .impl = &qubes_wlr_texture_impl,
         .width = (uint32_t)attribs.width,
         .height = (uint32_t)attribs.height,
      },
      .fmt = attribs.format,
      .stride = (uint32_t)attribs.stride,
      .buffer = buffer,
   };
   return &texture->inner;
}

static void qubes_wlr_renderer_destroy(struct wlr_renderer *renderer)
{
   extern void qubes_rust_destroy_renderer(void *renderer);
   struct qubes_renderer *q = wl_container_of(renderer, q, renderer);
   qubes_rust_destroy_renderer(q->qubes_rust_renderer_data);
   free(q);
}

static const struct wlr_renderer_impl qubes_renderer = {
   .texture_from_buffer = qubes_wlr_renderer_texture_from_buffer,
#ifdef READ_PIXELS
   .read_pixels = qubes_wlr_renderer_read_pixels,
#endif
   .get_shm_texture_formats = qubes_wlr_renderer_get_shm_texture_formats,
   .destroy = qubes_wlr_renderer_destroy,
};

bool setup(struct wl_display *display)
{
   struct wlr_xdg_shell *shell = NULL;
   struct qubes_renderer *renderer = calloc(sizeof(*renderer), 1);
   if (!renderer)
      return false;
   struct qubes_shell_listener *listener = calloc(sizeof(*listener), 1);
   if (!listener)
      goto fail;
   listener->display = display;

   if (!renderer)
      goto fail;

   wlr_renderer_init(&renderer->renderer, &qubes_renderer);
   if (!(renderer->qubes_rust_renderer_data = qubes_rust_allocate_renderer_data()))
      goto fail;
   if (!(listener->compositor = wlr_compositor_create(display, &renderer->renderer)))
      goto fail;
   if (!(listener->shell = wlr_xdg_shell_create(display)))
      goto fail;
   if (!(listener->seat = wlr_seat_create(display, "Qubes OS Virtual Seat")))
      goto fail;
   if (!(listener->data_device_manager = wlr_data_device_manager_create(listener->display)))
      goto fail;
   if (!(listener->server_decoration_manager = wlr_server_decoration_manager_create(listener->display)))
      goto fail;
   wlr_server_decoration_manager_set_default_mode(
      listener->server_decoration_manager,
      WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
   if (!(listener->xdg_decoration_manager_v1 = wlr_xdg_decoration_manager_v1_create(display)))
      goto fail;
   if (!(listener->xdg_activation_v1 = wlr_xdg_activation_v1_create(display)))
      goto fail;
   listener->new_surface.notify = qubes_new_surface;
   listener->destroy.notify = qubes_destroy_shell;
   wl_signal_add(&shell->events.new_surface, &listener->new_surface);
   wl_signal_add(&shell->events.destroy, &listener->destroy);

   return true;
fail:
   free(listener);
   free(renderer);
   return false;
}
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
