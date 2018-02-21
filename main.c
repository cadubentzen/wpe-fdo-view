/*
 * wpe-fdo-view: A reference application showcasing the
 * WebPlatformForEmbedded (WPE) port of WebKit using the
 * Freedesktop.org backend (WPEBackend-fdo) on Wayland.
 *
 * Copyright (C) 2018, Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 3, or (at your option) any later version as published by
 * the Free Software Foundation.
 *
 * This file is distributed for demostration purposes only, and
 * WITHOUT ANY WARRANTY. See the GNU General Public License
 * at <http://www.gnu.org/licenses/lgpl-3.0.txt> for more details.
 *
 * Authors:
 *   Eduardo Lima Mitev <elima@igalia.com>
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <wpe-fdo/view-backend-exportable.h>
#include <wpe-fdo/initialize-egl.h>
#include <wpe/webkit.h>
#include <wpe/input.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <glib-object.h>
#include <string.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

/* for mmap */
#include <sys/mman.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <locale.h>

#include "xdg-shell-unstable-v6-client-protocol.h"
#include "fullscreen-shell-unstable-v1-client-protocol.h"

#define DEFAULT_WIDTH  1024
#define DEFAULT_HEIGHT  768

#define DEFAULT_URL "https://igalia.com"
// #define DEFAULT_URL "http://webglsamples.org/video/video.html"
// #define DEFAULT_URL "http://webglsamples.org/dynamic-cubemap/dynamic-cubemap.html"

static GMainLoop *main_loop = NULL;

static struct {
   struct wl_display *display;
   struct wl_registry *registry;
   struct wl_compositor *compositor;

   struct zxdg_shell_v6 *xdg_shell;
   struct zwp_fullscreen_shell_v1 *fshell;
   struct wl_shell *shell;

   struct wl_seat *seat;
   struct {
      struct wl_pointer *obj;
      int32_t x;
      int32_t y;
      uint32_t button;
      uint32_t state;
   } pointer;

   struct {
      struct wl_keyboard *obj;

      struct {
         int32_t rate;
         int32_t delay;
      } repeat_info;

      struct {
         uint32_t key;
         uint32_t time;
         uint32_t state;
         uint32_t event_source;
      } repeat_data;

      uint32_t serial;
   } keyboard;

   GSource *event_src;
} wl_data = {NULL, };

static struct {
   struct egl_display *display;
   EGLContext context;
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEglImageTargetTexture2D;
   EGLConfig egl_config;
} egl_data;

static struct {
   struct wl_surface *wl_surface;
   struct wl_egl_window *egl_window;
   EGLSurface egl_surface;

   struct zxdg_surface_v6 *xdg_surface;
   struct zxdg_toplevel_v6 *xdg_toplevel;
   struct wl_shell_surface *shell_surface;

   uint32_t width;
   uint32_t height;

   bool is_fullscreen;
} win_data;

static struct gl_data {
   GLuint program;
   GLuint tex;
   GLuint tex_loc;
} gl_data = {0, };

static struct {
   struct xkb_context* context;
   struct xkb_keymap* keymap;
   struct xkb_state* state;

   struct xkb_compose_table* compose_table;
   struct xkb_compose_state* compose_state;

   struct {
      xkb_mod_index_t control;
      xkb_mod_index_t alt;
      xkb_mod_index_t shift;
   } indexes;
   uint8_t modifiers;
} xkb_data = {NULL, };

static struct {
   struct wpe_view_backend_exportable_fdo *exportable;
} wpe_host_data;

static struct {
   struct wpe_view_backend *backend;
   struct wl_resource* current_buffer;
   EGLImageKHR image;
   struct wl_callback *frame_callback;
} wpe_view_data = {NULL, };


static void draw (void);

static void
trap_sigint (int sig)
{
   if (main_loop != NULL)
      g_main_loop_quit (main_loop);

   signal (SIGINT, NULL);
}


struct wl_event_source {
   GSource source;
   GPollFD pfd;
   struct wl_display* display;
};

static gboolean
wl_src_prepare (GSource *base, gint *timeout)
{
   struct wl_event_source *src = (struct wl_event_source *) base;

   *timeout = -1;
   wl_display_dispatch_pending (src->display);
   wl_display_flush (src->display);

   return false;
}

static gboolean
wl_src_check (GSource *base)
{
   struct wl_event_source *src = (struct wl_event_source *) base;
   return !! src->pfd.revents;
}

static gboolean
wl_src_dispatch (GSource *base, GSourceFunc callback, gpointer user_data)
{
   struct wl_event_source *src = (struct wl_event_source *) base;

   if (src->pfd.revents & G_IO_IN)
      wl_display_dispatch (src->display);

   if (src->pfd.revents & (G_IO_ERR | G_IO_HUP))
      return false;

   src->pfd.revents = 0;

   return true;
}

static void
wl_src_finalize (GSource *base)
{
}

