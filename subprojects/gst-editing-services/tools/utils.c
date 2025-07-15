/* GStreamer Editing Services
 * Copyright (C) 2015 Mathieu Duponchelle <mathieu.duponchelle@opencreed.com>
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

#include <stdlib.h>
#include <glib/gprintf.h>
#include <string.h>
#include <gst/gst.h>
#include "utils.h"
#include "../ges/ges-internal.h"
#include "ges/ges-timeline-element.h"

#ifdef G_OS_WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#undef GST_CAT_DEFAULT

/* Copy of GST_ASCII_IS_STRING */
#define ASCII_IS_STRING(c) (g_ascii_isalnum((c)) || ((c) == '_') || \
    ((c) == '-') || ((c) == '+') || ((c) == '/') || ((c) == ':') || \
    ((c) == '.'))

/* g_free after usage */
static gchar *
_sanitize_argument (gchar * arg, const gchar * prev_arg)
{
  gboolean expect_equal = !(arg[0] == '+' || g_str_has_prefix (arg, "set-")
      || prev_arg == NULL || prev_arg[0] == '+'
      || g_str_has_prefix (prev_arg, "set-"));
  gboolean need_wrap = FALSE;
  gchar *first_equal = NULL;
  gchar *wrap_start;
  gchar *new_string, *tmp_string;
  gsize num_escape;

  for (tmp_string = arg; *tmp_string != '\0'; tmp_string++) {
    if (expect_equal && first_equal == NULL && *tmp_string == '=') {
      first_equal = tmp_string;
      /* if this is the first equal, then don't count it as necessarily
       * needing a wrap */
    } else if (!ASCII_IS_STRING (*tmp_string)) {
      need_wrap = TRUE;
      break;
    }
  }

  if (!need_wrap)
    return g_strdup (arg);

  if (first_equal)
    wrap_start = first_equal + 1;
  else
    wrap_start = arg;

  /* need to escape any '"' or '\\' to correctly parse in as a structure */
  num_escape = 0;
  for (tmp_string = wrap_start; *tmp_string != '\0'; tmp_string++) {
    if (*tmp_string == '"' || *tmp_string == '\\')
      num_escape++;
  }

  tmp_string = new_string =
      g_malloc (sizeof (gchar) * (strlen (arg) + num_escape + 3));

  while (arg != wrap_start)
    *(tmp_string++) = *(arg++);
  (*tmp_string++) = '"';

  while (*arg != '\0') {
    if (*arg == '"' || *arg == '\\')
      (*tmp_string++) = '\\';
    *(tmp_string++) = *(arg++);
  }
  *(tmp_string++) = '"';
  *tmp_string = '\0';

  return new_string;
}

gchar *
sanitize_timeline_description (gchar ** args, GESLauncherParsedOptions * opts)
{
  gint i;
  gchar *prev_arg = NULL;
  GString *track_def;
  GString *timeline_str;
  gboolean adds_tracks = FALSE;

  gchar *string = g_strdup (" ");

  for (i = 1; args[i]; i++) {
    gchar *new_string;
    gchar *sanitized = _sanitize_argument (args[i], prev_arg);

    new_string = g_strconcat (string, " ", sanitized, NULL);

    adds_tracks |= (g_strcmp0 (args[i], "+track") == 0);

    g_free (sanitized);
    g_free (string);
    string = new_string;
    prev_arg = args[i];
  }

  if (i == 1) {
    g_free (string);

    return NULL;
  }

  if (adds_tracks) {
    gchar *res = g_strconcat ("ges:", string, NULL);
    g_free (string);

    return res;
  }

  timeline_str = g_string_new (string);
  g_free (string);

  if (opts->track_types & GES_TRACK_TYPE_VIDEO) {
    track_def = g_string_new (" +track video ");

    if (opts->video_track_caps)
      g_string_append_printf (track_def, " restrictions=[%s] ",
          opts->video_track_caps);

    g_string_prepend (timeline_str, track_def->str);
    g_string_free (track_def, TRUE);
  }

  if (opts->track_types & GES_TRACK_TYPE_AUDIO) {
    track_def = g_string_new (" +track audio ");

    if (opts->audio_track_caps)
      g_string_append_printf (track_def, " restrictions=[%s] ",
          opts->audio_track_caps);

    g_string_prepend (timeline_str, track_def->str);
    g_string_free (track_def, TRUE);
  }

  g_string_prepend (timeline_str, "ges:");

  return g_string_free (timeline_str, FALSE);
}

