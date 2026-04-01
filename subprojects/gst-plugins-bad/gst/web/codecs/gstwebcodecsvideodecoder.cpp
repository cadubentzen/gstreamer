/*
 * GStreamer - gst.wasm WebCodecsVideoDecoder source
 *
 * Copyright 2024 Fluendo S.A.
 * @author: Jorge Zapata <jzapata@fluendo.com>
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

/*
 * Some ideas:
 * Implement the GST_QUERY_ALLOCATOR (propose_allocation) to
 * use buffers that can be sent directly to the decoder (JS)
 * without having to copy the data
 *
 * Implement the negotiate_pool to ask for specific downstream
 * buffers, i.e GL
 *
 * Some interesting links:
 * https://github.com/emscripten-core/emscripten/issues/3515
 * https://emscripten.org/docs/api_reference/val.h.html
 * https://github.com/emscripten-core/emscripten/issues/4927
 * https://github.com/emscripten-core/emscripten/issues/7025
 * https://github.com/w3c/webcodecs/issues/88
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/gl/gl.h>
#include <gst/pbutils/pbutils.h>
#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/bind.h>
#include <pthread.h>
#include <gst/web/gstwebutils.h>
#include <gst/web/gstwebvideoframe.h>

#include "gstwebcodecs.h"
#include "gstwebcodecsvideodecoder.h"

using namespace emscripten;

#define GST_WEB_CODECS_VIDEO_DECODER_MAX_DEQUEUE 32

#define GST_CAT_DEFAULT gst_web_codecs_video_decoder_debug_category
GST_DEBUG_CATEGORY_STATIC (gst_web_codecs_video_decoder_debug_category);

static gpointer parent_class = NULL;

typedef struct _GstWebCodecsVideoDecoderConfigureData
{
  GstWebCodecsVideoDecoder *self;
  GstVideoCodecState *state;
  gboolean ret;
} GstWebCodecsVideoDecoderConfigureData;

typedef struct _GstWebCodecsVideoDecoderDecodeData
{
  GstWebCodecsVideoDecoder *self;
  GstVideoCodecFrame *frame;
} GstWebCodecsVideoDecoderDecodeData;

#if 0
static void
gst_web_codecs_video_decoder_video_frame_to_codec_frame (
    GstWebCodecsVideoDecoder *self, val video_frame, GstVideoCodecFrame *frame)
{
  GstMapInfo map;
  val buffer_data;
  val options;

  GST_DEBUG_OBJECT (self, "Converting frame to C memory");
  /* destination buffer */
  /* map the buffer into an ArrayBuffer */
  gst_buffer_map (frame->output_buffer, &map, GST_MAP_WRITE);
  buffer_data = val (typed_memory_view (map.size, map.data));
  /* set the options */
  options = val::object ();
  /* Get the format from the output format */
  /* FIXME if the format is not of any rgb type, it complains and we must not
   * set it Also, we need to specify properly the layout of the planes. Check
   * allocationSize()
   */
  options.set ("format", std::string (self->output_format));
  video_frame.call<val> ("copyTo", buffer_data, options).await ();
  gst_buffer_unmap (frame->output_buffer, &map);
  GST_DEBUG_OBJECT (self, "Converted frame to C memory");
}
#endif

static gboolean
gst_web_codecs_video_decoder_get_format (
    GstWebCodecsVideoDecoder *self, val video_frame)
{
  GstVideoFormat format;
  gboolean ret = FALSE;
  const gchar *vf_format;
  gint width;
  gint height;

  /* Get the video frame format */
  vf_format = video_frame["format"].as<std::string> ().c_str ();
  if (!g_strcmp0 (vf_format, "I420")) {
    format = GST_VIDEO_FORMAT_I420;
  } else if (!g_strcmp0 (vf_format, "I420A")) {
    format = GST_VIDEO_FORMAT_A420;
  } else if (!g_strcmp0 (vf_format, "I422")) {
    format = GST_VIDEO_FORMAT_Y42B;
  } else if (!g_strcmp0 (vf_format, "I444")) {
    format = GST_VIDEO_FORMAT_Y444;
  } else if (!g_strcmp0 (vf_format, "NV12")) {
    format = GST_VIDEO_FORMAT_NV12;
  } else if (!g_strcmp0 (vf_format, "RGBA")) {
    format = GST_VIDEO_FORMAT_RGBA;
  } else if (!g_strcmp0 (vf_format, "RGBX")) {
    format = GST_VIDEO_FORMAT_RGBx;
  } else if (!g_strcmp0 (vf_format, "BGRA")) {
    format = GST_VIDEO_FORMAT_BGRA;
  } else if (!g_strcmp0 (vf_format, "BGRX")) {
    format = GST_VIDEO_FORMAT_BGRx;
  } else {
    GST_ERROR_OBJECT (self, "Unsupported format %s", vf_format);
    goto done;
  }
  self->output_format = g_strdup (vf_format);
  width = video_frame["displayWidth"].as<int> ();
  height = video_frame["displayHeight"].as<int> ();

  GST_DEBUG_OBJECT (self, "Negotiating with width: %d, height: %d, format: %s",
      width, height, self->output_format);

  self->format = format;
  self->width = width;
  self->height = height;

  ret = gst_video_decoder_negotiate (GST_VIDEO_DECODER (self));

done:
  return ret;
}