static GSource *
setup_wayland_event_source (GMainContext *main_context,
                            struct wl_display *display)
{
   static GSourceFuncs wl_src_funcs = {
      .prepare = wl_src_prepare,
      .check = wl_src_check,
      .dispatch = wl_src_dispatch,
      .finalize = wl_src_finalize,
   };

   struct wl_event_source *wl_source =
      (struct wl_event_source *) g_source_new (&wl_src_funcs,
                                               sizeof (struct wl_event_source));
   wl_source->display = display;
   wl_source->pfd.fd = wl_display_get_fd (display);
   wl_source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
   wl_source->pfd.revents = 0;
   g_source_add_poll (&wl_source->source, &wl_source->pfd);

   g_source_set_priority (&wl_source->source, G_PRIORITY_HIGH + 30);
   g_source_set_can_recurse (&wl_source->source, TRUE);
   g_source_attach (&wl_source->source, g_main_context_get_thread_default());

   g_source_unref (&wl_source->source);

   return &wl_source->source;
}

static void
resize_window (void)
{
   wl_egl_window_resize (win_data.egl_window,
                         win_data.width,
                         win_data.height,
                         0, 0);

   glViewport (0, 0, win_data.width, win_data.height);

   wpe_view_backend_dispatch_set_size (wpe_view_data.backend,
                                       win_data.width,
                                       win_data.height);
}

static void
shell_surface_ping (void *data,
                    struct wl_shell_surface *shell_surface,
                    uint32_t serial)
{
   wl_shell_surface_pong (shell_surface, serial);
}

static void
shell_surface_configure (void *data,
                         struct wl_shell_surface *shell_surface,
                         uint32_t edges,
                         int32_t width, int32_t height)
{
   win_data.width = width;
   win_data.height = height;
   if (width == 0 || height == 0)
      return;

   printf ("New wl_shell configuration: (%u, %u)\n", width, height);

   resize_window ();
}

static const struct wl_shell_surface_listener shell_surface_listener = {
   .ping = shell_surface_ping,
   .configure = shell_surface_configure,
};

static void
xdg_shell_ping (void *data, struct zxdg_shell_v6 *shell, uint32_t serial)
{
   zxdg_shell_v6_pong(shell, serial);
}

static const struct zxdg_shell_v6_listener xdg_shell_listener = {
   .ping = xdg_shell_ping,
};

static void
xdg_surface_on_configure (void *data,
                          struct zxdg_surface_v6 *surface,
                          uint32_t serial)
{
   zxdg_surface_v6_ack_configure (surface, serial);

   if (win_data.width == 0 || win_data.height == 0)
      return;
}

static const struct zxdg_surface_v6_listener xdg_surface_listener = {
   .configure = xdg_surface_on_configure
};

static void
xdg_toplevel_on_configure (void *data,
                           struct zxdg_toplevel_v6 *toplevel,
                           int32_t width, int32_t height,
                           struct wl_array *states)
{
   win_data.width = width;
   win_data.height = height;
   if (width == 0 || height == 0)
      return;

   printf ("New XDG toplevel configuration: (%u, %u)\n", width, height);

   resize_window ();
}

static void
xdg_toplevel_on_close (void *data, struct zxdg_toplevel_v6 *xdg_toplevel)
{
   g_main_loop_quit (main_loop);
}

static const struct zxdg_toplevel_v6_listener xdg_toplevel_listener = {
   .configure = xdg_toplevel_on_configure,
   .close = xdg_toplevel_on_close,
};

static void
registry_global (void               *data,
                 struct wl_registry *registry,
                 uint32_t            name,
                 const char         *interface,
                 uint32_t            version)
{
   if (strcmp (interface, wl_compositor_interface.name) == 0) {
      printf ("Wayland: Got a wl_compositor interface\n");
      wl_data.compositor = wl_registry_bind (registry,
                                             name,
                                             &wl_compositor_interface,
                                             version);
   } else if (strcmp (interface, wl_shell_interface.name) == 0) {
      printf ("Wayland: Got a wl_shell interface\n");
      wl_data.shell = wl_registry_bind (registry,
                                        name,
                                        &wl_shell_interface,
                                        version);
   } else if (strcmp (interface, zxdg_shell_v6_interface.name) == 0) {
      printf ("Wayland: Got an xdg_shell interface\n");
      wl_data.xdg_shell = wl_registry_bind (registry,
                                            name,
                                            &zxdg_shell_v6_interface,
                                            version);
      assert (wl_data.xdg_shell != NULL);
      zxdg_shell_v6_add_listener (wl_data.xdg_shell, &xdg_shell_listener, NULL);
   } else if (strcmp (interface,
                      zwp_fullscreen_shell_v1_interface.name) == 0) {
      printf ("Wayland: Got a fullscreen_shell interface\n");
      wl_data.fshell = wl_registry_bind (registry,
                                         name,
                                         &zwp_fullscreen_shell_v1_interface,
                                         version);
   } else if (strcmp (interface, wl_seat_interface.name) == 0) {
      printf ("Wayland: Got a wl_seat interface\n");
      wl_data.seat = wl_registry_bind (registry,
                                       name,
                                       &wl_seat_interface,
                                       version);
   }
}

