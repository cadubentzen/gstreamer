/*
 * GStreamer - GStreamer Web Streams HTTP source
 *
 * Copyright 2024 Fluendo S.A.
 *  @author: Alexander Slobodeniuk <aslobodeniuk@fluendo.com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <emscripten/bind.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include <gst/base/gstpushsrc.h>
#include <stdio.h>
#include <string.h>
#include <gst/web/gstwebutils.h>

using namespace emscripten;

static void gst_web_stream_src_uri_handler_init (
    gpointer g_iface);

#define GST_TYPE_WEB_STREAM_SRC (gst_web_stream_src_get_type ())
#define GST_CAT_DEFAULT gst_web_stream_src_debug
#define parent_class gst_web_stream_src_parent_class

#define PROP_LOCATION_DEFAULT NULL

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_MAX
};

typedef struct _GstWebStreamSrc
{
  GstPushSrc element;

  gchar *uri;
  gboolean in_eos;
  gchar *fetch_error;
  GQueue *q;
  guint queue_max_size;
  guint accumulated_data_size;
  GCond qcond;
  gboolean flushing;
  gint32 queue_signal; /* 0 = full (wait), 1 = has space (proceed) */
  gint64 download_start;
  gint64 download_end;
  gint64 download_offset;
  gint64 content_length;
  guint32 fetch_generation;
  gboolean fetch_active;
  gboolean seek_pending;
  gchar *pending_range;
} GstWebStreamSrc;

G_DECLARE_FINAL_TYPE (
    GstWebStreamSrc, gst_web_stream_src, GST, WEB_STREAM_SRC, GstPushSrc)
G_DEFINE_TYPE_WITH_CODE (GstWebStreamSrc, gst_web_stream_src,
    GST_TYPE_PUSH_SRC,
    G_IMPLEMENT_INTERFACE (
        GST_TYPE_URI_HANDLER, gst_web_stream_src_uri_handler_init));
GST_ELEMENT_REGISTER_DEFINE (web_stream_src, "webstreamsrc",
    GST_RANK_SECONDARY, GST_TYPE_WEB_STREAM_SRC);
GST_DEBUG_CATEGORY_STATIC (gst_web_stream_src_debug);

static gboolean
gst_web_stream_src_is_seekable (GstBaseSrc *bsrc)
{
  return TRUE;
}

static gboolean
gst_web_stream_src_get_size (GstBaseSrc *bsrc, guint64 *size)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (bsrc);
  gboolean ret = FALSE;

  GST_OBJECT_LOCK (self);
  if (self->content_length > 0) {
    *size = self->content_length;
    ret = TRUE;
  }
  GST_OBJECT_UNLOCK (self);

  return ret;
}

/* Forward declaration — defined as EM_JS below */
static void gst_web_stream_src_cancel_fetch (guintptr thiz);

/* Invalidate the current fetch and drain the queue. */
static void
gst_web_stream_src_reset_fetch (GstWebStreamSrc *self)
{
  GST_OBJECT_LOCK (self);

  /* Bump generation so JS callbacks become no-ops.  The AbortController
   * for the in-flight fetch is aborted when the next fetch starts (the
   * fetch EM_JS always aborts the previous controller for this element).
   * We don't call cancel_fetch here because it would need to dispatch to
   * the main thread, and an async dispatch races with start_fetch. */
  self->fetch_generation++;

  self->flushing = TRUE;
  g_cond_signal (&self->qcond);
  g_atomic_int_set (&self->queue_signal, 1);
  EM_ASM ({ Atomics.notify (HEAP32, $0 >> 2, 1); }, &self->queue_signal);

  self->in_eos = FALSE;
  self->fetch_active = FALSE;
  g_queue_clear_full (self->q, (GDestroyNotify) gst_buffer_unref);
  g_clear_pointer (&self->fetch_error, g_free);
  self->accumulated_data_size = 0;
  self->flushing = FALSE;
  self->queue_signal = 1;
  GST_OBJECT_UNLOCK (self);
}