gboolean
get_flags_from_string (GType type, const gchar * str_flags, guint * flags)
{
  GValue value = G_VALUE_INIT;
  g_value_init (&value, type);

  if (!gst_value_deserialize (&value, str_flags)) {
    g_value_unset (&value);

    return FALSE;
  }

  *flags = g_value_get_flags (&value);
  g_value_unset (&value);

  return TRUE;
}

gchar *
ensure_uri (const gchar * location)
{
  if (gst_uri_is_valid (location))
    return g_strdup (location);
  else
    return gst_filename_to_uri (location, NULL);
}

void
print_enum (GType enum_type)
{
  GEnumClass *enum_class = G_ENUM_CLASS (g_type_class_ref (enum_type));
  guint i;

  for (i = 0; i < enum_class->n_values; i++) {
    gst_print ("%s\n", enum_class->values[i].value_nick);
  }

  g_type_class_unref (enum_class);
}

void
ges_print (GstDebugColorFlags c, gboolean err, gboolean nline,
    const gchar * format, va_list var_args)
{
  GString *str = g_string_new (NULL);
  GstDebugColorMode color_mode;
  gchar *color = NULL;
  const gchar *clear = NULL;

  color_mode = gst_debug_get_color_mode ();
#ifdef G_OS_WIN32
  if (color_mode == GST_DEBUG_COLOR_MODE_UNIX) {
#else
  if (color_mode != GST_DEBUG_COLOR_MODE_OFF) {
#endif
    clear = "\033[00m";
    color = gst_debug_construct_term_color (c);
  }

  if (color) {
    g_string_append (str, color);
    g_free (color);
  }

  g_string_append_vprintf (str, format, var_args);

  if (nline)
    g_string_append_c (str, '\n');

  if (clear)
    g_string_append (str, clear);

  if (err)
    gst_printerr ("%s", str->str);
  else
    gst_print ("%s", str->str);

  g_string_free (str, TRUE);
}

void
ges_ok (const gchar * format, ...)
{
  va_list var_args;

  va_start (var_args, format);
  ges_print (GST_DEBUG_FG_GREEN, FALSE, TRUE, format, var_args);
  va_end (var_args);
}

void
ges_warn (const gchar * format, ...)
{
  va_list var_args;

  va_start (var_args, format);
  ges_print (GST_DEBUG_FG_YELLOW, TRUE, TRUE, format, var_args);
  va_end (var_args);
}

void
ges_printerr (const gchar * format, ...)
{
  va_list var_args;

  va_start (var_args, format);
  ges_print (GST_DEBUG_FG_RED, TRUE, TRUE, format, var_args);
  va_end (var_args);
}

gchar *
get_file_extension (gchar * uri)
{
  size_t len;
  gint find;

  len = strlen (uri);
  find = len - 1;

  while (find >= 0) {
    if (uri[find] == '.')
      break;
    find--;
  }

  if (find <= 0)
    return NULL;

  return g_strdup (&uri[find + 1]);
}

static const gchar *
get_type_icon (gpointer obj)
{
  if (GST_IS_ENCODING_AUDIO_PROFILE (obj) || GST_IS_DISCOVERER_AUDIO_INFO (obj))
    return "🔊 ";
  else if (GST_IS_ENCODING_VIDEO_PROFILE (obj)
      || GST_IS_DISCOVERER_VIDEO_INFO (obj))
    return "🎞 ";
  else if (GST_IS_ENCODING_CONTAINER_PROFILE (obj)
      || GST_IS_DISCOVERER_CONTAINER_INFO (obj))
    return "📽 ";
  else
    return "";
}

static void
print_profile (GstEncodingProfile * profile, const gchar * prefix)
{
  const gchar *name = gst_encoding_profile_get_name (profile);
  const gchar *desc = gst_encoding_profile_get_description (profile);
  GstCaps *format = gst_encoding_profile_get_format (profile);
  gchar *capsdesc = NULL;

  if (gst_caps_is_fixed (format))
    capsdesc = gst_pb_utils_get_codec_description (format);
  if (!capsdesc)
    capsdesc = gst_caps_to_string (format);

  if (GST_IS_ENCODING_CONTAINER_PROFILE (profile)) {
    gst_print ("%s> %s %s: %s%s%s%s\n", prefix,
        get_type_icon (profile),
        capsdesc, name ? name : "",
        desc ? " (" : "", desc ? desc : "", desc ? ")" : "");

  } else {
    gst_print ("%s%s %s%s%s%s%s%s", prefix, get_type_icon (profile),
        name ? name : capsdesc, desc ? ": " : "", desc ? desc : "",
        name ? " (" : "", name ? capsdesc : "", name ? ")" : "");

    if (GST_IS_ENCODING_VIDEO_PROFILE (profile)) {
      GstCaps *caps = gst_encoding_profile_get_restriction (profile);

      if (!caps && gst_caps_is_fixed (format))
        caps = gst_caps_ref (format);

      if (caps) {
        GstVideoInfo info;

        if (gst_video_info_from_caps (&info, caps)) {
          gst_print (" (%dx%d", info.width, info.height);
          if (info.fps_n)
            gst_print ("@%d/%dfps", info.fps_n, info.fps_d);
          gst_print (")");
        }
        gst_caps_unref (caps);
      }
    } else if (GST_IS_ENCODING_AUDIO_PROFILE (profile)) {
      GstCaps *caps = gst_encoding_profile_get_restriction (profile);

      if (!caps && gst_caps_is_fixed (format))
        caps = gst_caps_ref (format);

      if (caps) {
        GstAudioInfo info;

        if (gst_caps_is_fixed (caps) && gst_audio_info_from_caps (&info, caps))
          gst_print (" (%d channels @ %dhz)", info.channels, info.rate);
        gst_caps_unref (caps);
      }
    }


    gst_print ("\n");
  }

  gst_caps_unref (format);

  g_free (capsdesc);
}

void
describe_encoding_profile (GstEncodingProfile * profile)
{
  g_return_if_fail (GST_IS_ENCODING_PROFILE (profile));

  print_profile (profile, "     ");
  if (GST_IS_ENCODING_CONTAINER_PROFILE (profile)) {
    const GList *tmp;

    for (tmp =
        gst_encoding_container_profile_get_profiles
        (GST_ENCODING_CONTAINER_PROFILE (profile)); tmp; tmp = tmp->next)
      print_profile (tmp->data, "       - ");
  }
}

static void
describe_stream_info (GstDiscovererStreamInfo * sinfo, GString * desc)
{
  gchar *capsdesc;
  GstCaps *caps;

  caps = gst_discoverer_stream_info_get_caps (sinfo);
  capsdesc = gst_pb_utils_get_codec_description (caps);
  if (!capsdesc)
    capsdesc = gst_caps_to_string (caps);
  gst_caps_unref (caps);

  g_string_append_printf (desc, "%s%s%s", desc->len ? ", " : "",
      get_type_icon (sinfo), capsdesc);

  g_free (capsdesc);

  if (GST_IS_DISCOVERER_CONTAINER_INFO (sinfo)) {
    GList *tmp, *streams;

    streams =
        gst_discoverer_container_info_get_streams (GST_DISCOVERER_CONTAINER_INFO
        (sinfo));
    for (tmp = streams; tmp; tmp = tmp->next)
      describe_stream_info (tmp->data, desc);
    gst_discoverer_stream_info_list_free (streams);
  }
}

static gchar *
describe_discoverer (GstDiscovererInfo * info)
{
  GString *desc = g_string_new (NULL);
  GstDiscovererStreamInfo *sinfo = gst_discoverer_info_get_stream_info (info);

  describe_stream_info (sinfo, desc);
  gst_discoverer_stream_info_unref (sinfo);

  return g_string_free (desc, FALSE);
}

static gchar *
ges_clip_get_time_effects_rates (GESClip * clip)
{
  GList *l, *effects;
  GString *rates_str = g_string_new ("");
  gboolean reverse = FALSE;
  GstClockTime duration = GES_TIMELINE_ELEMENT_DURATION (clip);
  GHashTable *track_rates =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  gboolean all_same_rate = TRUE;
  gfloat first_rate = 0.0;

  ges_timeline_element_get_child_properties (GES_TIMELINE_ELEMENT (clip),
      "reverse", &reverse, NULL);

  effects = ges_clip_get_top_effects (clip);
  for (l = effects; l; l = l->next) {
    GESBaseEffect *effect = GES_BASE_EFFECT (l->data);
    GESTrack *track = ges_track_element_get_track (GES_TRACK_ELEMENT (effect));
    GHashTable *values;
    GstClockTime transformed_duration = duration;
    gfloat *track_rate;

    if (!track)
      continue;

    values = ges_base_effect_get_time_property_values (effect);
    if (!values)
      continue;

    transformed_duration =
        ges_base_effect_translate_source_to_sink_time (effect, duration,
        values);
    g_hash_table_unref (values);

    /* Get or create rate for this track */
    track_rate = g_hash_table_lookup (track_rates, track);
    if (!track_rate) {
      track_rate = g_new (gfloat, 1);
      *track_rate = 1.0;
      g_hash_table_insert (track_rates, track, track_rate);
    }

    /* Apply this effect's transformation to the track's rate */
    if (duration > 0 && transformed_duration > 0) {
      if (reverse) {
        /* In reverse mode, we need to adjust the rate inversely */
        *track_rate =
            *track_rate * (gfloat) transformed_duration / (gfloat) duration;
      } else {
        *track_rate =
            *track_rate * ((gfloat) duration / (gfloat) transformed_duration);
      }
    }
  }

  g_list_free_full (effects, gst_object_unref);

  /* Check if all tracks have the same rate */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, track_rates);

  while (g_hash_table_iter_next (&iter, &key, &value)) {
    gfloat *rate = (gfloat *) value;
    if (first_rate == 0.0) {
      first_rate = *rate;
    } else if (first_rate != *rate) {
      all_same_rate = FALSE;
    }
  }

  /* Format the output string */
  if (g_hash_table_size (track_rates) > 0) {
    if (all_same_rate && first_rate != 1.0) {
      g_string_append_printf (rates_str, " @ %s%.2fx", reverse ? "-" : "",
          first_rate);
    } else if (!all_same_rate) {
      gboolean first = TRUE;

      g_string_append (rates_str, " @");
      g_hash_table_iter_init (&iter, track_rates);
      while (g_hash_table_iter_next (&iter, &key, &value)) {
        GESTrack *track = (GESTrack *) key;
        gfloat *rate = (gfloat *) value;

        if (*rate != 1.0) {
          const gchar *track_type = track->type == GES_TRACK_TYPE_VIDEO ? "V" :
              track->type == GES_TRACK_TYPE_AUDIO ? "A" : "?";
          g_string_append_printf (rates_str, "%s %s:%s%.2fx",
              first ? "" : ",", track_type, reverse ? "-" : "", *rate);
          first = FALSE;
        }
      }
    }
  }

  g_hash_table_destroy (track_rates);
  return g_string_free (rates_str, FALSE);
}