static void
pointer_on_enter (void* data,
                  struct wl_pointer* pointer,
                  uint32_t serial,
                  struct wl_surface* surface,
                  wl_fixed_t fixed_x,
                  wl_fixed_t fixed_y)
{
}

static void
pointer_on_leave (void* data,
                  struct wl_pointer *pointer,
                  uint32_t serial,
                  struct wl_surface* surface)
{
}

static void
pointer_on_motion (void* data,
                   struct wl_pointer *pointer,
                   uint32_t time,
                   wl_fixed_t fixed_x,
                   wl_fixed_t fixed_y)
{
   wl_data.pointer.x = wl_fixed_to_int (fixed_x);
   wl_data.pointer.y = wl_fixed_to_int (fixed_y);

   struct wpe_input_pointer_event event = {
      wpe_input_pointer_event_type_motion,
      time,
      wl_data.pointer.x,
      wl_data.pointer.y,
      wl_data.pointer.button,
      wl_data.pointer.state
   };

   wpe_view_backend_dispatch_pointer_event (wpe_view_data.backend, &event);
}

static void
pointer_on_button (void* data,
                   struct wl_pointer *pointer,
                   uint32_t serial,
                   uint32_t time,
                   uint32_t button,
                   uint32_t state)
{
   /* @FIXME: what is this for?
   if (button >= BTN_MOUSE)
      button = button - BTN_MOUSE + 1;
   else
      button = 0;
   */

   wl_data.pointer.button = !!state ? button : 0;
   wl_data.pointer.state = state;

   struct wpe_input_pointer_event event = {
      wpe_input_pointer_event_type_button,
      time,
      wl_data.pointer.x,
      wl_data.pointer.y,
      wl_data.pointer.button,
      wl_data.pointer.state,
   };

   wpe_view_backend_dispatch_pointer_event (wpe_view_data.backend, &event);
}

static void
pointer_on_axis (void* data,
                 struct wl_pointer *pointer,
                 uint32_t time,
                 uint32_t axis,
                 wl_fixed_t value)
{
   struct wpe_input_axis_event event = {
      wpe_input_axis_event_type_motion,
      time,
      wl_data.pointer.x,
      wl_data.pointer.y,
      axis,
      - wl_fixed_to_int (value),
   };

   wpe_view_backend_dispatch_axis_event (wpe_view_data.backend, &event);
}

static void
pointer_on_frame (void* data,
                  struct wl_pointer *pointer)
{
   /* @FIXME: buffer pointer events and handle them in frame. That's the
    * recommended usage of this interface.
    */
}

static void
pointer_on_axis_source (void *data,
                        struct wl_pointer *wl_pointer,
                        uint32_t axis_source)
{
}

static void
pointer_on_axis_stop (void *data,
                      struct wl_pointer *wl_pointer,
                      uint32_t time,
                      uint32_t axis)
{
}

static void
pointer_on_axis_discrete (void *data,
                          struct wl_pointer *wl_pointer,
                          uint32_t axis,
                          int32_t discrete)
{
}

static const struct wl_pointer_listener pointer_listener = {
   .enter = pointer_on_enter,
   .leave = pointer_on_leave,
   .motion = pointer_on_motion,
   .button = pointer_on_button,
   .axis = pointer_on_axis,
   .frame = pointer_on_frame,
   .axis_source = pointer_on_axis_source,
   .axis_stop = pointer_on_axis_stop,
   .axis_discrete = pointer_on_axis_discrete,
};

static void
keyboard_on_keymap (void *data,
                    struct wl_keyboard *wl_keyboard,
                    uint32_t format,
                    int32_t fd,
                    uint32_t size)
{
   if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
      close (fd);
      return;
   }

   void* mapping = mmap (NULL, size, PROT_READ, MAP_SHARED, fd, 0);
   if (mapping == MAP_FAILED) {
      close (fd);
      return;
   }

   xkb_data.keymap =
      xkb_keymap_new_from_string (xkb_data.context,
                                  (char *) mapping,
                                  XKB_KEYMAP_FORMAT_TEXT_V1,
                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
   munmap (mapping, size);
   close (fd);

   if (xkb_data.keymap == NULL)
      return;

   xkb_data.state = xkb_state_new (xkb_data.keymap);
   if (xkb_data.state == NULL)
      return;

   xkb_data.indexes.control = xkb_keymap_mod_get_index (xkb_data.keymap,
                                                        XKB_MOD_NAME_CTRL);
   xkb_data.indexes.alt = xkb_keymap_mod_get_index (xkb_data.keymap,
                                                    XKB_MOD_NAME_ALT);
   xkb_data.indexes.shift = xkb_keymap_mod_get_index (xkb_data.keymap,
                                                      XKB_MOD_NAME_SHIFT);
}

static void
keyboard_on_enter (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface,
                   struct wl_array *keys)
{
   assert (surface == win_data.wl_surface);
   wl_data.keyboard.serial = serial;
}

