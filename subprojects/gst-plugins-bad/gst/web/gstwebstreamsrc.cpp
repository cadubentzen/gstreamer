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
#define DEFAULT_RETRIES 3
#define RETRY_BACKOFF_BASE_MS 500
#define RETRY_BACKOFF_MAX_MS 30000

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_EXTRA_HEADERS,
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
  guint http_status;       /* HTTP response status code, 0 = not received */
  gboolean seekable;       /* server supports Range requests */
  guint retry_count;
  GstStructure *extra_headers;
} GstWebStreamSrc;

G_DECLARE_FINAL_TYPE (
    GstWebStreamSrc, gst_web_stream_src, GST, WEB_STREAM_SRC, GstPushSrc)
G_DEFINE_TYPE_WITH_CODE (GstWebStreamSrc, gst_web_stream_src,
    GST_TYPE_PUSH_SRC,
    G_IMPLEMENT_INTERFACE (
        GST_TYPE_URI_HANDLER, gst_web_stream_src_uri_handler_init));
GST_ELEMENT_REGISTER_DEFINE (web_stream_src, "webstreamsrc",
    GST_RANK_PRIMARY, GST_TYPE_WEB_STREAM_SRC);
GST_DEBUG_CATEGORY_STATIC (gst_web_stream_src_debug);

static gboolean
gst_web_stream_src_is_seekable (GstBaseSrc *bsrc)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (bsrc);
  gboolean ret;

  GST_OBJECT_LOCK (self);
  ret = self->seekable;
  GST_OBJECT_UNLOCK (self);

  return ret;
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
  self->http_status = 0;
  self->seekable = TRUE;
  self->accumulated_data_size = 0;
  self->flushing = FALSE;
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
  GST_OBJECT_LOCK (self);
  self->download_start = segment->start;
  self->download_end =
      GST_CLOCK_TIME_IS_VALID (segment->stop) ? segment->stop : -1;
  self->seek_pending = TRUE;
  GST_OBJECT_UNLOCK (self);

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
gst_web_stream_src_http_error_from_js (
    guintptr thiz, guint32 gen, int status, val vmsg)
{
  GstWebStreamSrc *self = (GstWebStreamSrc *) thiz;
  std::string stds = vmsg.as<std::string> ();
  const char *msg = stds.c_str ();

  GST_OBJECT_LOCK (self);
  if (gen != self->fetch_generation) {
    GST_DEBUG_OBJECT (self,
        "Stale HTTP error (gen %u vs %u), ignoring: %d %s", gen,
        self->fetch_generation, status, msg);
    GST_OBJECT_UNLOCK (self);
    return;
  }

  GST_ERROR_OBJECT (self, "HTTP error %d: %s", status, msg);
  self->http_status = (guint) status;
  g_free (self->fetch_error);
  self->fetch_error = g_strdup_printf ("HTTP %d: %s", status, msg);
  self->fetch_active = FALSE;
  g_cond_signal (&self->qcond);
  GST_OBJECT_UNLOCK (self);
}

static void
gst_web_stream_src_got_headers_from_js (
    guintptr thiz, guint32 gen, int status, int accept_ranges_none)
{
  GstWebStreamSrc *self = (GstWebStreamSrc *) thiz;

  GST_OBJECT_LOCK (self);
  if (gen != self->fetch_generation) {
    GST_OBJECT_UNLOCK (self);
    return;
  }

  if (accept_ranges_none) {
    GST_INFO_OBJECT (self,
        "Server sent Accept-Ranges: none, not seekable");
    self->seekable = FALSE;
  } else if (self->download_start > 0 && status != 206) {
    GST_WARNING_OBJECT (self,
        "Range request returned %d instead of 206, not seekable", status);
    self->seekable = FALSE;
  }

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
  function ("gst_web_stream_src_http_error_from_js",
      &gst_web_stream_src_http_error_from_js);
  function ("gst_web_stream_src_got_headers_from_js",
      &gst_web_stream_src_got_headers_from_js);
}

/* The fetch must run on the main browser thread because the streaming
 * pthread blocks in g_cond_wait after calling this, preventing promise
 * callbacks from firing on that thread's microtask queue.
 *
 * We use EM_JS + emscripten_async_run_in_main_runtime_thread to
 * dispatch the fetch to the main thread without blocking. */