static gchar *
create_clip_bar (GESClip * clip, GstClockTime layer_end, gint width)
{
  GString *result = g_string_sized_new (width + 1);
  gboolean *filled = g_malloc0 (width * sizeof (gboolean));

  if (layer_end == 0) {
    g_string_append_printf (result, "%*s", width, "");
    g_free (filled);
    return g_string_free (result, FALSE);
  }

  GstClockTime start = GES_TIMELINE_ELEMENT_START (clip);
  GstClockTime end = GES_TIMELINE_ELEMENT_END (clip);

  gint start_pos = (gint) ((start * width) / layer_end);
  gint end_pos = (gint) ((end * width) / layer_end);

  start_pos = CLAMP (start_pos, 0, width - 1);
  end_pos = CLAMP (end_pos, 0, width - 1);

  for (gint i = start_pos; i <= end_pos && i < width; i++) {
    filled[i] = TRUE;
  }

  /* Build result string with colors */
  GESTrackType supported_types = ges_clip_get_supported_formats (clip);
  const gchar *color_start = "";
  const gchar *color_end = "";

  /* Choose color based on track type */
  if (gst_debug_get_color_mode () != GST_DEBUG_COLOR_MODE_OFF) {
    if (supported_types & GES_TRACK_TYPE_VIDEO
        && supported_types & GES_TRACK_TYPE_AUDIO) {
      color_start = "\033[1;32m";       /* Bright green for mixed */
    } else if (supported_types & GES_TRACK_TYPE_VIDEO) {
      color_start = "\033[0;32m";       /* Regular green for video */
    } else if (supported_types & GES_TRACK_TYPE_AUDIO) {
      color_start = "\033[0;36m";       /* Cyan for audio */
    }
    color_end = "\033[0m";
  }

  gboolean in_block = FALSE;
  for (gint i = 0; i < width; i++) {
    if (filled[i] && !in_block) {
      /* Start of a block */
      g_string_append (result, color_start);
      g_string_append (result, "█");
      in_block = TRUE;
    } else if (filled[i] && in_block) {
      /* Continue block */
      g_string_append (result, "█");
    } else if (!filled[i] && in_block) {
      /* End of block */
      g_string_append (result, color_end);
      g_string_append_c (result, ' ');
      in_block = FALSE;
    } else {
      /* Empty space */
      g_string_append_c (result, ' ');
    }
  }

  /* Close color if we ended in a block */
  if (in_block) {
    g_string_append (result, color_end);
  }

  g_free (filled);
  return g_string_free (result, FALSE);
}