static void
keyboard_on_leave (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface)
{
   wl_data.keyboard.serial = serial;
}

static bool
capture_app_key_bindings (uint32_t keysym,
                          uint32_t unicode,
                          uint32_t state,
                          uint8_t modifiers)
{
   if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      /* fullscreen */
      if (modifiers == 0 && unicode == 0 && keysym == XKB_KEY_F11) {
         if (! win_data.is_fullscreen)
            zxdg_toplevel_v6_set_fullscreen (win_data.xdg_toplevel, NULL);
         else
            zxdg_toplevel_v6_unset_fullscreen (win_data.xdg_toplevel);
         win_data.is_fullscreen = ! win_data.is_fullscreen;
         return true;
      }
      /* Ctrl+W, exit the application */
      else if (modifiers == wpe_input_keyboard_modifier_control &&
               unicode == 0x17 && keysym == 0x77) {
         g_main_loop_quit (main_loop);
         return true;
      }
   }

   return false;
}

static void
handle_key_event (uint32_t key, uint32_t state, uint32_t time)
{
   uint32_t keysym = xkb_state_key_get_one_sym (xkb_data.state, key);
   uint32_t unicode = xkb_state_key_get_utf32 (xkb_data.state, key);

   /* Capture app-level key-bindings here */
   if (capture_app_key_bindings (keysym, unicode, state, xkb_data.modifiers))
      return;

   if (xkb_data.compose_state != NULL
       && state == WL_KEYBOARD_KEY_STATE_PRESSED
       && xkb_compose_state_feed (xkb_data.compose_state, keysym)
         == XKB_COMPOSE_FEED_ACCEPTED
       && xkb_compose_state_get_status (xkb_data.compose_state)
         == XKB_COMPOSE_COMPOSED) {
      keysym = xkb_compose_state_get_one_sym (xkb_data.compose_state);
      unicode = xkb_keysym_to_utf32 (keysym);
   }

   struct wpe_input_keyboard_event event = {
      time,
      keysym,
      unicode,
      state == true,
      xkb_data.modifiers
   };

   wpe_view_backend_dispatch_keyboard_event (wpe_view_data.backend, &event);
}

static gboolean
repeat_delay_timeout (void *data)
{
   handle_key_event (wl_data.keyboard.repeat_data.key,
                     wl_data.keyboard.repeat_data.state,
                     wl_data.keyboard.repeat_data.time);

   wl_data.keyboard.repeat_data.event_source =
      g_timeout_add (wl_data.keyboard.repeat_info.rate,
                     (GSourceFunc) repeat_delay_timeout, NULL);

   return G_SOURCE_REMOVE;
}

static void
keyboard_on_key (void *data,
                 struct wl_keyboard *wl_keyboard,
                 uint32_t serial,
                 uint32_t time,
                 uint32_t key,
                 uint32_t state)
{
   /* @FIXME: investigate why is this necessary */
   // IDK.
   key += 8;

   wl_data.keyboard.serial = serial;
   handle_key_event (key, state, time);

   if (wl_data.keyboard.repeat_info.rate == 0)
      return;

   if (state == WL_KEYBOARD_KEY_STATE_RELEASED
       && wl_data.keyboard.repeat_data.key == key) {
      if (wl_data.keyboard.repeat_data.event_source)
         g_source_remove (wl_data.keyboard.repeat_data.event_source);

      memset (&wl_data.keyboard.repeat_data,
              0x00,
              sizeof (wl_data.keyboard.repeat_data));
   } else if (state == WL_KEYBOARD_KEY_STATE_PRESSED
              && xkb_keymap_key_repeats (xkb_data.keymap, key)) {
      if (wl_data.keyboard.repeat_data.event_source)
         g_source_remove (wl_data.keyboard.repeat_data.event_source);

      wl_data.keyboard.repeat_data.key = key;
      wl_data.keyboard.repeat_data.time = time;
      wl_data.keyboard.repeat_data.state = state;
      wl_data.keyboard.repeat_data.event_source =
         g_timeout_add (wl_data.keyboard.repeat_info.delay,
                        (GSourceFunc) repeat_delay_timeout, NULL);
   }
}