static void
_gl_delete_texture (gpointer data)
{
  GLuint tex_id = GPOINTER_TO_UINT (data);
  glDeleteTextures (1, &tex_id);
}

static void
gst_web_codecs_video_decoder_on_output (guintptr self_, val video_frame)
{
  GstWebCodecsVideoDecoder *self = (GstWebCodecsVideoDecoder *) self_;
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  GstVideoCodecFrame *frame;
  GstFlowReturn flow;
  val vf_timestamp;

  GST_VIDEO_DECODER_STREAM_LOCK (self);
  frame = gst_video_decoder_get_oldest_frame (GST_VIDEO_DECODER (self));
  if (!frame) {
    GST_WARNING_OBJECT (self, "get_oldest_frame returned NULL");
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    video_frame.call<void> ("close");
    return;
  }
  GST_DEBUG_OBJECT (self,
      "queued frame %" GST_TIME_FORMAT " decoded frame %" GST_TIME_FORMAT,
      GST_TIME_ARGS (frame->pts),
      GST_TIME_ARGS (GST_MSECOND * video_frame["timestamp"].as<int> ()));

  /* Configure the output */
  if (!self->output_state) {
    self->need_negotiation = TRUE;
    gst_web_codecs_video_decoder_get_format (self, video_frame);
  }
  /* FIXME If we want to push video_frames directly or glTextures, we need
   * to create the buffers ourselves
   */
#if 0
  flow = gst_video_decoder_allocate_output_frame (dec, frame);

  GST_ERROR_OBJECT (self, "Output caps are %" GST_PTR_FORMAT,
      self->output_state->allocation_caps);

  if (flow != GST_FLOW_OK) {
    GST_ERROR_OBJECT (self, "Flow error: %d", flow);
    goto done;
  }

  /* In a software based pipeline we need to extract the video data
   * in the format the sink element requires
   */
  gst_web_codecs_video_decoder_video_frame_to_codec_frame (self, video_frame,
      frame);
#endif
  /* Create the output buffer — either GL texture or WebVideoFrame */
  if (self->output_gl && self->gl_context) {
    GstBuffer *b;
    GstGLBaseMemoryAllocator *gl_alloc;
    GstGLVideoAllocationParams *params;
    GstGLMemory *gl_mem;
    GLuint tex_id = 0;

    /* Make GL context current, generate texture, upload VideoFrame */
    gst_gl_context_activate (self->gl_context, TRUE);
    glGenTextures (1, &tex_id);
    glBindTexture (GL_TEXTURE_2D, tex_id);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    /* Upload VideoFrame to GL texture.
     *
     * Two paths depending on whether GLctx is a real WebGLRenderingContext
     * on this thread (OffscreenCanvas / runner mode) or a proxied handle
     * (PROXY_ALWAYS, e.g. Node.js headless).
     *
     * Zero-copy path: texImage2D(videoFrame) lets the browser transfer a
     * GPU-resident VideoFrame directly to the WebGL texture without any
     * CPU readback.
     *
     * Fallback path: copyTo() extracts pixels into the WASM heap on the
     * runner thread, then the proxied glTexImage2D uploads from there. */
    {
      gboolean gl_is_local = (gboolean) EM_ASM_INT ({
        return GL.currentContextIsProxied ? 0 : 1;
      });

      if (gl_is_local) {
        /* Zero-copy path: local GL context on the runner thread. */
        EM_ASM ({
          var vf = Emval.toValue ($0);
          GLctx.texImage2D (GLctx.TEXTURE_2D, 0, GLctx.RGBA,
              GLctx.RGBA, GLctx.UNSIGNED_BYTE, vf);
          vf.close ();
        }, video_frame.as_handle ());
      } else {
        /* Fallback: copyTo + glTexImage2D for PROXY_ALWAYS (Node.js, etc.) */
        val options = val::object ();
        options.set ("format", std::string ("RGBA"));

        int w = video_frame["displayWidth"].as<int> ();
        int h = video_frame["displayHeight"].as<int> ();
        int alloc_size = w * h * 4;
        guint8 *pixels = (guint8 *) g_malloc (alloc_size);

        val dest = val::take_ownership ((EM_VAL) EM_ASM_PTR ({
          return Emval.toHandle (HEAPU8.subarray ($0, $1));
        }, (int) (uintptr_t) pixels,
           (int) ((uintptr_t) pixels + alloc_size)));
        video_frame.call<val> ("copyTo", dest, options).await ();
        video_frame.call<void> ("close");

        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        g_free (pixels);
      }
    }

    glBindTexture (GL_TEXTURE_2D, 0);

    /* Wrap the texture as GstGLMemory */
    gl_alloc = GST_GL_BASE_MEMORY_ALLOCATOR (
        gst_gl_memory_allocator_get_default (self->gl_context));
    params = gst_gl_video_allocation_params_new_wrapped_texture (
        self->gl_context, NULL, &self->output_state->info,
        0, NULL, GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA,
        tex_id, GUINT_TO_POINTER (tex_id), _gl_delete_texture);

    gl_mem = (GstGLMemory *) gst_gl_base_memory_alloc (gl_alloc,
        (GstGLAllocationParams *) params);
    gst_gl_allocation_params_free ((GstGLAllocationParams *) params);
    gst_object_unref (gl_alloc);

    b = gst_buffer_new ();
    gst_buffer_append_memory (b, GST_MEMORY_CAST (gl_mem));
    frame->output_buffer = b;

    gst_gl_context_activate (self->gl_context, FALSE);
  } else {
    GstBuffer *b;
    GstWebRunner *runner;
    GstWebVideoFrame *memory;

    runner = gst_web_canvas_get_runner (self->canvas);
    memory = gst_web_video_frame_wrap (video_frame, runner);
    b = gst_buffer_new ();
    gst_buffer_insert_memory (b, -1, GST_MEMORY_CAST (memory));
    frame->output_buffer = b;
    gst_object_unref (runner);
  }

  flow = gst_video_decoder_finish_frame (dec, frame);
  frame = NULL;
  if (flow == GST_FLOW_FLUSHING) {
    GST_DEBUG_OBJECT (self, "Flushing, dropping frame");
    goto done;
  } else if (flow != GST_FLOW_OK) {
    GST_ERROR_OBJECT (self, "Flow error: %d", flow);
    goto done;
  }

done:
  if (frame)
    gst_video_codec_frame_unref (frame);

  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
}