static gchar *
repeat_utf8_char (const gchar * utf8_char, gint count)
{
  if (count <= 0 || !utf8_char)
    return g_strdup ("");

  GString *result = g_string_sized_new (count * strlen (utf8_char));
  for (gint i = 0; i < count; i++) {
    g_string_append (result, utf8_char);
  }
  return g_string_free (result, FALSE);
}

static gchar *
format_timeline_description (const gchar * description)
{
  gchar *formatted = g_strdup (description);

  /* Replace " +" with "\n  +" to put each command on a new line */
  gchar **parts = g_strsplit (formatted, " +", -1);
  g_free (formatted);

  GString *result = g_string_new ("");

  for (gint i = 0; parts[i]; i++) {
    gchar *part = parts[i];

    if (i == 0) {
      /* First part - handle initial command */
      g_string_append (result, part);
    } else {
      /* Split command from parameters */
      gchar **cmd_parts = g_strsplit (part, " ", -1);
      if (cmd_parts[0]) {
        gchar *cmd = cmd_parts[0];
        gint base_indent = g_str_equal (cmd, "effect") ? 4 : 2;

        /* Add colored command */
        g_string_append_printf (result, " \\\n%*s\033[1;32m+%s\033[0m",
            base_indent, "", cmd);

        /* Add parameters on separate lines with proper indentation */
        for (gint j = 1; cmd_parts[j]; j++) {
          gchar *param = cmd_parts[j];

          if (strchr (param, '=')) {
            /* Parameter with value - put on new line with extra indent */
            g_string_append_printf (result, " \\\n%*s%s",
                base_indent + 2, "", param);
          } else {
            /* Simple parameter - add to same line */
            g_string_append_printf (result, " %s", param);
          }
        }
      }
      g_strfreev (cmd_parts);
    }
  }

  g_strfreev (parts);
  return g_string_free (result, FALSE);
}

