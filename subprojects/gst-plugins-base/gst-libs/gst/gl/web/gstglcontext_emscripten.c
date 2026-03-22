/*
 * GStreamer
 * Copyright (C) 2023 Jorge Zapata <jzapata@fluendo.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/gl/gl.h>
#include <emscripten/threading.h>
#include <emscripten/em_asm.h>
#include "../gstglcontext_private.h"

#include "gstglcontext_emscripten.h"
#include "gstglwindow_canvas.h"

#define GST_CAT_DEFAULT gst_gl_context_debug

/* FIXME rename this to GstGLContextWebEmscripten */
/* This is not defined in any header, in emscripten it is forward referenced like this */
extern void* emscripten_GetProcAddress(const char *name);

struct _GstGLContextEmscriptenPrivate
{
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle;
};

#define gst_gl_context_emscripten_parent_class parent_class
G_DEFINE_TYPE_WITH_PRIVATE (GstGLContextEmscripten, gst_gl_context_emscripten,
    GST_TYPE_GL_CONTEXT);


static guintptr
gst_gl_context_emscripten_get_gl_context (GstGLContext * context)
{
  GstGLContextEmscripten *self;

  self = GST_GL_CONTEXT_EMSCRIPTEN (context);
  return (guintptr)self->priv->handle;
}

static gboolean
gst_gl_context_emscripten_activate (GstGLContext * context, gboolean activate)
{
  GstGLContextEmscripten *self;
  EMSCRIPTEN_RESULT result;

  self = GST_GL_CONTEXT_EMSCRIPTEN (context);
  GST_DEBUG_OBJECT (context, "Activating context");
  result = emscripten_webgl_make_context_current(self->priv->handle);
  if (!result) {
    GST_DEBUG_OBJECT (context, "Context activated");
    return TRUE;
  } else {
    GST_WARNING_OBJECT (context, "Context activation failed (%d)", result);
    return FALSE;
  }
}

static GQuark
_gl_runner_context_quark (void)
{
  static GQuark quark = 0;
  if (G_UNLIKELY (quark == 0))
    quark = g_quark_from_static_string ("gst.gl.runner.main-context");
  return quark;
}

static gboolean
gst_gl_context_emscripten_create_context (GstGLContext * context,
    GstGLAPI gl_api, GstGLContext * other_context, GError ** error)
{
  GstGLContextEmscripten *self;
  GstGLDisplay *display = NULL;
  EmscriptenWebGLContextAttributes attrs;
  gchar *canvas;
  GMainContext *runner_ctx;

  self = GST_GL_CONTEXT_EMSCRIPTEN (context);

  if (other_context) {
    g_set_error (error, GST_GL_CONTEXT_ERROR,
        GST_GL_CONTEXT_ERROR_WRONG_CONFIG,
        "Shared contexts are not allowed");
    return FALSE;
  }

  display = gst_gl_context_get_display (context);
  canvas = (gchar *) gst_gl_display_get_handle (display);

  /* Check if a WebRunner GMainContext was attached to the display.
   * If so, we're running on the runner's thread — create the GL
   * context locally without PROXY_ALWAYS for zero-copy rendering. */
  runner_ctx = g_object_get_qdata (G_OBJECT (display),
      _gl_runner_context_quark ());

  emscripten_webgl_init_context_attributes (&attrs);
  attrs.majorVersion = 2;
  attrs.alpha = EM_FALSE;

  if (runner_ctx) {
    /* Runner thread path: the GL context will run on the same thread
     * as the WebRunner (shared context mode).  Still use PROXY_ALWAYS
     * so GL calls are proxied to the main thread where #canvas lives.
     * The benefit is not proxy avoidance but thread merging — the GL
     * window loop runs on the runner thread, not a separate one. */
    GST_DEBUG_OBJECT (context,
        "Creating WebGL context with shared runner thread");
    attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;
    attrs.explicitSwapControl = EM_TRUE;
    attrs.renderViaOffscreenBackBuffer = EM_TRUE;
    self->priv->handle = emscripten_webgl_create_context (canvas, &attrs);
  } else {
    /* Fallback: dedicated GL thread with PROXY_ALWAYS */
    attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;
    attrs.explicitSwapControl = EM_TRUE;
    attrs.renderViaOffscreenBackBuffer = EM_TRUE;

    GST_DEBUG_OBJECT (context, "Creating Emscripten WebGL context on %s",
        (gchar *)canvas);
    self->priv->handle = emscripten_webgl_create_context (canvas, &attrs);
  }

  gst_object_unref (display);

  if (!self->priv->handle) {
    g_set_error (error, GST_GL_CONTEXT_ERROR,
        GST_GL_CONTEXT_ERROR_CREATE_CONTEXT,
        "Failed to create Emscripten WebGL context on '%s'.",
        (gchar *) canvas);
    return FALSE;
  }

  return TRUE;
}