static void
gst_web_codecs_video_decoder_on_error (guintptr self_, val error)
{
  GstWebCodecsVideoDecoder *self = (GstWebCodecsVideoDecoder *) self_;
  std::string msg = error["message"].as<std::string> ();

  g_mutex_lock (&self->dequeue_lock);
  if (self->flushing) {
    GST_DEBUG_OBJECT (self, "Ignoring WebCodecs error during flush: %s",
        msg.c_str ());
    g_mutex_unlock (&self->dequeue_lock);
    return;
  }
  GST_ERROR_OBJECT (self, "WebCodecs error: %s", msg.c_str ());
  self->has_error = TRUE;
  g_cond_signal (&self->dequeue_cond);
  g_mutex_unlock (&self->dequeue_lock);
}

static void
gst_web_codecs_video_decoder_on_dequeue (guintptr self_, val event)
{
  GstWebCodecsVideoDecoder *self = (GstWebCodecsVideoDecoder *) self_;
  gint dequeue_size;

  dequeue_size = self->decoder["decodeQueueSize"].as<int> ();

  GST_INFO_OBJECT (
      self, "Dequeue received with current size %d", dequeue_size);
  g_mutex_lock (&self->dequeue_lock);
  self->dequeue_size = dequeue_size;
  g_cond_signal (&self->dequeue_cond);
  g_mutex_unlock (&self->dequeue_lock);
  GST_DEBUG_OBJECT (self, "Handle frame notified");
}

EMSCRIPTEN_BINDINGS (gst_web_codecs_video_decoder)
{
  function ("gst_web_codecs_video_decoder_on_output",
      &gst_web_codecs_video_decoder_on_output);
  function ("gst_web_codecs_video_decoder_on_error",
      &gst_web_codecs_video_decoder_on_error);
  function ("gst_web_codecs_video_decoder_on_dequeue",
      &gst_web_codecs_video_decoder_on_dequeue);
}

static void
gst_web_codecs_video_decoder_decode (gpointer data)
{
  GstWebCodecsVideoDecoderDecodeData *decode_data =
      (GstWebCodecsVideoDecoderDecodeData *) data;
  GstWebCodecsVideoDecoder *self = decode_data->self;
  GstVideoCodecFrame *frame = decode_data->frame;
  GstMapInfo map;
  val chunkclass = val::global ("EncodedVideoChunk");
  val options = val::object ();

  GST_DEBUG_OBJECT (self,
      "Decoding frame at %" GST_TIME_FORMAT " with duration %" GST_TIME_FORMAT,
      GST_TIME_ARGS (frame->pts), GST_TIME_ARGS (frame->duration));

  /* TODO use 'transfer' option in case we provide an allocator to avoid
   * the copy. For that, we need to create the memory in JS and give the
   * ownership to it.
   */
  if (GST_CLOCK_TIME_IS_VALID (frame->pts))
    options.set (
        "timestamp", static_cast<int> (GST_TIME_AS_MSECONDS (frame->pts)));
  if (GST_CLOCK_TIME_IS_VALID (frame->duration))
    options.set (
        "duration", static_cast<int> (GST_TIME_AS_MSECONDS (frame->duration)));
  options.set (
      "type", GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame) ? "key" : "delta");
  gst_buffer_map (frame->input_buffer, &map, GST_MAP_READ);
  val buffer_data = val (typed_memory_view (map.size, map.data));
  options.set ("data", buffer_data);

  /* Check decoder state before calling decode() — pending async decode
   * operations may still be queued after the decoder has been closed by an
   * error or reset. Calling decode() on a closed codec throws an uncaught
   * exception that crashes the worker thread. */
  std::string state = self->decoder["state"].as<std::string> ();
  if (state != "configured") {
    GST_WARNING_OBJECT (self,
        "Decoder state is '%s', skipping decode", state.c_str ());
    gst_buffer_unmap (frame->input_buffer, &map);
    gst_video_codec_frame_unref (frame);
    return;
  }

  val chunk = chunkclass.new_ (options);
  self->decoder.call<void> ("decode", chunk);
  gst_buffer_unmap (frame->input_buffer, &map);

  gst_video_codec_frame_unref (frame);
  GST_DEBUG_OBJECT (self, "Done decoding");
}