static gint
get_terminal_width (void)
{
#ifdef G_OS_WIN32
  HANDLE console = GetStdHandle (STD_OUTPUT_HANDLE);
  if (console != INVALID_HANDLE_VALUE) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo (console, &csbi)) {
      return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
  }
#else
  struct winsize ws;
  if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return ws.ws_col;
  }
#endif

  /* Fallback to COLUMNS environment variable */
  const gchar *columns_env = g_getenv ("COLUMNS");
  if (columns_env) {
    gint width = g_ascii_strtoll (columns_env, NULL, 10);
    if (width > 0)
      return width;
  }

  /* Final fallback */
  return 80;
}

static gchar *
truncate_clip_name (const gchar * name, gint max_width)
{
  if (!name)
    return g_strdup ("");

  gint name_len = g_utf8_strlen (name, -1);
  if (name_len <= max_width)
    return g_strdup (name);

  /* For filenames, try to keep the extension and truncate from the middle */
  gchar *dot = g_strrstr (name, ".");
  if (dot && (dot - name) > 3) {
    gchar *extension = g_strdup (dot);
    gint ext_len = g_utf8_strlen (extension, -1);
    gint available_for_name = max_width - ext_len - 3;  /* -3 for "..." */

    if (available_for_name > 3) {
      gchar *truncated = g_malloc0 (max_width + 1);
      gchar *end_ptr = g_utf8_offset_to_pointer (name, available_for_name);
      g_strlcpy (truncated, name, end_ptr - name + 1);
      g_strlcat (truncated, "…", max_width + 1);
      g_strlcat (truncated, extension, max_width + 1);
      g_free (extension);
      return truncated;
    }
    g_free (extension);
  }

  /* Fallback: simple truncation from end */
  gchar *truncated = g_malloc0 (max_width + 1);
  gchar *end_ptr = g_utf8_offset_to_pointer (name, max_width - 1);
  g_strlcpy (truncated, name, end_ptr - name + 1);
  g_strlcat (truncated, "…", max_width + 1);
  return truncated;
}