/* Like souphttpsrc: do_seek just records the target position.
 * The actual reconnection happens lazily in create(). */
static gboolean
gst_web_stream_src_do_seek (GstBaseSrc *bsrc, GstSegment *segment)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (bsrc);

  GST_DEBUG_OBJECT (self, "do_seek(%" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT
      ")", segment->start, segment->stop);

  if (segment->format != GST_FORMAT_BYTES) {
    GST_ERROR_OBJECT (self, "Only bytes format is supported for seeking");
    return FALSE;
  }

  /* Just record — create() will pick this up */
  self->download_start = segment->start;
  self->download_end =
      GST_CLOCK_TIME_IS_VALID (segment->stop) ? segment->stop : -1;
  self->seek_pending = TRUE;

  return TRUE;
}

static int
gst_web_stream_src_chunk (guintptr thiz, guint32 gen, val chunk)
{
  GstWebStreamSrc *self = (GstWebStreamSrc *) thiz;
  GstBuffer *buffer;
  guint chunk_size;

  enum
  {
    GST_WEB_STREAM_STOP = 0,
    GST_WEB_STREAM_CONTINUE = 1,
    GST_WEB_STREAM_WAIT = 2
  } ret = GST_WEB_STREAM_CONTINUE;

  /* Check generation before allocating to avoid work on stale fetches */
  if (gen != g_atomic_int_get ((gint *) &self->fetch_generation))
    return GST_WEB_STREAM_STOP;

  buffer = gst_web_utils_js_array_to_buffer (chunk);
  chunk_size = gst_buffer_get_size (buffer);

  GST_DEBUG_OBJECT (self, "Received chunk of size: %u (gen %u)", chunk_size,
      gen);

  GST_OBJECT_LOCK (self);
  if (self->flushing || gen != self->fetch_generation) {
    GST_DEBUG_OBJECT (self,
        "Element is flushing or stale fetch (gen %u vs %u), stop fetching",
        gen, self->fetch_generation);
    ret = GST_WEB_STREAM_STOP;
    goto done;
  }

  self->accumulated_data_size += chunk_size;

  GST_BUFFER_OFFSET (buffer) = self->download_offset;
  self->download_offset += chunk_size;
  g_queue_push_tail (self->q, buffer);
  buffer = NULL;

  GST_DEBUG_OBJECT (self,
      "Pushed buffer of size %u to the queue. Now it's of (%u/%u) bytes",
      chunk_size, self->accumulated_data_size, self->queue_max_size);

  /* Apply backpressure: tell the JS read loop to pause if the queue
   * is full.  The JS side will Atomics.waitAsync on queue_signal
   * until the consumer notifies space available. */
  if (self->accumulated_data_size >= self->queue_max_size) {
    g_atomic_int_set (&self->queue_signal, 0);
    ret = GST_WEB_STREAM_WAIT;
  }

done:
  g_cond_signal (&self->qcond);
  GST_OBJECT_UNLOCK (self);
  g_clear_pointer (&buffer, gst_buffer_unref);
  return ret;
}

static void
gst_web_stream_src_eos (guintptr thiz, guint32 gen)
{
  GstWebStreamSrc *self = (GstWebStreamSrc *) thiz;

  GST_DEBUG_OBJECT (self, "EOS (gen %u)", gen);
  GST_OBJECT_LOCK (self);
  if (gen != self->fetch_generation) {
    GST_DEBUG_OBJECT (self, "Stale fetch EOS (gen %u vs %u), ignoring", gen,
        self->fetch_generation);
    GST_OBJECT_UNLOCK (self);
    return;
  }
  self->in_eos = TRUE;
  self->fetch_active = FALSE;
  g_cond_signal (&self->qcond);
  GST_OBJECT_UNLOCK (self);
}