static void
gst_web_codecs_video_decoder_configure (gpointer data)
{
  GstWebCodecsVideoDecoderConfigureData *conf_data =
      (GstWebCodecsVideoDecoderConfigureData *) data;
  GstWebCodecsVideoDecoder *self = conf_data->self;
  GstVideoCodecState *state = conf_data->state;
  GstStructure *s;
  const GValue *codec_data_value = NULL;
  GstBuffer *codec_data = NULL;
  GstMapInfo map;
  gchar *mime_codec;
  val config = val::object ();

  mime_codec = gst_codec_utils_caps_get_mime_codec (state->caps);
  config.set ("codec", std::string (mime_codec));
  g_free (mime_codec);

  s = gst_caps_get_structure (state->caps, 0);
  codec_data_value = gst_structure_get_value (s, "codec_data");
  if (!codec_data_value) {
    GST_ERROR_OBJECT (self, "Caps do not have codec_data");
    conf_data->ret = FALSE;
    return;
  }

  codec_data = gst_value_get_buffer (codec_data_value);
  if (!gst_buffer_map (codec_data, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Impossible to map the buffer");
    conf_data->ret = FALSE;
    return;
  }

  val codec_data_js = val (typed_memory_view (map.size, map.data));
  config.set ("description", codec_data_js);

  GST_DEBUG_OBJECT (self, "Setting format");
  self->decoder.call<void> ("configure", config);
  gst_buffer_unmap (codec_data, &map);
}

static void
gst_web_codecs_video_decoder_ctor (gpointer data)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (data);
  val vdecclass = val::global ("VideoDecoder");
  val mod = val::global ("Module");
  val options = val::object ();

  self->decoder_generation++;
  guint32 gen = self->decoder_generation;

  /* clang-format off */
  EM_ASM ({
    const self = $0;
    const options = Emval.toValue ($1);
    const gen = $2;
    const gen_offset = $3;
    options["output"] = (data) => {
      /* Stale decoder — drop the frame */
      if (HEAPU32[(self + gen_offset) >> 2] !== gen) {
        data.close();
        return;
      }
      Module.gst_web_codecs_video_decoder_on_output (self, data);
    };
    options["error"] = (e) => {
      if (HEAPU32[(self + gen_offset) >> 2] !== gen) return;
      Module.gst_web_codecs_video_decoder_on_error (self, e);
    }
  }, (guintptr) self, options.as_handle (), gen,
     (guint32) offsetof (GstWebCodecsVideoDecoder, decoder_generation));
  /* clang-format on */

  self->decoder = vdecclass.new_ (options);

  /* clang-format off */
  EM_ASM ({
    const self = $0;
    const decoder = Emval.toValue ($1);
    const gen = $2;
    const gen_offset = $3;

    decoder.addEventListener ("dequeue", (event) => {
      if (HEAPU32[(self + gen_offset) >> 2] !== gen) return;
      Module.gst_web_codecs_video_decoder_on_dequeue (self, event);
    });
  }, (guintptr) self, self->decoder.as_handle (), gen,
     (guint32) offsetof (GstWebCodecsVideoDecoder, decoder_generation));
  /* clang-format on */

  GST_DEBUG_OBJECT (self, "decoder gen=%u created successfully", gen);
}

/* Called with the streaming lock taken */
static gboolean
gst_web_codecs_video_decoder_negotiate (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstCaps *peer_caps;

  /* If we don't know the output format yet, skip this */
  if (!self->need_negotiation)
    return TRUE;

  self->need_negotiation = FALSE;

  if (self->output_state) {
    gst_video_codec_state_unref (self->output_state);
    self->output_state = NULL;
  }

  /* Check if downstream accepts GLMemory */
  self->output_gl = FALSE;
  peer_caps = gst_pad_peer_query_caps (decoder->srcpad, NULL);
  if (peer_caps) {
    GstCapsFeatures *gl_features =
        gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL);
    guint i;
    for (i = 0; i < gst_caps_get_size (peer_caps); i++) {
      GstCapsFeatures *features = gst_caps_get_features (peer_caps, i);
      if (features && gst_caps_features_is_equal (features, gl_features)) {
        self->output_gl = TRUE;
        break;
      }
    }
    gst_caps_features_free (gl_features);
    gst_caps_unref (peer_caps);
  }

  if (self->output_gl) {
    /* For GL output, use RGBA format */
    self->output_state = gst_video_decoder_set_output_state (
        decoder, GST_VIDEO_FORMAT_RGBA, self->width, self->height,
        self->input_state);
    self->output_state->caps =
        gst_video_info_to_caps (&self->output_state->info);
    gst_caps_set_features_simple (self->output_state->caps,
        gst_caps_features_new (
            GST_CAPS_FEATURE_MEMORY_GL_MEMORY, (char *) NULL));
    gst_caps_set_simple (self->output_state->caps,
        "texture-target", G_TYPE_STRING, "2D", NULL);

    GST_INFO_OBJECT (self, "Negotiated GL output");
  } else {
    self->output_state = gst_video_decoder_set_output_state (
        decoder, self->format, self->width, self->height, self->input_state);
    self->output_state->caps =
        gst_video_info_to_caps (&self->output_state->info);
    gst_caps_set_features_simple (self->output_state->caps,
        gst_caps_features_new (
            GST_CAPS_FEATURE_MEMORY_WEB_VIDEO_FRAME, (char *) NULL));
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->negotiate (decoder);
}