static void
gst_gl_context_emscripten_destroy_context (GstGLContext * context)
{
  GstGLContextEmscripten *self;

  self = GST_GL_CONTEXT_EMSCRIPTEN (context);
  emscripten_webgl_destroy_context (self->priv->handle);
  self->priv->handle = 0;
}

static void gst_gl_context_emscripten_swap_buffers (GstGLContext * context)
{
  /* When using a local context on the runner thread (no PROXY_ALWAYS),
   * the OffscreenCanvas auto-composites to the visible canvas.
   * When using PROXY_ALWAYS with explicitSwapControl, commit_frame
   * blits the offscreen FBO. */
  emscripten_webgl_commit_frame ();
}

static GstGLAPI
gst_gl_context_emscripten_get_gl_api (GstGLContext * context)
{
  return GST_GL_API_GLES2;
}

static GstGLPlatform
gst_gl_context_emscripten_get_gl_platform (GstGLContext * context)
{
  return GST_GL_PLATFORM_EMSCRIPTEN;
}

static gpointer
gst_gl_context_emscripten_get_proc_address (GstGLAPI gl_api, const gchar * name)
{
  gpointer result;

  if (!(result = gst_gl_context_default_get_proc_address (gl_api, name))) {
    result = emscripten_GetProcAddress (name);
  }

  if (!result) {
    GST_ERROR ("Failed to get proc address for '%s'", name);
  }

  return result;
}

static guintptr
gst_gl_context_emscripten_get_current_context (void)
{
  return (guintptr) emscripten_webgl_get_current_context();
}

static GThread *
gst_gl_context_emscripten_create_thread (GstGLContext * context,
    const gchar * name, GThreadFunc run)
{
  GstGLDisplay *display;
  GMainContext *runner_ctx;

  display = gst_gl_context_get_display (context);
  runner_ctx = g_object_get_qdata (G_OBJECT (display),
      _gl_runner_context_quark ());

  if (runner_ctx) {
    GST_DEBUG_OBJECT (context,
        "Running GL context creation on current thread (runner thread)");

    /* Replace the GL window's GMainContext with the runner's so that
     * gst_gl_window_send_message dispatches to the runner's loop. */
    if (context->window && context->window->main_context) {
      GstGLWindowCanvas *canvas_window =
          GST_GL_WINDOW_CANVAS (context->window);
      g_main_context_unref (context->window->main_context);
      context->window->main_context = g_main_context_ref (runner_ctx);
      canvas_window->shared_context = TRUE;

      /* Mark for gst_gl_context_create_thread to skip cleanup */
      g_object_set_data (G_OBJECT (context->window),
          "shared-context", GINT_TO_POINTER (TRUE));
    }

    /* Push the runner context as thread-default */
    g_main_context_push_thread_default (runner_ctx);

    /* Run gst_gl_context_create_thread directly on this thread.
     * It will: create_context (local, no PROXY_ALWAYS), activate,
     * fire _unlock_create_thread, call run() which drains and returns,
     * then skip cleanup (shared-context flag). */
    run (context);

    g_main_context_pop_thread_default (runner_ctx);

    gst_object_unref (display);
    return g_thread_self ();
  }

  /* Fallback: spawn a new GL thread (original behavior) */
  {
    GThread *thread;
    GST_DEBUG_OBJECT (context, "Creating dedicated GL thread");
    thread = g_thread_emscripten_new (name, NULL, run, context);
    gst_object_unref (display);
    return thread;
  }
}

static void
gst_gl_context_emscripten_init (GstGLContextEmscripten * self)
{
  self->priv = gst_gl_context_emscripten_get_instance_private (self);
}

static void
gst_gl_context_emscripten_class_init (GstGLContextEmscriptenClass * klass)
{
  GstGLContextClass *context_class = (GstGLContextClass *) klass;

  context_class->get_gl_context =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_get_gl_context);
  context_class->activate =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_activate);
  context_class->create_context =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_create_context);
  context_class->destroy_context =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_destroy_context);
  context_class->swap_buffers =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_swap_buffers);

  context_class->get_gl_api =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_get_gl_api);
  context_class->get_gl_platform =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_get_gl_platform);
  context_class->get_proc_address =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_get_proc_address);
  context_class->get_current_context =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_get_current_context);
  context_class->create_thread =
      GST_DEBUG_FUNCPTR (gst_gl_context_emscripten_create_thread);
}

GstGLContextEmscripten *
gst_gl_context_emscripten_new (GstGLDisplay * display)
{
  GstGLContextEmscripten *context;

  if ((gst_gl_display_get_handle_type (display) & GST_GL_DISPLAY_TYPE_WEB) == 0) {
    GST_ERROR_OBJECT (context, "Emscripten context requires a Web Display");
    return NULL;
  }

  context = g_object_new (GST_TYPE_GL_CONTEXT_EMSCRIPTEN, NULL);
  gst_object_ref_sink (context);

  return context;
}