static void
keyboard_on_modifiers (void *data,
                       struct wl_keyboard *wl_keyboard,
                       uint32_t serial,
                       uint32_t mods_depressed,
                       uint32_t mods_latched,
                       uint32_t mods_locked,
                       uint32_t group)
{
   xkb_state_update_mask (xkb_data.state,
                          mods_depressed,
                          mods_latched,
                          mods_locked,
                          0,
                          0,
                          group);

   xkb_data.modifiers = 0;
   uint32_t component
      = (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

   if (xkb_state_mod_index_is_active (xkb_data.state,
                                      xkb_data.indexes.control,
                                      component)) {
      xkb_data.modifiers |= wpe_input_keyboard_modifier_control;
   }
   if (xkb_state_mod_index_is_active (xkb_data.state,
                                      xkb_data.indexes.alt,
                                      component)) {
      xkb_data.modifiers |= wpe_input_keyboard_modifier_alt;
   }
   if (xkb_state_mod_index_is_active (xkb_data.state,
                                      xkb_data.indexes.shift,
                                      component)) {
      xkb_data.modifiers |= wpe_input_keyboard_modifier_shift;
   }
}

static void
keyboard_on_repeat_info (void *data,
                         struct wl_keyboard *wl_keyboard,
                         int32_t rate,
                         int32_t delay)
{
   wl_data.keyboard.repeat_info.rate = rate;
   wl_data.keyboard.repeat_info.delay = delay;

   /* a rate of zero disables any repeating. */
   if (rate == 0 && wl_data.keyboard.repeat_data.event_source > 0) {
      g_source_remove(wl_data.keyboard.repeat_data.event_source);
      memset (&wl_data.keyboard.repeat_data,
              0x00,
              sizeof (wl_data.keyboard.repeat_data));
   }
}

static const struct wl_keyboard_listener keyboard_listener = {
   .keymap = keyboard_on_keymap,
   .enter = keyboard_on_enter,
   .leave = keyboard_on_leave,
   .key = keyboard_on_key,
   .modifiers = keyboard_on_modifiers,
   .repeat_info = keyboard_on_repeat_info,
};

static void
seat_on_capabilities (void* data, struct wl_seat* seat, uint32_t capabilities)
{
   printf ("Seat caps: ");

   /* Pointer */
   const bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
   if (has_pointer && wl_data.pointer.obj == NULL) {
      wl_data.pointer.obj = wl_seat_get_pointer (wl_data.seat);
      assert (wl_data.pointer.obj != NULL);
      wl_pointer_add_listener (wl_data.pointer.obj, &pointer_listener, NULL);
      printf ("Pointer ");
   } else if (! has_pointer && wl_data.pointer.obj != NULL) {
      wl_pointer_release (wl_data.pointer.obj);
      wl_data.pointer.obj = NULL;
   }

   /* Keyboard */
   const bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
   if (has_keyboard && wl_data.keyboard.obj == NULL) {
      wl_data.keyboard.obj = wl_seat_get_keyboard (wl_data.seat);
      assert (wl_data.keyboard.obj != NULL);
      wl_keyboard_add_listener (wl_data.keyboard.obj, &keyboard_listener, NULL);
      printf ("Keyboard ");
   } else if (! has_keyboard && wl_data.keyboard.obj != NULL) {
      wl_keyboard_release (wl_data.keyboard.obj);
      wl_data.pointer.obj = NULL;
   }

   printf ("\n");
}

static void
seat_on_name (void *data, struct wl_seat *seat, const char *name)
{
   printf ("Seat name '%s'\n", name);
}

static const struct wl_seat_listener seat_listener = {
   .capabilities = seat_on_capabilities,
   .name = seat_on_name,
};

static void
registry_global_remove (void *a, struct wl_registry *b, uint32_t c)
{
}

static const struct wl_registry_listener registry_listener = {
   .global = registry_global,
   .global_remove = registry_global_remove
};

static void
on_surface_frame (void *data, struct wl_callback *callback, uint32_t time)
{
   if (wpe_view_data.frame_callback != NULL) {
      assert (wpe_view_data.frame_callback == callback);
      wl_callback_destroy (wpe_view_data.frame_callback);
      wpe_view_data.frame_callback = NULL;
   }

   if (wpe_view_data.current_buffer != NULL) {
      wpe_view_backend_exportable_fdo_dispatch_frame_complete
         (wpe_host_data.exportable);

      wpe_view_backend_exportable_fdo_dispatch_release_buffer
         (wpe_host_data.exportable, wpe_view_data.current_buffer);
      wpe_view_data.current_buffer = NULL;

      eglDestroyImage (egl_data.display, wpe_view_data.image);
      wpe_view_data.image = NULL;
   }
}

static const struct wl_callback_listener frame_listener = {
   .done = on_surface_frame
};

static void
request_frame (void)
{
   if (wpe_view_data.frame_callback != NULL)
      return;

   wpe_view_data.frame_callback = wl_surface_frame (win_data.wl_surface);
   wl_callback_add_listener (wpe_view_data.frame_callback,
                             &frame_listener,
                             NULL);
}

static void
draw (void)
{
   egl_data.glEglImageTargetTexture2D (GL_TEXTURE_2D, wpe_view_data.image);

   glClearColor (0.0, 0.0, 0.0, 1.0);
   glClear (GL_COLOR_BUFFER_BIT);

   glDrawArrays (GL_TRIANGLE_FAN, 0, 4);

   request_frame ();

   eglSwapBuffers (egl_data.display, win_data.egl_surface);
}

static void
on_export_buffer_resource (void* data, struct wl_resource* buffer_resource)
{
   wpe_view_data.current_buffer = buffer_resource;

   static EGLAttrib image_attrs[] = {
      EGL_WAYLAND_PLANE_WL, 0,
      EGL_NONE
   };
   wpe_view_data.image = eglCreateImage (egl_data.display,
                                         egl_data.context,
                                         EGL_WAYLAND_BUFFER_WL,
                                         buffer_resource,
                                         image_attrs);
   assert (wpe_view_data.image != NULL);

   draw ();
}

static void
init_wayland (void)
{
   wl_data.display = wl_display_connect (NULL);
   assert (wl_data.display != NULL);

   wl_data.registry = wl_display_get_registry (wl_data.display);
   assert (wl_data.registry != NULL);
   wl_registry_add_listener (wl_data.registry,
                             &registry_listener,
                             NULL);
   wl_display_roundtrip (wl_data.display);

   assert (wl_data.compositor != NULL);
   assert (wl_data.xdg_shell != NULL ||
           wl_data.shell != NULL ||
           wl_data.fshell != NULL);

   wl_data.event_src =
      setup_wayland_event_source (g_main_context_get_thread_default (),
                                  wl_data.display);
}

static void
clear_wayland (void)
{
   g_source_destroy (wl_data.event_src);

   if (wl_data.xdg_shell != NULL)
      zxdg_shell_v6_destroy (wl_data.xdg_shell);
   if (wl_data.fshell != NULL)
      zwp_fullscreen_shell_v1_destroy (wl_data.fshell);
   if (wl_data.shell != NULL)
      wl_shell_destroy (wl_data.shell);

   if (wl_data.compositor != NULL)
      wl_compositor_destroy (wl_data.compositor);

   wl_registry_destroy (wl_data.registry);
   wl_display_flush (wl_data.display);
   wl_display_disconnect (wl_data.display);
}

static void
init_egl (void)
{
   egl_data.display = eglGetDisplay ((EGLNativeDisplayType) wl_data.display);
   assert (egl_data.display != NULL);

   EGLint major, minor;
   if (! eglInitialize (egl_data.display, &major, &minor))
      assert (!"Error initializing EGL display\n");
   printf ("EGL initialized with version %d.%d\n", major, minor);

   if (! eglBindAPI (EGL_OPENGL_ES_API))
      assert (!"Error binding OpenGL-ES API\n");

   static const EGLint context_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE
   };

   static const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE, 1,
      EGL_GREEN_SIZE, 1,
      EGL_BLUE_SIZE, 1,
      EGL_ALPHA_SIZE, 1,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
      EGL_NONE
   };

   EGLint num_configs;
   if (! eglChooseConfig(egl_data.display,
                         config_attribs,
                         &egl_data.egl_config,
                         1,
                         &num_configs)) {
      assert (!"Error choosing EGL config\n");
   }
   assert (num_configs > 0);

   egl_data.context = eglCreateContext (egl_data.display,
                                        egl_data.egl_config,
                                        EGL_NO_CONTEXT,
                                        context_attribs);
   assert (egl_data.context != NULL);

   egl_data.glEglImageTargetTexture2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
      eglGetProcAddress ("glEGLImageTargetTexture2DOES");
   assert (egl_data.glEglImageTargetTexture2D != NULL);
}

