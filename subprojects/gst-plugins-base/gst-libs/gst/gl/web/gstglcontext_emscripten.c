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
#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/em_asm.h>
#include "../gstglcontext_private.h"

#include "gstglcontext_emscripten.h"
#include "gstglwindow_canvas.h"

#define GST_CAT_DEFAULT gst_gl_context_debug

/* FIXME rename this to GstGLContextWebEmscripten */
/* This is not defined in any header, in emscripten it is forward referenced like this */
extern void *emscripten_GetProcAddress (const char *name);

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
  return (guintptr) self->priv->handle;
}

static gboolean
gst_gl_context_emscripten_activate (GstGLContext * context, gboolean activate)
{
  GstGLContextEmscripten *self;
  EMSCRIPTEN_RESULT result;

  self = GST_GL_CONTEXT_EMSCRIPTEN (context);
  GST_DEBUG_OBJECT (context, "Activating context");
  result = emscripten_webgl_make_context_current (self->priv->handle);
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

/* Transfer the DOM canvas to an OffscreenCanvas and send it to the
 * runner thread via postMessage with Transferable.  Returns TRUE if
 * the OffscreenCanvas was successfully received and stored in
 * specialHTMLTargets[selector] on the current worker thread.
 *
 * The mechanism:
 *  1. Register a message handler on the current worker thread that
 *     listens for { gst_cmd: "offscreenCanvas" } messages and stores
 *     the received OffscreenCanvas in specialHTMLTargets.
 *  2. Use MAIN_THREAD_EM_ASM to call transferControlToOffscreen()
 *     on the DOM canvas and postMessage the result to this worker.
 *  3. Yield to the JS event loop via emscripten_sleep(0) so the
 *     worker can process the queued postMessage and fire the handler.
 *  4. Check a shared-memory flag to determine success or failure.
 *
 * The emscripten_sleep(0) call uses ASYNCIFY to unwind the WASM
 * stack, yielding control to the JS event loop.  This allows the
 * addEventListener('message') handler to fire and store the
 * OffscreenCanvas in specialHTMLTargets before we return.
 *
 * This is safe because we are in the NULL->READY state change with
 * no GL calls active and no stream locks held.
 */
static gboolean
_try_transfer_offscreen_canvas (GstGLContext * context, const gchar * selector)
{
  /* Allocate the flag on the heap, not the stack, because
   * emscripten_sleep() uses ASYNCIFY which unwinds the WASM stack.
   * During the unwind, the stack frame is serialized and the physical
   * stack memory may be reused.  The JS message handler's closure
   * captures the flag's address, so it must remain valid while the
   * stack is unwound. */
  volatile int32_t *transfer_flag = g_new0 (int32_t, 1);
  gboolean result = FALSE;
  gboolean supported;
  int retries;

  GST_DEBUG_OBJECT (context,
      "Attempting OffscreenCanvas transfer for '%s'", selector);

  /* Step 1: Check browser environment and OffscreenCanvas support.
   * In Node.js there is no document and no transferControlToOffscreen. */
  /* *INDENT-OFF* */
  supported = (gboolean) EM_ASM_INT ({
    return (typeof document !== 'undefined' &&
            typeof OffscreenCanvas !== 'undefined') ? 1 : 0;
  });
  /* *INDENT-ON* */

  if (!supported) {
    GST_DEBUG_OBJECT (context,
        "OffscreenCanvas not available (Node.js or unsupported browser)");
    g_free ((gpointer) transfer_flag);
    return FALSE;
  }

  /* Step 2: Register a message handler on this worker thread that will
   * receive the OffscreenCanvas and store it in specialHTMLTargets.
   * The handler sets *transfer_flag to 1 on success or -1 on failure. */
  /* *INDENT-OFF* */
  EM_ASM ({
    var selector = UTF8ToString ($0);
    var flagPtr = $1;

    Module['_gst_offscreen_canvas_handler'] = function (e) {
      var msgData = e.data;
      if (msgData && msgData['gst_cmd'] === 'offscreenCanvas' &&
          msgData['selector'] === selector) {
        var offscreen = msgData['offscreenCanvas'];
        if (offscreen) {
          /* Store in specialHTMLTargets so emscripten_webgl_create_context
           * can find it via findCanvasEventTarget / findEventTarget */
          specialHTMLTargets[selector] = offscreen;
          Atomics.store (HEAP32, flagPtr >> 2, 1);
        } else {
          Atomics.store (HEAP32, flagPtr >> 2, -1);
        }

        /* Remove ourselves after handling */
        removeEventListener ('message',
            Module['_gst_offscreen_canvas_handler']);
        Module['_gst_offscreen_canvas_handler'] = null;
      }
    };

    addEventListener ('message', Module['_gst_offscreen_canvas_handler']);
  }, selector, (int32_t *) transfer_flag);
  /* *INDENT-ON* */

  /* Step 3: From the browser main thread, find the canvas, call
   * transferControlToOffscreen(), and postMessage it to our worker.
   * For error cases (canvas not found, worker not found), the main
   * thread sets the flag directly since no postMessage is needed. */
  /* *INDENT-OFF* */
  MAIN_THREAD_EM_ASM ({
    var selector = UTF8ToString ($0);
    var threadId = $1;
    var flagPtr = $2;
    var canvas = document.querySelector (selector);

    if (!canvas || typeof canvas.transferControlToOffscreen !== 'function') {
      /* Signal failure -- canvas not found or API unavailable */
      Atomics.store (HEAP32, flagPtr >> 2, -1);
      return;
    }

    var offscreen = canvas.transferControlToOffscreen ();
    var worker = PThread.pthreads[threadId];
    if (!worker) {
      Atomics.store (HEAP32, flagPtr >> 2, -1);
      return;
    }

    worker.postMessage ({
      gst_cmd: 'offscreenCanvas',
      selector: selector,
      offscreenCanvas: offscreen
    }, [offscreen]);
  }, selector, pthread_self (), (int32_t *) transfer_flag);
  /* *INDENT-ON* */

  /* Step 4: Yield to the JS event loop so the worker can process the
   * queued postMessage.  emscripten_sleep(0) uses ASYNCIFY to unwind
   * the WASM stack, giving the JS event loop a chance to deliver the
   * message and fire our addEventListener handler.  We retry a few
   * times in case the message delivery is delayed. */
  for (retries = 0; retries < 10 && *transfer_flag == 0; retries++) {
    emscripten_sleep (0);
  }

  if (*transfer_flag == 1) {
    GST_INFO_OBJECT (context,
        "OffscreenCanvas transferred successfully for '%s'", selector);
    result = TRUE;
    goto done;
  }

  if (*transfer_flag == 0) {
    GST_WARNING_OBJECT (context,
        "OffscreenCanvas transfer timed out for '%s'", selector);
  } else {
    GST_WARNING_OBJECT (context,
        "OffscreenCanvas transfer failed for '%s' (flag=%d)",
        selector, (int) *transfer_flag);
  }

  /* Clean up the message handler if still registered */
  /* *INDENT-OFF* */
  EM_ASM ({
    if (Module['_gst_offscreen_canvas_handler']) {
      removeEventListener ('message',
          Module['_gst_offscreen_canvas_handler']);
      Module['_gst_offscreen_canvas_handler'] = null;
    }
  });
  /* *INDENT-ON* */

done:
  g_free ((gpointer) transfer_flag);
  return result;
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
        GST_GL_CONTEXT_ERROR_WRONG_CONFIG, "Shared contexts are not allowed");
    return FALSE;
  }

  display = gst_gl_context_get_display (context);
  canvas = (gchar *) gst_gl_display_get_handle (display);

  /* Check if a WebRunner GMainContext was attached to the display.
   * If so, we're running on the runner's thread — try to create the GL
   * context locally with an OffscreenCanvas for zero-copy rendering. */
  runner_ctx = g_object_get_qdata (G_OBJECT (display),
      _gl_runner_context_quark ());

  emscripten_webgl_init_context_attributes (&attrs);
  attrs.majorVersion = 2;
  attrs.alpha = EM_FALSE;

  if (runner_ctx && _try_transfer_offscreen_canvas (context, canvas)) {
    /* OffscreenCanvas path: the canvas has been transferred to this
     * worker thread and stored in specialHTMLTargets[selector].
     * Create the WebGL context locally (no proxy) so GL calls execute
     * directly on the runner thread.  This enables zero-copy
     * texImage2D with WebCodecs VideoFrame. */
    GST_DEBUG_OBJECT (context,
        "Creating local WebGL context on runner thread (OffscreenCanvas)");
    attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_DISALLOW;
    attrs.explicitSwapControl = EM_FALSE;
    attrs.renderViaOffscreenBackBuffer = EM_FALSE;
    self->priv->handle = emscripten_webgl_create_context (canvas, &attrs);

    if (!self->priv->handle) {
      GST_WARNING_OBJECT (context,
          "Local context creation failed, falling back to PROXY_ALWAYS");
      goto proxy_always;
    }
  } else {
  proxy_always:
    /* Fallback: PROXY_ALWAYS path.  Used when:
     *  - No runner context is set (dedicated GL thread)
     *  - Running in Node.js (no OffscreenCanvas)
     *  - OffscreenCanvas transfer failed
     * GL calls are proxied to the browser main thread where the
     * DOM canvas lives. */
    GST_DEBUG_OBJECT (context,
        "Creating Emscripten WebGL context with PROXY_ALWAYS on %s",
        (gchar *) canvas);
    attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;
    attrs.explicitSwapControl = EM_TRUE;
    attrs.renderViaOffscreenBackBuffer = EM_TRUE;
    self->priv->handle = emscripten_webgl_create_context (canvas, &attrs);
  }

  gst_object_unref (display);

  if (!self->priv->handle) {
    g_set_error (error, GST_GL_CONTEXT_ERROR,
        GST_GL_CONTEXT_ERROR_CREATE_CONTEXT,
        "Failed to create Emscripten WebGL context on '%s'.", (gchar *) canvas);
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

static void
gst_gl_context_emscripten_swap_buffers (GstGLContext * context)
{
  /* Two paths:
   *
   * Local OffscreenCanvas (PROXY_DISALLOW, explicitSwapControl=false):
   *   The browser auto-composites the OffscreenCanvas to the visible canvas
   *   when the worker thread's current task completes and control returns to
   *   the JS event loop.  This is spec-defined behavior for OffscreenCanvas
   *   obtained via transferControlToOffscreen(), but means frame presentation
   *   is asynchronous — the drawn content appears only after the worker yields.
   *   emscripten_webgl_commit_frame() is a no-op in this configuration.
   *
   * PROXY_ALWAYS (explicitSwapControl=true):
   *   emscripten_webgl_commit_frame() blits the offscreen FBO to the visible
   *   DOM canvas immediately during the proxied call on the main thread. */
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
  return (guintptr) emscripten_webgl_get_current_context ();
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
      GstGLWindowCanvas *canvas_window = GST_GL_WINDOW_CANVAS (context->window);
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