static void
gst_web_stream_src_error (guintptr thiz, guint32 gen, val vmsg)
{
  GstWebStreamSrc *self = (GstWebStreamSrc *) thiz;
  std::string stds = vmsg.as<std::string> ();
  const char *msg = stds.c_str ();

  GST_OBJECT_LOCK (self);
  if (gen != self->fetch_generation) {
    GST_DEBUG_OBJECT (self,
        "Stale fetch error (gen %u vs %u), ignoring: %s", gen,
        self->fetch_generation, msg);
    GST_OBJECT_UNLOCK (self);
    return;
  }

  GST_ERROR_OBJECT (self, "Download failed: %s", msg);
  g_free (self->fetch_error);
  self->fetch_error = g_strdup (msg);
  self->fetch_active = FALSE;
  g_cond_signal (&self->qcond);
  GST_OBJECT_UNLOCK (self);
}

static void
gst_web_stream_src_set_content_length (GstWebStreamSrc *self, gint64 length)
{
  GST_INFO_OBJECT (self, "Content-Length: %" G_GINT64_FORMAT, length);
  GST_OBJECT_LOCK (self);
  self->content_length = length;
  GST_OBJECT_UNLOCK (self);

  GstBaseSrc *basesrc = GST_BASE_SRC (self);
  GST_OBJECT_LOCK (basesrc);
  basesrc->segment.duration = length;
  GST_OBJECT_UNLOCK (basesrc);

  gst_element_post_message (GST_ELEMENT (self),
      gst_message_new_duration_changed (GST_OBJECT (self)));
}

static void
gst_web_stream_src_set_content_length_from_js (guintptr thiz, double length)
{
  GstWebStreamSrc *self = (GstWebStreamSrc *) thiz;
  gst_web_stream_src_set_content_length (self, (gint64) length);
}

EMSCRIPTEN_BINDINGS (gst_web_stream_src)
{
  function ("gst_web_stream_src_error", &gst_web_stream_src_error);
  function ("gst_web_stream_src_eos", &gst_web_stream_src_eos);
  function ("gst_web_stream_src_chunk", &gst_web_stream_src_chunk);
  function ("gst_web_stream_src_set_content_length_from_js",
      &gst_web_stream_src_set_content_length_from_js);
}

// clang-format off
EM_JS(void, gst_web_stream_src_cancel_fetch_js, (guintptr thiz), {
    if (Module._fetchControllers && Module._fetchControllers[thiz]) {
        Module._fetchControllers[thiz].abort();
        delete Module._fetchControllers[thiz];
    }
});
// clang-format on

/* Dispatch cancel to the main thread where _fetchControllers lives.
 * Use async dispatch to avoid deadlocks — cancel_fetch may be called
 * with GST_OBJECT_LOCK held, and the main thread's chunk callback
 * also takes that lock. */
static void
gst_web_stream_src_cancel_fetch (guintptr thiz)
{
  emscripten_async_run_in_main_runtime_thread (
      EM_FUNC_SIG_VI, gst_web_stream_src_cancel_fetch_js, thiz);
}

/* The fetch must run on the main browser thread because the streaming
 * pthread blocks in g_cond_wait after calling this, preventing promise
 * callbacks from firing on that thread's microtask queue.
 *
 * We use EM_JS + emscripten_async_run_in_main_runtime_thread to
 * dispatch the fetch to the main thread without blocking. */

