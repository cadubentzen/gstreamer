/* GStreamer
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * glib-compat.h: Public GLib compatibility shims
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */


/*
 * Modified by the GLib Team and others 1997-2000. See the AUTHORS file from
 * glib-2.8.0 for a list of people on the GLib Team. See the ChangeLog files
 * from glib-2.8.0 for a list of changes. These files are distributed with GLib
 * at ftp://ftp.gtk.org/pub/gtk/.
 */

#ifndef __GST_GLIB_COMPAT_H__
#define __GST_GLIB_COMPAT_H__

#include <glib.h>

G_BEGIN_DECLS

#if !GLIB_CHECK_VERSION(2, 88, 0)
static inline void
g_destroy_notify_to_func (gpointer data, gpointer user_data)
{
  ((GDestroyNotify) user_data) (data);
}

static inline gint
g_compare_func_to_compare_data_func (gconstpointer a, gconstpointer b,
    gpointer user_data)
{
  return ((GCompareFunc) user_data) (a, b);
}

static inline gpointer
g_copy_to_func (gconstpointer src, gpointer user_data)
{
  typedef gpointer (*GCopyFunc1) (gconstpointer);
  return ((GCopyFunc1) user_data) (src);
}


#define GTypeClassInitFunc1 GClassInitFunc
#define GTypeInstanceInitFunc1 GInstanceInitFunc
#define g_type_register_static_simple1 g_type_register_static_simple
#define g_type_add_interface_static1(instance_type, interface_type, iface_init) \
  do { \
    const GInterfaceInfo g_implement_interface_info = { \
      (GInterfaceInitFunc)(void (*)(void))(iface_init), NULL, NULL \
    }; \
    g_type_add_interface_static (instance_type, interface_type, &g_implement_interface_info); \
  } while (0)
#endif

G_END_DECLS

#endif /* __GST_GLIB_COMPAT_H__ */