void
print_timeline (GESTimeline * timeline)
{
  gchar *uri;
  GList *layer, *clip, *clips;
  GstClockTime timeline_duration = 0;
  gint terminal_width = get_terminal_width ();
  gint timeline_width = 70;
  gint max_content_width = 0;

  if (!timeline->layers)
    return;

  /* Calculate timeline duration and find the longest content line */
  for (layer = timeline->layers; layer; layer = layer->next) {
    clips = ges_layer_get_clips (layer->data);
    for (clip = clips; clip; clip = clip->next) {
      GstClockTime clip_end = GES_TIMELINE_ELEMENT_END (clip->data);
      timeline_duration = MAX (timeline_duration, clip_end);

      /* Calculate content width needed for this clip */
      gchar *clip_name = NULL;
      if (GES_IS_URI_CLIP (clip->data)) {
        GESUriClipAsset *asset =
            GES_URI_CLIP_ASSET (ges_extractable_get_asset (clip->data));
        const gchar *full_path = ges_asset_get_id (GES_ASSET (asset));
        gchar *filename = g_path_get_basename (full_path);
        gchar *asset_desc =
            describe_discoverer (ges_uri_clip_asset_get_info (asset));

        clip_name = g_strdup (filename);
        g_free (filename);
        g_free (asset_desc);
      } else {
        clip_name = g_strdup (GES_TIMELINE_ELEMENT_NAME (clip->data));
      }

      if (clip_name) {
        gint content_width = g_utf8_strlen (clip_name, -1) + 30;
        max_content_width = MAX (max_content_width, content_width);
        g_free (clip_name);
      }
    }
    g_list_free_full (clips, gst_object_unref);
  }

  /* Set timeline width based on content but respect terminal width */
  timeline_width = MAX (timeline_width, max_content_width);
  timeline_width = MIN (timeline_width, terminal_width - 4);    /* -4 for borders and padding */


  uri = ges_command_line_formatter_get_timeline_uri (timeline);

  gint title_len = g_utf8_strlen ("🎬 TIMELINE OVERVIEW", -1);
  gint left_padding = (timeline_width - title_len - 2) / 2;
  gint right_padding = timeline_width - title_len - 2 - left_padding;

  gchar *left_border = repeat_utf8_char ("═", left_padding);
  gchar *right_border = repeat_utf8_char ("═", right_padding);
  gst_print ("%s 🎬 TIMELINE OVERVIEW %s\n", left_border, right_border);
  g_free (left_border);
  g_free (right_border);

  gchar *formatted_desc = format_timeline_description (&uri[5]);
  gst_print ("Command: %s\n\n", formatted_desc);
  g_free (formatted_desc);
  g_free (uri);

  for (layer = timeline->layers; layer; layer = layer->next) {
    clips = ges_layer_get_clips (layer->data);

    if (!clips)
      continue;

    GstClockTime layer_end = 0;

    for (clip = clips; clip; clip = clip->next) {
      GstClockTime clip_end = GES_TIMELINE_ELEMENT_END (clip->data);
      layer_end = MAX (layer_end, clip_end);
    }

    /* Print layer header with duration */
    gchar *left_border = repeat_utf8_char ("─", 8);
    gchar *middle_border = repeat_utf8_char ("─", 13);
    gchar *right_border = repeat_utf8_char ("─", timeline_width - 51);

    /* Use appropriate junction character based on position */
    const gchar *junction = layer->prev ? "┼" : (layer->next ? "┬" : "─");

    gst_print ("%s%s%s Duration: %" GST_TIME_FORMAT " %s\n",
        left_border, junction, middle_border, GST_TIME_ARGS (layer_end),
        right_border);
    g_free (left_border);
    g_free (middle_border);
    g_free (right_border);


    gboolean first_clip = TRUE;
    for (clip = clips; clip; clip = clip->next) {
      gchar *name;

      gchar *asset_desc = NULL;
      if (GES_IS_URI_CLIP (clip->data)) {
        GESUriClipAsset *asset =
            GES_URI_CLIP_ASSET (ges_extractable_get_asset (clip->data));
        const gchar *full_path = ges_asset_get_id (GES_ASSET (asset));
        gchar *filename = g_path_get_basename (full_path);
        asset_desc = describe_discoverer (ges_uri_clip_asset_get_info (asset));

        /* Truncate filename to fit in timeline width */
        gint max_name_width = timeline_width - 20;
        name = truncate_clip_name (filename, max_name_width);
        g_free (filename);
      } else {
        gchar *clip_name = GES_TIMELINE_ELEMENT_NAME (clip->data);
        gint max_name_width = timeline_width - 20;
        name = truncate_clip_name (clip_name, max_name_width);
      }
      gchar *rates = ges_clip_get_time_effects_rates (clip->data);

      GESTrackType supported_types =
          ges_clip_get_supported_formats (clip->data);
      GString *track_types = g_string_new ("");
      if (supported_types & GES_TRACK_TYPE_VIDEO)
        g_string_append (track_types, "📹");
      if (supported_types & GES_TRACK_TYPE_AUDIO)
        g_string_append (track_types, "🔊");

      /* Add clip timeline bar as header */
      gchar *clip_bar =
          create_clip_bar (clip->data, layer_end, timeline_width - 10);

      if (first_clip) {
        gst_print ("Layer %d │\n", ges_layer_get_priority (layer->data));
        gst_print ("        │ %s\n", clip_bar);
        first_clip = FALSE;
      } else {
        gst_print ("        │\n");
        gst_print ("        │ %s\n", clip_bar);
      }
      g_free (clip_bar);

      gst_print ("        │ %s  %s\n", name, rates);

      if (asset_desc) {
        gst_print ("        │         %s\n", asset_desc);
        g_free (asset_desc);
      }

      /* Format time components */
      gchar *start_time = g_strdup_printf ("%" GST_TIME_FORMAT,
          GST_TIME_ARGS (GES_TIMELINE_ELEMENT_START (clip->data)));
      gchar *end_time = g_strdup_printf ("%" GST_TIME_FORMAT,
          GST_TIME_ARGS (GES_TIMELINE_ELEMENT_END (clip->data)));
      gchar *inpoint_str = NULL;

      if (GES_TIMELINE_ELEMENT_INPOINT (clip->data)) {
        inpoint_str = g_strdup_printf ("(inpoint: %" GST_TIME_FORMAT ")",
            GST_TIME_ARGS (GES_TIMELINE_ELEMENT_INPOINT (clip->data)));
      }

      /* Calculate available width and spacing */
      gint available_width = timeline_width - 13;       /* Account for "        │     " prefix */
      gint start_len = g_utf8_strlen (start_time, -1);
      gint end_len = g_utf8_strlen (end_time, -1);
      gint inpoint_len = inpoint_str ? g_utf8_strlen (inpoint_str, -1) : 0;

      if (inpoint_str) {
        /* Three components: start, inpoint (center), end */
        gint remaining_width =
            available_width - start_len - end_len - inpoint_len;
        if (remaining_width > 0) {
          gint left_padding = remaining_width / 2;
          gint right_padding = remaining_width - left_padding;
          gchar *left_spaces = g_strnfill (left_padding, ' ');
          gchar *right_spaces = g_strnfill (right_padding, ' ');
          gst_print ("        │%s %s%s%s%s\n", start_time, left_spaces,
              inpoint_str, right_spaces, end_time);
          g_free (left_spaces);
          g_free (right_spaces);
        } else {
          /* Fallback if not enough space */
          gst_print ("        │%s %s %s\n", start_time, inpoint_str,
              end_time);
        }
      } else {
        gint remaining_width = available_width - start_len - end_len;

        if (remaining_width > 0) {
          gchar *spaces = g_strnfill (remaining_width, ' ');
          gst_print ("        │%s%s%s\n", start_time, spaces, end_time);
          g_free (spaces);
        } else {
          gst_print ("        │%s %s\n", start_time, end_time);
        }
      }

      g_free (start_time);
      g_free (end_time);
      g_free (inpoint_str);

      g_free (name);
      g_free (rates);
      g_string_free (track_types, TRUE);
    }

    /* Only print bottom border if it is the last layer */
    if (!layer->next) {
      gchar *bottom_left = repeat_utf8_char ("─", 8);
      gchar *bottom_right = repeat_utf8_char ("─", timeline_width - 9);
      gst_print ("%s┴%s\n", bottom_left, bottom_right);
      g_free (bottom_left);
      g_free (bottom_right);
    } else {
      gst_print ("        │\n");
    }

    g_list_free_full (clips, gst_object_unref);
  }

  gst_print ("\n");
}