// clang-format off
EM_JS(void, gst_web_stream_src_fetch_on_main, (const char* url, guintptr thiz, guintptr signal_addr, guint32 generation, const char *range), {
      const fetchUrl = UTF8ToString (url);
      const signalIdx = signal_addr >> 2;
      const gen = generation;
      const rangeStr = range ? UTF8ToString (range) : null;

      /* Set up an AbortController so we can cancel this fetch */
      if (!Module._fetchControllers) Module._fetchControllers = {};
      if (Module._fetchControllers[thiz])
          Module._fetchControllers[thiz].abort();
      const abortCtrl = new AbortController();
      Module._fetchControllers[thiz] = abortCtrl;

      var options = { signal: abortCtrl.signal };
      if (rangeStr) {
          options.headers = { 'Range': rangeStr };
      }

      // Fetch data using the Streams API
      fetch(fetchUrl, options)
        .then(response => {
             // Extract Content-Length from response headers
             const cl = response.headers.get('Content-Length');
             if (cl) {
               Module.gst_web_stream_src_set_content_length_from_js(thiz, parseInt(cl, 10));
             }
             return response.body;
        })
        .then(async (rs) => {
             const reader = rs.getReader ();
             try {
                 let result = 1;
                 while (result) {
                     const { done, value } = await reader.read();

                     if (done) {
                         Module.gst_web_stream_src_eos (thiz, gen);
                         break;
                     }

                     result = Module.gst_web_stream_src_chunk(thiz, gen, value);

                     /* If queue is full (WAIT=2), wait for the consumer
                      * to notify space available via Atomics. */
                     if (result === 2) {
                         var w = Atomics.waitAsync(HEAP32, signalIdx, 0);
                         if (w.async)
                             await w.value;
                         result = 1;
                     }
                 }
             } catch (e) {
                 if (e.name !== 'AbortError')
                     Module.gst_web_stream_src_error (thiz, gen, e.toString());
             } finally {
                 reader.releaseLock();
             }
        })
        .catch(fetchError => {
            if (fetchError.name === 'AbortError') return;
            Module.gst_web_stream_src_error (thiz, gen, fetchError.toString());
        });
});
// clang-format on

static GstURIType
gst_web_stream_src_urihandler_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar *const *
gst_web_stream_src_urihandler_get_protocols (GType type)
{
  static const gchar *protocols[] = { "http", "https", NULL };

  return protocols;
}

static gboolean
gst_web_stream_src_urihandler_set_uri (
    GstURIHandler *handler, const gchar *uri, GError **error)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (handler);

  g_return_val_if_fail (GST_IS_URI_HANDLER (handler), FALSE);
  g_return_val_if_fail (uri != NULL, FALSE);

  GST_OBJECT_LOCK (self);

  if (self->uri != NULL) {
    GST_DEBUG_OBJECT (self,
        "URI already present as %s, updating to new URI %s", self->uri, uri);
    g_free (self->uri);
  }

  self->uri = g_strdup (uri);
  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static gchar *
gst_web_stream_src_urihandler_get_uri (GstURIHandler *handler)
{
  gchar *ret;
  GstWebStreamSrc *self;

  g_return_val_if_fail (GST_IS_URI_HANDLER (handler), NULL);
  self = GST_WEB_STREAM_SRC (handler);

  GST_OBJECT_LOCK (self);
  ret = g_strdup (self->uri);
  GST_OBJECT_UNLOCK (self);

  return ret;
}

static void
gst_web_stream_src_uri_handler_init (gpointer g_iface)
{
  GstURIHandlerInterface *uri_iface = (GstURIHandlerInterface *) g_iface;

  uri_iface->get_type = gst_web_stream_src_urihandler_get_type;
  uri_iface->get_protocols = gst_web_stream_src_urihandler_get_protocols;
  uri_iface->get_uri = gst_web_stream_src_urihandler_get_uri;
  uri_iface->set_uri = gst_web_stream_src_urihandler_set_uri;
}

static void
gst_web_stream_src_init (GstWebStreamSrc *self)
{
  g_cond_init (&self->qcond);
  self->q = g_queue_new ();
  self->queue_max_size = 1024 * 1024;

  gst_base_src_set_dynamic_size (GST_BASE_SRC (self), TRUE);
  self->flushing = FALSE;
  self->queue_signal = 1;
  self->content_length = -1;
  self->download_start = 0;
  self->download_end = -1;
  self->download_offset = 0;
}