// clang-format off
EM_JS(void, gst_web_stream_src_fetch_on_main, (const char* url, guintptr thiz, guintptr signal_addr, guint32 generation, const char *range, const char *extra_headers_json), {
      const fetchUrl = UTF8ToString (url);
      const signalIdx = signal_addr >> 2;
      const gen = generation;
      const rangeStr = range ? UTF8ToString (range) : null;
      /* The range pointer was allocated with g_strdup for this dispatch.
       * Free it now that we have copied the string into JS. */
      if (range) _free (range);

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

      /* Apply extra headers passed as a JSON string parameter */
      if (extra_headers_json) {
          var hdrs = JSON.parse(UTF8ToString(extra_headers_json));
          _free(extra_headers_json);
          if (!options.headers) options.headers = {};
          Object.assign(options.headers, hdrs);
      }

      // Fetch data using the Streams API
      fetch(fetchUrl, options)
        .then(response => {
             if (!response.ok) {
               Module.gst_web_stream_src_http_error_from_js(thiz, gen,
                   response.status, response.statusText);
               return null;
             }
             /* Report seekability info to C++ */
             const ar = response.headers.get('Accept-Ranges');
             Module.gst_web_stream_src_got_headers_from_js(thiz, gen,
                 response.status, ar === 'none' ? 1 : 0);
             /* Extract total file size from response headers.
              * For 206 Partial Content, Content-Length is the range size,
              * not the total — use Content-Range: bytes X-Y/TOTAL instead. */
             const cr = response.headers.get('Content-Range');
             if (cr) {
               const total = cr.split('/')[1];
               if (total && total !== '*')
                 Module.gst_web_stream_src_set_content_length_from_js(thiz, parseInt(total, 10));
             } else {
               const cl = response.headers.get('Content-Length');
               if (cl)
                 Module.gst_web_stream_src_set_content_length_from_js(thiz, parseInt(cl, 10));
             }
             return response.body;
        })
        .then(async (rs) => {
             if (!rs) return;
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
  self->http_status = 0;
  self->seekable = TRUE;
  self->retry_count = 0;
  self->extra_headers = NULL;
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

  /* Serialize extra headers to a JSON string for the EM_JS function.
   * We pass this as a parameter rather than using Module globals because
   * EM_ASM (proxied from pthread) and EM_JS (async dispatch) may see
   * different Module scopes in the Emscripten runtime. */
  gchar *headers_json = NULL;
  if (self->extra_headers) {
    GString *json = g_string_new ("{");
    gint n = gst_structure_n_fields (self->extra_headers);
    gint written = 0;
    for (gint i = 0; i < n; i++) {
      const gchar *name =
          gst_structure_nth_field_name (self->extra_headers, i);
      const gchar *str_val =
          gst_structure_get_string (self->extra_headers, name);
      if (str_val) {
        if (written > 0)
          g_string_append_c (json, ',');
        g_string_append_c (json, '"');
        /* Header names should not need escaping, but be safe */
        for (const gchar *p = name; *p; p++) {
          if (*p == '"' || *p == '\\')
            g_string_append_c (json, '\\');
          g_string_append_c (json, *p);
        }
        g_string_append (json, "\":\"");
        for (const gchar *p = str_val; *p; p++) {
          if (*p == '"' || *p == '\\')
            g_string_append_c (json, '\\');
          g_string_append_c (json, *p);
        }
        g_string_append_c (json, '"');
        written++;
      }
    }
    g_string_append_c (json, '}');
    headers_json = g_string_free (json, FALSE);
  }

  /* Fire the async JS fetch on the main browser thread.  We must use
   * async dispatch because sync dispatch deadlocks with the GStreamer
   * seek flow (main thread calls unlock() which waits for create() to
   * return, but create() is blocked waiting for the main thread).
   *
   * String lifetime: self->uri lives as long as the element.  range and
   * headers_json are freshly allocated copies — the EM_JS frees them
   * with _free() after converting to JS strings. */
  self->fetch_active = TRUE;
  emscripten_async_run_in_main_runtime_thread (
      EM_FUNC_SIG_VIIIIII,
      gst_web_stream_src_fetch_on_main,
      self->uri, (guintptr) self,
      (guintptr) &self->queue_signal, self->fetch_generation,
      range, headers_json);
}

static GstFlowReturn
gst_web_stream_src_create (GstPushSrc *psrc, GstBuffer **outbuf)
{
  GstWebStreamSrc *self = GST_WEB_STREAM_SRC (psrc);

retry:
  GST_OBJECT_LOCK (self);

  if (G_UNLIKELY (self->flushing)) {
    GST_OBJECT_UNLOCK (self);
    return GST_FLOW_FLUSHING;
  }

  /* If do_seek was called, cancel the current fetch and start a new
   * one at the requested offset — like souphttpsrc. */
  if (self->seek_pending) {
    gboolean was_active = self->fetch_active;
    GST_DEBUG_OBJECT (self,
        "Seek pending (start %" G_GINT64_FORMAT "), restarting fetch",
        self->download_start);
    self->seek_pending = FALSE;
    self->retry_count = 0;
    self->in_eos = FALSE;
    GST_OBJECT_UNLOCK (self);
    if (was_active)
      gst_web_stream_src_reset_fetch (self);
    GST_OBJECT_LOCK (self);
  }

  if (!self->fetch_active && !self->in_eos) {
    /* No active fetch and not at EOS — start a new fetch.
     * Drain any leftover buffers from a previous fetch (e.g. after a
     * seek) BEFORE starting the new fetch.  start_fetch dispatches
     * asynchronously to the main thread and the first chunk callback
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

  /* Lock is already held — fall through to the wait loop */
  while (g_queue_is_empty (self->q) && !self->fetch_error &&
         !self->flushing && !self->in_eos) {
    GST_DEBUG_OBJECT (self, "Queue is empty, wait for a buffer");
    g_cond_wait (&self->qcond, GST_OBJECT_GET_LOCK (self));
  }

  if (self->fetch_error) {
    gchar *err = self->fetch_error;
    /* status == 0 means a network-level error (not an HTTP response) */
    guint status = self->http_status;
    gboolean retryable = (status == 0 || status >= 500);
    self->fetch_error = NULL;
    self->http_status = 0;

    if (retryable && self->retry_count < DEFAULT_RETRIES) {
      gint64 resume_offset = self->download_offset;
      guint attempt = ++self->retry_count;
      GST_OBJECT_UNLOCK (self);

      guint backoff_ms =
          MIN (RETRY_BACKOFF_BASE_MS << (attempt - 1), RETRY_BACKOFF_MAX_MS);
      GST_WARNING_OBJECT (self,
          "Retryable error (%s), attempt %u/%u, backing off %u ms", err,
          attempt, DEFAULT_RETRIES, backoff_ms);
      g_free (err);

      /* Interruptible backoff — wake on flush */
      gint64 deadline =
          g_get_monotonic_time () + (gint64) backoff_ms * 1000;
      GST_OBJECT_LOCK (self);
      while (!self->flushing && g_get_monotonic_time () < deadline)
        g_cond_wait_until (
            &self->qcond, GST_OBJECT_GET_LOCK (self), deadline);
      if (self->flushing) {
        GST_OBJECT_UNLOCK (self);
        return GST_FLOW_FLUSHING;
      }
      GST_OBJECT_UNLOCK (self);

      gst_web_stream_src_reset_fetch (self);
      GST_OBJECT_LOCK (self);
      self->download_start = resume_offset;
      GST_OBJECT_UNLOCK (self);
      goto retry;
    }

    GST_OBJECT_UNLOCK (self);

    if (status == 404) {
      GST_ELEMENT_ERROR (
          self, RESOURCE, NOT_FOUND, ("%s", err), (NULL));
    } else if (status == 401 || status == 403 || status == 407) {
      GST_ELEMENT_ERROR (
          self, RESOURCE, NOT_AUTHORIZED, ("%s", err), (NULL));
    } else {
      GST_ELEMENT_ERROR (
          self, RESOURCE, FAILED, ("%s", err), (NULL));
    }

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
  self->retry_count = 0;
  GST_OBJECT_UNLOCK (self);

  return GST_FLOW_OK;
}

static gboolean
gst_web_stream_src_query (GstBaseSrc *bsrc, GstQuery *query)
{
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_SCHEDULING:{
      GstSchedulingFlags flags;
      gint minsize, maxsize, align;

      if (!GST_BASE_SRC_CLASS (parent_class)->query (bsrc, query))
        return FALSE;

      gst_query_parse_scheduling (query, &flags, &minsize, &maxsize, &align);
      flags = (GstSchedulingFlags) (flags
          | GST_SCHEDULING_FLAG_BANDWIDTH_LIMITED);
      gst_query_set_scheduling (query, flags, minsize, maxsize, align);
      return TRUE;
    }
    default:
      break;
  }

  return GST_BASE_SRC_CLASS (parent_class)->query (bsrc, query);
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
    case PROP_EXTRA_HEADERS:{
      const GstStructure *s = gst_value_get_structure (value);
      GST_OBJECT_LOCK (self);
      if (self->extra_headers)
        gst_structure_free (self->extra_headers);
      self->extra_headers = s ? gst_structure_copy (s) : NULL;
      GST_OBJECT_UNLOCK (self);
      break;
    }
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
    case PROP_EXTRA_HEADERS:
      GST_OBJECT_LOCK (self);
      gst_value_set_structure (value, self->extra_headers);
      GST_OBJECT_UNLOCK (self);
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
  g_cond_clear (&self->qcond);
  g_queue_clear_full (self->q, (GDestroyNotify) gst_buffer_unref);
  g_queue_free (self->q);
  if (self->extra_headers)
    gst_structure_free (self->extra_headers);

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
  basesrc_class->query = gst_web_stream_src_query;

  gst_element_class_add_pad_template (
      element_class, gst_static_pad_template_get (&srcpadtemplate));

  gobject_class->set_property = gst_web_stream_src_set_property;
  gobject_class->get_property = gst_web_stream_src_get_property;
  gobject_class->finalize = gst_web_stream_src_finalize;

  g_object_class_install_property (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "Location", "URI of resource to read",
          PROP_LOCATION_DEFAULT,
          GParamFlags (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_EXTRA_HEADERS,
      g_param_spec_boxed ("extra-headers", "Extra Headers",
          "Extra HTTP request headers as a GstStructure of string values, "
          "e.g. extra-headers,Authorization=\"Bearer token\"",
          GST_TYPE_STRUCTURE,
          GParamFlags (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  gst_element_class_set_static_metadata (element_class,
      "HTTP Client Source using Web Streams API", "Source/Network",
      "Receiver data as a client over a network via HTTP using Web Streams "
      "API",
      "Alexander Slobodeniuk <aslobodeniuk@fluendo.com>");
}