static void
clear_egl (void)
{
   eglDestroyContext (egl_data.display, egl_data.context);
   eglTerminate (egl_data.display);
   eglReleaseThread ();
}

static void
create_window (void)
{
   win_data.wl_surface = wl_compositor_create_surface (wl_data.compositor);
   assert (win_data.wl_surface != NULL);

   if (wl_data.xdg_shell != NULL) {
      win_data.xdg_surface =
         zxdg_shell_v6_get_xdg_surface (wl_data.xdg_shell,
                                        win_data.wl_surface);
      assert (win_data.xdg_surface != NULL);

      zxdg_surface_v6_add_listener (win_data.xdg_surface,
                                    &xdg_surface_listener,
                                    NULL);
      win_data.xdg_toplevel =
         zxdg_surface_v6_get_toplevel (win_data.xdg_surface);
      assert (win_data.xdg_toplevel != NULL);

      zxdg_toplevel_v6_add_listener (win_data.xdg_toplevel,
                                     &xdg_toplevel_listener,
                                     NULL);
      zxdg_toplevel_v6_set_title (win_data.xdg_toplevel, "wpe-fdo-view");
      zxdg_toplevel_v6_set_app_id (win_data.xdg_toplevel, "wpe-fdo-view");
   } else if (wl_data.fshell != NULL) {
      zwp_fullscreen_shell_v1_present_surface (wl_data.fshell,
                                 win_data.wl_surface,
                                 ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
                                 NULL);
   } else if (wl_data.shell != NULL) {
      win_data.shell_surface = wl_shell_get_shell_surface (wl_data.shell,
                                                           win_data.wl_surface);
      assert (win_data.shell_surface != NULL);

      wl_shell_surface_add_listener (win_data.shell_surface,
                                     &shell_surface_listener,
                                     0);
      wl_shell_surface_set_toplevel (win_data.shell_surface);

      if (false) {
         wl_shell_surface_set_fullscreen (win_data.shell_surface,
                                       WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE,
                                       0,
                                       NULL);
         wl_display_roundtrip (wl_data.display);
      }
   }

   win_data.egl_window = wl_egl_window_create (win_data.wl_surface,
                                               DEFAULT_WIDTH,
                                               DEFAULT_HEIGHT);
   assert (win_data.egl_window != NULL);

   win_data.egl_surface =
      eglCreateWindowSurface (egl_data.display,
                              egl_data.egl_config,
                              (EGLNativeWindowType) win_data.egl_window,
                              NULL);
   assert (win_data.egl_surface != NULL);

   /* @FIXME: figure out what the below code does
   struct wl_region *region = wl_compositor_create_region (wl_data.compositor);
   wl_region_add (region, 0, 0, DEFAULT_WIDTH, DEFAULT_HEIGHT);
   wl_surface_set_opaque_region (win_data.wl_surface, region);
   wl_region_destroy (region);
   */

   wl_surface_commit (win_data.wl_surface);
   wl_display_flush (wl_data.display);

   /* Activate window */
   if (! eglMakeCurrent (egl_data.display,
                         win_data.egl_surface,
                         win_data.egl_surface,
                         egl_data.context)) {
      assert (!"Error make the EGL context current\n");
   }
}