static void
gst_web_stream_src_start_fetch (GstWebStreamSrc *self)
{
  gchar *range;
  gint64 s = self->download_start, e = self->download_end;

  GST_INFO_OBJECT (self, "Start fetching from %s (offset %" G_GINT64_FORMAT ")",
      self->uri, s);

  if (s != -1 && e != -1) {
     range = g_strdup_printf ("bytes=%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT, s, e);
  } else if (e == -1 && s != -1 && s != 0) {
     range = g_strdup_printf ("bytes=%" G_GINT64_FORMAT "-", s);
  } else {
     range = NULL;
  }

  /* Fire the async JS fetch on the main browser thread.  We must use
   * async dispatch because sync dispatch deadlocks with the GStreamer
   * seek flow (main thread calls unlock() which waits for create() to
   * return, but create() is blocked waiting for the main thread).
   *
   * String lifetime: self->uri lives as long as the element.  range is
   * stored in self->pending_range so it survives until the main thread
   * reads it via UTF8ToString. */
  g_free (self->pending_range);
  self->pending_range = range;    /* takes ownership, freed on next call */
  self->fetch_active = TRUE;
  emscripten_async_run_in_main_runtime_thread (
      EM_FUNC_SIG_VIIIII,
      gst_web_stream_src_fetch_on_main,
      self->uri, (guintptr) self,
      (guintptr) &self->queue_signal, self->fetch_generation,
      self->pending_range);
}

static GstFlowReturn
gst_web_stream_src_create (GstPushSrc *psrc, GstBuffer **outbuf)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (psrc);

  if (G_UNLIKELY (self->flushing))
    return GST_FLOW_FLUSHING;

  /* If do_seek was called, cancel the current fetch and start a new
   * one at the requested offset — like souphttpsrc. */
  if (self->seek_pending) {
    GST_DEBUG_OBJECT (self,
        "Seek pending (start %" G_GINT64_FORMAT "), restarting fetch",
        self->download_start);
    self->seek_pending = FALSE;
    self->in_eos = FALSE;
    if (self->fetch_active)
      gst_web_stream_src_reset_fetch (self);
  }

  if (!self->fetch_active && !self->in_eos) {
    /* No active fetch and not at EOS — start a new fetch.
     * Drain any leftover buffers from a previous fetch (e.g. after a
     * seek) BEFORE starting the new fetch.  start_fetch dispatches
     * synchronously to the main thread and the first chunk callback
     * may fire before it returns — clearing after would lose data. */
    if (!g_queue_is_empty (self->q)) {
      g_queue_clear_full (self->q, (GDestroyNotify) gst_buffer_unref);
      self->accumulated_data_size = 0;
    }
    self->download_offset = self->download_start;
    g_clear_pointer (&self->fetch_error, g_free);
    /* Bump generation so any pending EOS/chunk/error callbacks from
     * the previous fetch are ignored by their generation check. */
    self->fetch_generation++;
    gst_web_stream_src_start_fetch (self);
  }

  GST_OBJECT_LOCK (self);
  while (g_queue_is_empty (self->q) && !self->fetch_error &&
         !self->flushing && !self->in_eos) {
    GST_DEBUG_OBJECT (self, "Queue is empty, wait for a buffer");
    g_cond_wait (&self->qcond, GST_OBJECT_GET_LOCK (self));
  }

  if (self->fetch_error) {
    gchar *err = self->fetch_error;
    self->fetch_error = NULL;
    GST_OBJECT_UNLOCK (self);
    GST_ELEMENT_ERROR (
        self, RESOURCE, FAILED, ("Fetch failed: %s", err), (NULL));
    g_free (err);
    return GST_FLOW_ERROR;
  }

  if (G_UNLIKELY (self->flushing)) {
    GST_OBJECT_UNLOCK (self);
    return GST_FLOW_FLUSHING;
  }

  *outbuf = (GstBuffer *) g_queue_pop_head (self->q);
  if (G_UNLIKELY (*outbuf == NULL)) {
     GstFlowReturn ret = self->in_eos ? GST_FLOW_EOS
	: self->flushing ? GST_FLOW_FLUSHING
	: GST_FLOW_ERROR;
    GST_OBJECT_UNLOCK (self);
    return ret;
  }

  self->accumulated_data_size -= gst_buffer_get_size (*outbuf);

  /* Wake the JS read loop if it was paused due to a full queue */
  if (self->accumulated_data_size < self->queue_max_size &&
      g_atomic_int_get (&self->queue_signal) == 0) {
    g_atomic_int_set (&self->queue_signal, 1);
    EM_ASM ({ Atomics.notify (HEAP32, $0 >> 2, 1); }, &self->queue_signal);
  }

  GST_DEBUG_OBJECT (self, "Buffer of size %" G_GSIZE_FORMAT " ready",
      gst_buffer_get_size (*outbuf));
  GST_OBJECT_UNLOCK (self);

  return GST_FLOW_OK;
}

