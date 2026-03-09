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

static gboolean
gst_gl_context_emscripten_create_context (GstGLContext * context,
    GstGLAPI gl_api, GstGLContext * other_context, GError ** error)
{
  GstGLContextEmscripten *self;
  GstGLDisplay *display = NULL;
  EmscriptenWebGLContextAttributes attrs;
  gchar *canvas;

  self = GST_GL_CONTEXT_EMSCRIPTEN (context);

  if (other_context) {
    g_set_error (error, GST_GL_CONTEXT_ERROR,
        GST_GL_CONTEXT_ERROR_WRONG_CONFIG,
        "Shared contexts are not allowed");
    return FALSE;
  }

  display = gst_gl_context_get_display (context);
  canvas = (gchar *) gst_gl_display_get_handle (display);
 
  emscripten_webgl_init_context_attributes (&attrs);
  attrs.majorVersion = 2;
  attrs.alpha = EM_FALSE;
  attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;
  attrs.explicitSwapControl = EM_TRUE;
  attrs.renderViaOffscreenBackBuffer = EM_TRUE;
  /* comma-delimited list with # */
  GST_DEBUG_OBJECT (context, "Creating Emscripten WebGL context on %s", (gchar *)canvas);
  self->priv->handle = emscripten_webgl_create_context (canvas, &attrs);

  /* Work around Emscripten bug: with PROXY_ALWAYS, the proxied GL context
   * activation sets GL.currentContext to the raw WebGL context instead of
   * the Emscripten wrapper object. This breaks glBindFramebuffer's FBO
   * interception (GL.currentContext.defaultFbo is undefined, so FBO 0
   * binds to the canvas instead of the offscreen FBO).
   *
   * Fix by hooking gl.bindFramebuffer to manually redirect FBO null
   * to the offscreen FBO. */
  MAIN_THREAD_EM_ASM({
    var keys = Object.keys(GL.contexts);
    for (var i = 0; i < keys.length; i++) {
      var ctx = GL.contexts[keys[i]];
      if (ctx && ctx.defaultFbo && ctx.GLctx && !ctx._fboPatched) {
        var gl = ctx.GLctx;
        var origBind = gl.bindFramebuffer.bind(gl);
        var defaultFbo = ctx.defaultFbo;
        gl.bindFramebuffer = function(target, fb) {
          /* Only redirect 'undefined' (broken proxy interception result)
           * to the offscreen FBO. Leave 'null' alone since the blit
           * function uses null intentionally to bind the real canvas. */
          if (fb === undefined) {
            fb = defaultFbo;
          }
          return origBind(target, fb);
        };
        ctx._fboPatched = true;
      }
    }
  });

  gst_object_unref (display);

  if (!self->priv->handle) {
    g_set_error (error, GST_GL_CONTEXT_ERROR,
        GST_GL_CONTEXT_ERROR_CREATE_CONTEXT,
        "Failed to create Emscripten WebGL context on '%s'. "
        "In Node.js, install the 'gl' npm package for headless GL support.",
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
  /* With PROXY_ALWAYS, GL calls are proxied to the main thread which
   * renders to an offscreen FBO. However, the Emscripten proxied context
   * activation incorrectly sets GL.currentContext to the raw WebGL context
   * instead of the wrapper object, so GL.currentContext.defaultFbo is
   * undefined and the FBO interception in glBindFramebuffer doesn't work.
   *
   * Work around this by looking up the correct context wrapper from
   * GL.contexts and using it to blit the offscreen FBO to the canvas. */
  MAIN_THREAD_EM_ASM({
    var keys = Object.keys(GL.contexts);
    for (var i = 0; i < keys.length; i++) {
      var ctx = GL.contexts[keys[i]];
      if (ctx && ctx.defaultFbo) {
        GL.currentContext = ctx;
        Module["ctx"] = GLctx = ctx.GLctx;
        GL.blitOffscreenFramebuffer(ctx);
        break;
      }
    }
  });
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
  GThread *thread;
  gchar *canvas;

  display = gst_gl_context_get_display (context);
  canvas = (gchar *) gst_gl_display_get_handle (display);

  GST_DEBUG_OBJECT (context, "Creating GL thread (canvas %s stays on main thread for proxied rendering)", canvas);
  /* Don't transfer the canvas to the worker. With PROXY_ALWAYS, GL calls
   * are proxied to the main thread where the canvas lives. This avoids
   * OffscreenCanvas compositing issues (the main thread event loop handles
   * canvas presentation naturally). */
  thread = g_thread_emscripten_new (name, NULL, run, context);
  GST_DEBUG_OBJECT (context, "Thread created");

  gst_object_unref (display);
  return thread;
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