static void
destroy_window (void)
{
   eglMakeCurrent (egl_data.display,
                   EGL_NO_SURFACE,
                   EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
   eglDestroySurface (egl_data.display, win_data.egl_surface);

   wl_egl_window_destroy (win_data.egl_window);

   if (win_data.xdg_toplevel != NULL)
      zxdg_toplevel_v6_destroy (win_data.xdg_toplevel);

   if (win_data.xdg_surface != NULL)
      zxdg_surface_v6_destroy (win_data.xdg_surface);

   if (win_data.shell_surface != NULL)
      wl_shell_surface_destroy (win_data.shell_surface);

   wl_surface_destroy (win_data.wl_surface);
}

static void
init_input (void)
{
   if (wl_data.seat != NULL) {
      wl_seat_add_listener (wl_data.seat, &seat_listener, NULL);

      xkb_data.context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
      assert (xkb_data.context != NULL);
      xkb_data.compose_table =
         xkb_compose_table_new_from_locale (xkb_data.context,
                                            setlocale (LC_CTYPE, NULL),
                                            XKB_COMPOSE_COMPILE_NO_FLAGS);
      if (xkb_data.compose_table != NULL) {
         xkb_data.compose_state =
            xkb_compose_state_new (xkb_data.compose_table,
                                   XKB_COMPOSE_STATE_NO_FLAGS);
      }
   }
}

static void
clear_input (void)
{
   if (wl_data.pointer.obj != NULL)
      wl_pointer_destroy (wl_data.pointer.obj);
   if (wl_data.keyboard.obj != NULL)
      wl_keyboard_destroy (wl_data.keyboard.obj);
   if (wl_data.seat != NULL)
      wl_seat_destroy (wl_data.seat);

   if (xkb_data.state != NULL)
      xkb_state_unref (xkb_data.state);
   if (xkb_data.compose_state != NULL)
      xkb_compose_state_unref (xkb_data.compose_state);
   if (xkb_data.compose_table != NULL)
      xkb_compose_table_unref (xkb_data.compose_table);
   if (xkb_data.keymap != NULL)
      xkb_keymap_unref (xkb_data.keymap);
   if (xkb_data.context != NULL)
      xkb_context_unref (xkb_data.context);
}

static bool
print_shader_log (GLuint shader)
{
   GLint length;
   char buffer[4096] = {0};
   GLint success;

   glGetShaderiv (shader, GL_INFO_LOG_LENGTH, &length);
   if (length == 0)
      return true;

   glGetShaderInfoLog (shader, 4096, NULL, buffer);
   if (strlen (buffer) > 0)
      printf ("Shader compilation log: %s\n", buffer);

   glGetShaderiv (shader, GL_COMPILE_STATUS, &success);

   return success == GL_TRUE;
}

static GLuint
utils_load_shader (const char *shader_source, GLenum type)
{
   GLuint shader = glCreateShader (type);

   glShaderSource (shader, 1, &shader_source, NULL);
   assert (glGetError () == GL_NO_ERROR);
   glCompileShader (shader);
   assert (glGetError () == GL_NO_ERROR);

   print_shader_log (shader);

   return shader;
}

static void
init_gles (void)
{
   const char *VERTEX_SOURCE =
      "#version 310 es\n"
      "out vec2 tex_coord;\n"
      "const vec2 vertices[4] = vec2[](vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0), vec2(-1.0, -1.0));\n"
      "const vec2 coords[4] = vec2[](vec2(1.0, 1.0), vec2(1.0, 0.0), vec2(0.0, 0.0), vec2(0.0, 1.0));\n"
      "void main() {\n"
      "  tex_coord = coords[gl_VertexID];\n"
      "  gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);\n"
      "}\n";

   const char *FRAGMENT_SOURCE =
      "#version 310 es\n"
      "precision mediump float;\n"
      "in vec2 tex_coord;\n"
      "out vec4 my_FragColor;\n"
      "uniform sampler2D u_tex;\n"
      "\n"
      "void main() {\n"
      "  my_FragColor = texture2D (u_tex, tex_coord);\n"
      "}\n";

   GLuint vertex_shader = utils_load_shader (VERTEX_SOURCE, GL_VERTEX_SHADER);
   assert (vertex_shader >= 0);
   assert (glGetError () == GL_NO_ERROR);

   GLuint fragment_shader = utils_load_shader (FRAGMENT_SOURCE,
                                               GL_FRAGMENT_SHADER);
   assert (fragment_shader >= 0);
   assert (glGetError () == GL_NO_ERROR);

   gl_data.program = glCreateProgram ();
   assert (glGetError () == GL_NO_ERROR);
   glAttachShader (gl_data.program, vertex_shader);
   assert (glGetError () == GL_NO_ERROR);
   glAttachShader (gl_data.program, fragment_shader);
   assert (glGetError () == GL_NO_ERROR);
   glLinkProgram (gl_data.program);
   assert (glGetError () == GL_NO_ERROR);

   glDeleteShader (vertex_shader);
   glDeleteShader (fragment_shader);

   glUseProgram (gl_data.program);
   assert (glGetError () == GL_NO_ERROR);

   glEnable (GL_BLEND);
   glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   gl_data.tex_loc = glGetUniformLocation (gl_data.program, "u_tex");
   assert (gl_data.tex_loc != GL_INVALID_INDEX);

   glGenTextures (1, &gl_data.tex);
   assert (glGetError () == GL_NO_ERROR);
   assert (gl_data.tex > 0);
   glBindTexture (GL_TEXTURE_2D, gl_data.tex);
   assert (glGetError () == GL_NO_ERROR);
   glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

   glActiveTexture (GL_TEXTURE0);
   glUniform1i (gl_data.tex_loc, 0);
}

static void
clear_gles (void)
{
   glUseProgram (0);
   glDeleteProgram (gl_data.program);
   glDeleteTextures (1, &gl_data.tex);
}

int32_t
main (int32_t argc, char *argv[])
{
   init_wayland ();
   init_egl ();
   create_window ();
   init_input ();
   init_gles ();

   /* init WPE host data */
   wpe_fdo_initialize_for_egl_display (egl_data.display);

   struct wpe_view_backend_exportable_fdo_client exportable_client = {
      .export_buffer_resource = on_export_buffer_resource,
   };

   wpe_host_data.exportable =
      wpe_view_backend_exportable_fdo_create (&exportable_client,
                                              NULL,
                                              DEFAULT_WIDTH,
                                              DEFAULT_HEIGHT);
   assert (wpe_host_data.exportable != NULL);

   /* init WPE view backend */
   wpe_view_data.backend =
      wpe_view_backend_exportable_fdo_get_view_backend (wpe_host_data.exportable);
   assert (wpe_view_data.backend != NULL);

   WebKitWebViewBackend *wk_view_backend =
      webkit_web_view_backend_new (wpe_view_data.backend,
                       (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                       wpe_host_data.exportable);
   assert (wk_view_backend != NULL);

   /* WebKit web context */
   WebKitWebContext *web_context = webkit_web_context_get_default ();
   assert (web_context != NULL);

   /* WebKit settings */
   WebKitSettings *settings =
      webkit_settings_new_with_settings ("allow-file-access-from-file-urls",
                                         true,
                                "allow-universal-access-from-file-urls", true,
                                "enable-write-console-messages-to-stdout", true,
                                NULL);
   assert (settings != NULL);

   /* WebKit web view */
   WebKitWebView *view = g_object_new (webkit_web_view_get_type (),
                                       "backend", wk_view_backend,
                                       "web-context", web_context,
                                       "settings", settings,
                                       NULL);
   assert (view != NULL);

   /* start the show */
   const char *url;
   if (argc > 1)
      url = argv[1];
   else
      url = DEFAULT_URL;

   webkit_web_view_load_uri (view, url);

   signal (SIGINT, trap_sigint);
   main_loop = g_main_loop_new (NULL, false);
   g_main_loop_run (main_loop);

   /* free app data */
   g_main_loop_unref (main_loop);

   /* free WPE view data */
   if (wpe_view_data.frame_callback != NULL)
      wl_callback_destroy (wpe_view_data.frame_callback);
   if (wpe_view_data.image != NULL)
      eglDestroyImage (egl_data.display, wpe_view_data.image);
   g_object_unref (view);
   g_object_unref (settings);
   /* @FIXME: check why this segfaults
   wpe_view_backend_destroy (wpe_view_data.backend);
   */

   /* free WPE host data */
   /* @FIXME: check why this segfaults
   wpe_view_backend_exportable_fdo_destroy (wpe_host_data.exportable);
   */

   clear_gles ();
   clear_input ();
   destroy_window ();
   clear_egl ();
   clear_wayland ();

   printf ("Exit clean\n");
   return 0;
}