static GstStateChangeReturn
gst_web_stream_src_change_state (
    GstElement *element, GstStateChange transition)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (self->uri == NULL) {
        GST_ELEMENT_ERROR (
            element, RESOURCE, OPEN_READ, ("No URL set."), ("Missing URL"));
        return GST_STATE_CHANGE_FAILURE;
      }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_web_stream_src_reset_fetch (self);
      break;
    default:
      break;
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_web_stream_src_set_property (
    GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (object);

  switch (prop_id) {
    case PROP_LOCATION:
      gst_web_stream_src_urihandler_set_uri (
          GST_URI_HANDLER (self), g_value_get_string (value), NULL);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_web_stream_src_get_property (
    GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_value_take_string (value,
          gst_web_stream_src_urihandler_get_uri (GST_URI_HANDLER (self)));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_web_stream_src_finalize (GObject *obj)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (obj);

  gst_web_stream_src_reset_fetch (self);
  g_free (self->uri);
  g_free (self->pending_range);
  g_cond_clear (&self->qcond);
  g_queue_clear_full (self->q, (GDestroyNotify) gst_buffer_unref);
  g_queue_free (self->q);

  G_OBJECT_CLASS (gst_web_stream_src_parent_class)->finalize (obj);
}

static gboolean
gst_web_stream_src_unlock (GstBaseSrc *bsrc)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (bsrc);

  GST_DEBUG_OBJECT (self, "Unlock");
  GST_OBJECT_LOCK (self);
  self->flushing = TRUE;
  g_cond_signal (&self->qcond);
  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static gboolean
gst_web_stream_src_unlock_stop (GstBaseSrc *bsrc)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (bsrc);

  GST_DEBUG_OBJECT (self, "Unlock stop");
  GST_OBJECT_LOCK (self);
  self->flushing = FALSE;
  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static void
gst_web_stream_src_class_init (GstWebStreamSrcClass *klass)
{
  static GstStaticPadTemplate srcpadtemplate = GST_STATIC_PAD_TEMPLATE (
      "src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = (GstElementClass *) klass;
  GstBaseSrcClass *basesrc_class = (GstBaseSrcClass *) klass;
  GstPushSrcClass *pushsrc_class = (GstPushSrcClass *) klass;

  GST_DEBUG_CATEGORY_INIT (gst_web_stream_src_debug, "webstreamsrc", 0,
      "HTTP Client Source using Web Streams API");

  element_class->change_state = gst_web_stream_src_change_state;

  pushsrc_class->create = gst_web_stream_src_create;

  basesrc_class->is_seekable = gst_web_stream_src_is_seekable;
  basesrc_class->get_size = gst_web_stream_src_get_size;
  basesrc_class->do_seek = gst_web_stream_src_do_seek;
  basesrc_class->unlock = gst_web_stream_src_unlock;
  basesrc_class->unlock_stop = gst_web_stream_src_unlock_stop;

  gst_element_class_add_pad_template (
      element_class, gst_static_pad_template_get (&srcpadtemplate));

  gobject_class->set_property = gst_web_stream_src_set_property;
  gobject_class->get_property = gst_web_stream_src_get_property;
  gobject_class->finalize = gst_web_stream_src_finalize;

  g_object_class_install_property (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "Location", "URI of resource to read",
          PROP_LOCATION_DEFAULT,
          GParamFlags (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  gst_element_class_set_static_metadata (element_class,
      "HTTP Client Source using Web Streams API", "Source/Network",
      "Receiver data as a client over a network via HTTP using Web Streams "
      "API",
      "Alexander Slobodeniuk <aslobodeniuk@fluendo.com>");
}