static GstFlowReturn
gst_web_codecs_video_decoder_handle_frame (
    GstVideoDecoder *decoder, GstVideoCodecFrame *frame)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebCodecsVideoDecoderDecodeData *decode_data;
  GstWebRunner *runner;
  GstFlowReturn res = GST_FLOW_OK;

  GST_DEBUG_OBJECT (decoder, "Handling frame");
  /* Wait until the decode queue has room, or we are interrupted by
   * a flush or error. */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  g_mutex_lock (&self->dequeue_lock);
  while (self->dequeue_size >= GST_WEB_CODECS_VIDEO_DECODER_MAX_DEQUEUE
      && !self->flushing && !self->has_error) {
    GST_DEBUG_OBJECT (self, "Reached queue limit [%d/%d], waiting for dequeue",
        self->dequeue_size, GST_WEB_CODECS_VIDEO_DECODER_MAX_DEQUEUE);
    g_cond_wait (&self->dequeue_cond, &self->dequeue_lock);
  }
  if (self->flushing) {
    g_mutex_unlock (&self->dequeue_lock);
    GST_VIDEO_DECODER_STREAM_LOCK (self);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_FLUSHING;
  }
  if (self->has_error) {
    g_mutex_unlock (&self->dequeue_lock);
    GST_VIDEO_DECODER_STREAM_LOCK (self);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
  self->dequeue_size++;
  g_mutex_unlock (&self->dequeue_lock);
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  /* We are ready to process data */
  GST_DEBUG_OBJECT (self, "Ready to process more data");
  decode_data = g_new (GstWebCodecsVideoDecoderDecodeData, 1);
  decode_data->self = self;
  decode_data->frame = frame;
  /* We can not keep the stream lock taken here and when the decoder outputs
   * frames, do it asynchronous
   */
  runner = gst_web_canvas_get_runner (self->canvas);
  gst_web_runner_send_message_async (
      runner, gst_web_codecs_video_decoder_decode, decode_data, g_free);
  gst_object_unref (runner);

  GST_DEBUG_OBJECT (decoder, "Handle frame done");

  return res;
}

static gboolean
gst_web_codecs_video_decoder_sink_event (
    GstVideoDecoder *decoder, GstEvent *event)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);

  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_START) {
    GST_DEBUG_OBJECT (self, "FLUSH_START — unblocking handle_frame");
    g_mutex_lock (&self->dequeue_lock);
    self->flushing = TRUE;
    g_cond_signal (&self->dequeue_cond);
    g_mutex_unlock (&self->dequeue_lock);
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->sink_event (decoder, event);
}

static gboolean
gst_web_codecs_video_decoder_set_format (
    GstVideoDecoder *decoder, GstVideoCodecState *state)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebCodecsVideoDecoderConfigureData conf_data;
  GstWebRunner *runner;

  GST_INFO_OBJECT (
      self, "Setting format with sink caps %" GST_PTR_FORMAT, state->caps);
#if 0
  /* If the output format IS GL, we need to request the context
   * and do everything on the GL context thread, otherwise create
   * our own thread. This is a limitation on how Emscripten works
   * tegarding emscripten::val and threads. The vals are not shared
   * among threads, are local.
   * We have an open issue for this case:
   * https://github.com/emscripten-core/emscripten/issues/22541
   *
   * In any case we'll have a situation, the video_frame belongs to the WebRunner
   * transferring the image to a GLTexture requires that both, the GL thread and the
   * WebRunner thread share the js video_frame ... we'll figure that out later.
   * There are several options to handle this
   */

  if (!gst_web_runner_run (self->runner, NULL)) {
    GST_ERROR_OBJECT (self, "Impossible to run the runner");
    return FALSE;
  }
#endif
  /* TODO Check if downstream OpenGL is supported, if so, request the GL
   * context */

  /* Call constructor */
  runner = gst_web_canvas_get_runner (self->canvas);
  gst_web_runner_send_message (
      runner, gst_web_codecs_video_decoder_ctor, self);
  /* Configure */
  conf_data.self = self;
  conf_data.state = state;
  gst_web_runner_send_message (
      runner, gst_web_codecs_video_decoder_configure, &conf_data);

  /* Keep the input state available */
  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = gst_video_codec_state_ref (state);

  gst_object_unref (runner);

  return TRUE;
}

typedef struct _GstWebCodecsVideoDecoderFlushData
{
  GstWebCodecsVideoDecoder *self;
  GMutex lock;
  GCond cond;
  gboolean done;
} GstWebCodecsVideoDecoderFlushData;

static void
gst_web_codecs_video_decoder_do_flush (gpointer data)
{
  GstWebCodecsVideoDecoderFlushData *flush_data =
      (GstWebCodecsVideoDecoderFlushData *) data;
  GstWebCodecsVideoDecoder *self = flush_data->self;

  GST_DEBUG_OBJECT (self, "Calling WebCodecs flush()");

  /* Call flush() and attach a .then() callback that signals completion.
   * The output callbacks will fire during the flush before the promise
   * resolves. We use EM_ASM to call .then() since we can't use .await()
   * inside a GMainContext dispatch. */
  EM_ASM ({
    var decoder = Emval.toValue ($0);
    var flush_data_ptr = $1;
    decoder.flush ().then (function () {
      /* Signal completion back to C */
      Module._gst_web_codecs_video_decoder_flush_done (flush_data_ptr);
    });
  }, self->decoder.as_handle (), (guintptr) flush_data);
}

extern "C" EMSCRIPTEN_KEEPALIVE void
gst_web_codecs_video_decoder_flush_done (guintptr flush_data_ptr)
{
  GstWebCodecsVideoDecoderFlushData *flush_data =
      (GstWebCodecsVideoDecoderFlushData *) flush_data_ptr;

  g_mutex_lock (&flush_data->lock);
  flush_data->done = TRUE;
  g_cond_signal (&flush_data->cond);
  g_mutex_unlock (&flush_data->lock);
}

static GstFlowReturn
gst_web_codecs_video_decoder_finish (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebRunner *runner;
  GstWebCodecsVideoDecoderFlushData flush_data;

  GST_DEBUG_OBJECT (self, "Draining — flushing WebCodecs decoder");

  flush_data.self = self;
  flush_data.done = FALSE;
  g_mutex_init (&flush_data.lock);
  g_cond_init (&flush_data.cond);

  /* Release stream lock while we wait, otherwise the output callback
   * (which takes the stream lock) will deadlock */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  runner = gst_web_canvas_get_runner (self->canvas);
  gst_web_runner_send_message_async (
      runner, gst_web_codecs_video_decoder_do_flush, &flush_data, NULL);
  gst_object_unref (runner);

  /* Wait for the JS flush() Promise to resolve */
  g_mutex_lock (&flush_data.lock);
  while (!flush_data.done)
    g_cond_wait (&flush_data.cond, &flush_data.lock);
  g_mutex_unlock (&flush_data.lock);

  g_mutex_clear (&flush_data.lock);
  g_cond_clear (&flush_data.cond);

  GST_VIDEO_DECODER_STREAM_LOCK (self);

  GST_DEBUG_OBJECT (self, "Drain complete");
  return GST_FLOW_OK;
}

static void
gst_web_codecs_video_decoder_do_reset (gpointer data)
{
  GstWebCodecsVideoDecoderConfigureData *conf_data =
      (GstWebCodecsVideoDecoderConfigureData *) data;
  GstWebCodecsVideoDecoder *self = conf_data->self;

  GST_DEBUG_OBJECT (self, "Recreating WebCodecs decoder for flush");
  gst_web_codecs_video_decoder_ctor (self);
  GST_DEBUG_OBJECT (self, "Reconfiguring after recreate");
  gst_web_codecs_video_decoder_configure (data);
}

static gboolean
gst_web_codecs_video_decoder_flush (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebCodecsVideoDecoderConfigureData conf_data;
  GstWebRunner *runner;

  GST_DEBUG_OBJECT (self, "Flushing");

  if (!self->input_state) {
    GST_DEBUG_OBJECT (self, "No input state, nothing to flush");
    return TRUE;
  }

  conf_data.self = self;
  conf_data.state = self->input_state;

  /* Release stream lock before synchronous dispatch to the runner thread.
   * The on_output callback runs on the runner thread and takes the stream
   * lock — keeping it held here would deadlock. */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  runner = gst_web_canvas_get_runner (self->canvas);
  gst_web_runner_send_message (
      runner, gst_web_codecs_video_decoder_do_reset, &conf_data);
  gst_object_unref (runner);

  /* reset() clears the decode queue without firing on_dequeue callbacks,
   * so we must reset the counter ourselves to unblock handle_frame.
   * Also clear the flushing and error flags so handle_frame resumes. */
  g_mutex_lock (&self->dequeue_lock);
  self->dequeue_size = 0;
  self->flushing = FALSE;
  self->has_error = FALSE;
  g_cond_signal (&self->dequeue_cond);
  g_mutex_unlock (&self->dequeue_lock);

  GST_VIDEO_DECODER_STREAM_LOCK (self);

  GST_DEBUG_OBJECT (self, "Flushed");
  return TRUE;
}

static gboolean
gst_web_codecs_video_decoder_open (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  gboolean ret = FALSE;

  if (!gst_web_utils_element_ensure_canvas (
          GST_ELEMENT (self), &self->canvas, NULL)) {
    GST_ERROR_OBJECT (self, "Failed requesting a WebCanvas context");
    goto done;
  }

  ret = TRUE;

done:
  return ret;
}

static gboolean
gst_web_codecs_video_decoder_start (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);
  GstWebRunner *runner;
  gboolean ret = FALSE;

  GST_DEBUG_OBJECT (self, "Start");

  g_mutex_lock (&self->dequeue_lock);
  self->flushing = FALSE;
  self->has_error = FALSE;
  g_mutex_unlock (&self->dequeue_lock);

  runner = gst_web_canvas_get_runner (self->canvas);
  if (!gst_web_runner_run (runner, NULL)) {
    GST_ERROR_OBJECT (self, "Impossible to run the runner");
    goto done;
  }
  GST_DEBUG_OBJECT (self, "Started");
  ret = TRUE;

done:
  gst_object_unref (runner);
  return ret;
}

static gboolean
gst_web_codecs_video_decoder_stop (GstVideoDecoder *decoder)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);

  GST_DEBUG_OBJECT (self, "Stop");

  /* Unblock handle_frame if it is waiting on the decode queue — the
   * streaming thread may be blocked there while the state-change thread
   * is tearing us down. */
  g_mutex_lock (&self->dequeue_lock);
  self->flushing = TRUE;
  g_cond_signal (&self->dequeue_cond);
  g_mutex_unlock (&self->dequeue_lock);

  if (self->output_format) {
    g_free (self->output_format);
    self->output_format = NULL;
  }

  if (self->input_state) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  if (self->output_state) {
    gst_video_codec_state_unref (self->output_state);
    self->output_state = NULL;
  }

  GST_DEBUG_OBJECT (self, "Stopped");

  return TRUE;
}

static void
gst_web_codecs_video_decoder_set_context (
    GstElement *element, GstContext *context)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (element);

  gst_web_utils_element_set_context (element, context, &self->canvas);
  gst_gl_handle_set_context (element, context,
      &self->gl_display, &self->other_gl_context);
}

static gboolean
gst_web_codecs_video_decoder_query (GstElement *element, GstQuery *query)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (element);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      ret = gst_web_utils_element_handle_context_query (
          element, query, self->canvas);
      if (!ret) {
        ret = gst_gl_handle_context_query (element, query,
            self->gl_display, self->gl_context, self->other_gl_context);
      }
      /* Never chain context queries to the parent class — GstVideoDecoder
       * forwards them to pads which query peers, causing infinite recursion
       * between the decoder and downstream GL elements. */
      return ret;
    default:
      break;
  }

  if (!ret)
    ret = GST_ELEMENT_CLASS (parent_class)->query (element, query);

  return ret;
}

static GstStateChangeReturn
gst_web_codecs_video_decoder_change_state (
    GstElement *element, GstStateChange transition)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_gl_ensure_element_data (element, &self->gl_display,
              &self->other_gl_context))
        return GST_STATE_CHANGE_FAILURE;

      /* Attach the WebRunner's GMainContext to the GL display so that
       * gstglcontext_emscripten can create the GL context on the runner
       * thread instead of spawning a new one.  This enables zero-copy
       * texImage2D(videoFrame) since both live on the same thread. */
      if (self->gl_display && self->canvas) {
        GstWebRunner *runner = gst_web_canvas_get_runner (self->canvas);
        if (runner) {
          GMainContext *runner_ctx = gst_web_runner_get_main_context (runner);
          static GQuark runner_ctx_quark = 0;
          if (G_UNLIKELY (runner_ctx_quark == 0))
            runner_ctx_quark =
                g_quark_from_static_string ("gst.gl.runner.main-context");
          g_object_set_qdata (G_OBJECT (self->gl_display),
              runner_ctx_quark, runner_ctx);
          GST_INFO_OBJECT (self, "Attached runner GMainContext to GL display");
          gst_object_unref (runner);
        }
      }
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_clear_object (&self->gl_context);
      gst_clear_object (&self->other_gl_context);
      gst_clear_object (&self->gl_display);
      self->output_gl = FALSE;
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_web_codecs_video_decoder_finalize (GObject *object)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (object);

  g_mutex_clear (&self->dequeue_lock);
  g_cond_clear (&self->dequeue_cond);
  if (self->canvas) {
    gst_object_unref (self->canvas);
    self->canvas = NULL;
  }
  gst_clear_object (&self->gl_context);
  gst_clear_object (&self->other_gl_context);
  gst_clear_object (&self->gl_display);

  GST_DEBUG_OBJECT (self, "End of finalize");
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_web_codecs_video_decoder_decide_allocation (
    GstVideoDecoder *decoder, GstQuery *query)
{
  GstWebCodecsVideoDecoder *self = GST_WEB_CODECS_VIDEO_DECODER (decoder);

  if (self->output_gl) {
    /* Get the GL context from downstream (e.g. glimagesink).
     * This ensures we use the same context for texture creation,
     * so textures are valid in downstream elements. */
    gst_clear_object (&self->gl_context);

    if (!gst_gl_query_local_gl_context (GST_ELEMENT (self), GST_PAD_SRC,
            &self->gl_context)) {
      /* No context from downstream — try the display */
      if (self->gl_display) {
        self->gl_context =
            gst_gl_display_get_gl_context_for_thread (self->gl_display, NULL);
        if (!self->gl_context) {
          gst_gl_display_create_context (self->gl_display,
              self->other_gl_context, &self->gl_context, NULL);
        }
      }
    }

    if (self->gl_context) {
      GST_INFO_OBJECT (self, "Using GL context %" GST_PTR_FORMAT
          " from downstream", self->gl_context);
    } else {
      GST_WARNING_OBJECT (self, "No GL context available, "
          "falling back to WebVideoFrame");
      self->output_gl = FALSE;
    }
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (
      decoder, query);
}

static void
gst_web_codecs_video_decoder_init (
    GstWebCodecsVideoDecoder *self, GstWebCodecsVideoDecoderClass g_class)
{
  gst_video_decoder_set_needs_sync_point (GST_VIDEO_DECODER (self), TRUE);
  g_mutex_init (&self->dequeue_lock);
  g_cond_init (&self->dequeue_cond);
}

static void
gst_web_codecs_video_decoder_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstCaps *sink_caps;
  GstCaps *src_caps;
  GstCaps *copy_to_caps;
  GstCaps *gl_caps;
  GstCaps *video_frame_caps;
  GstPadTemplate *templ;

  sink_caps = (GstCaps *) g_type_get_qdata (
      G_TYPE_FROM_CLASS (g_class), gst_web_codecs_data_quark);
  /* This happens for the base class and abstract subclasses */
  if (!sink_caps)
    return;

  /* The caps accepted by the copyTo() */
  /* FIXME remove it once the VideoFrame based memory is ready */
  copy_to_caps = gst_caps_from_string ("video/x-raw, "
                                       "format = { (string) RGBA, (string) "
                                       "RGBx, (string) BGRA, (string) BGRx }");

  video_frame_caps = gst_caps_from_string (
      "video/x-raw(" GST_CAPS_FEATURE_MEMORY_WEB_VIDEO_FRAME "),"
      "format = { (string) RGBA, (string) RGBx, (string) BGRA, (string) BGRx, "
      "(string) I420, (string) A420, (string) Y42B, (string) Y444, (string) "
      "NV12 }");
  gl_caps = gst_caps_from_string (
      "video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY
      "), format = (string) RGBA, texture-target = (string) 2D");

  src_caps = gst_caps_merge (gl_caps, copy_to_caps);
  src_caps = gst_caps_merge (src_caps, video_frame_caps);
  /* Add pad templates */
  templ =
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink_caps);
  gst_element_class_add_pad_template (element_class, templ);

  templ = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, src_caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (gl_caps);
}

static void
gst_web_codecs_video_decoder_class_init (
    GstWebCodecsVideoDecoderClass *klass, gpointer klass_data)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gobject_class->finalize = gst_web_codecs_video_decoder_finalize;
  element_class->set_context = gst_web_codecs_video_decoder_set_context;
  element_class->change_state = gst_web_codecs_video_decoder_change_state;
  element_class->query = gst_web_codecs_video_decoder_query;
  gst_element_class_set_static_metadata (element_class,
      "WebCodecs base video decoder", "Codec/Decoder/Video",
      "decode streams using WebCodecs API",
      "Fluendo S.A. <engineering@fluendo.com>");

  video_decoder_class->open =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_open);
  video_decoder_class->start =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_start);
  video_decoder_class->stop =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_stop);
  video_decoder_class->finish =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_finish);
  video_decoder_class->flush =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_flush);
  video_decoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_sink_event);
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_set_format);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_handle_frame);
  video_decoder_class->negotiate =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_negotiate);
  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_web_codecs_video_decoder_decide_allocation);

  parent_class = g_type_class_peek_parent (klass);
}

GType
gst_web_codecs_video_decoder_get_type (void)
{
  static gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = { sizeof (GstWebCodecsVideoDecoderClass),
      (GBaseInitFunc) gst_web_codecs_video_decoder_base_init, NULL,
      (GClassInitFunc) gst_web_codecs_video_decoder_class_init, NULL, NULL,
      sizeof (GstWebCodecsVideoDecoder), 0,
      (GInstanceInitFunc) gst_web_codecs_video_decoder_init, NULL };

    _type = g_type_register_static (GST_TYPE_VIDEO_DECODER,
        "GstWebCodecsVideoDecoder", &info, G_TYPE_FLAG_NONE);

    GST_DEBUG_CATEGORY_INIT (gst_web_codecs_video_decoder_debug_category,
        "webcodecsviddec", 0, "WebCodecs Video Decoder");

    g_once_init_leave (&type, _type);
  }
  return type;
}
