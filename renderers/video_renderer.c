/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-24 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/video/video-overlay-composition.h>
#include "video_renderer.h"
#include "direct_playback_state.h"
#include "playback_diagnostics.h"
#include "hls_gap_repair.h"

#define SECOND_IN_NSECS 1000000000UL
#define SECOND_IN_MICROSECS 1000000
#ifdef X_DISPLAY_FIX
#include <gst/video/navigation.h>
#include "x_display_fix.h"
static bool fullscreen = false;
static bool alt_keypress = false;
static unsigned char X11_search_attempts = 0;
#endif

static GstClockTime gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
static unsigned short width, height, width_source, height_source;  /* not currently used */
static bool first_packet = false;
static bool sync = false;
static bool auto_videosink = true;
static bool hls_video = false;
#ifdef X_DISPLAY_FIX
static bool use_x11 = false;
#endif
static bool logger_debug = false;
static gint64 hls_requested_start_position = 0;
static gint64 hls_seek_start = 0;
static gint64 hls_seek_end = 0;
static gint64 hls_duration = 0;
static gboolean hls_seek_enabled = FALSE;
static gboolean hls_playing = FALSE;
static gboolean hls_buffer_empty = FALSE;
static gboolean hls_buffer_full = FALSE;
/* HTTP controls and the GLib bus/lifecycle run on different threads. */
static GRecMutex direct_mutex;
static direct_playback_state_t direct_state;
static gboolean direct_request_pending = FALSE;
static guint64 direct_generation = 0;
static gint64 direct_requested_at = 0;
static screen_info_mode_t screen_mode = SCREEN_INFO_OFF;
static gboolean screen_output_suspended = FALSE;
static int type_264 = 0;
static int type_265 = 0;
static int type_hls = 0;
static int type_jpeg = 0;

typedef enum {
  //GST_PLAY_FLAG_VIDEO         = (1 << 0),
  //GST_PLAY_FLAG_AUDIO         = (1 << 1),
  //GST_PLAY_FLAG_TEXT          = (1 << 2),
  //GST_PLAY_FLAG_VIS           = (1 << 3),
  //GST_PLAY_FLAG_SOFT_VOLUME   = (1 << 4),
  //GST_PLAY_FLAG_NATIVE_AUDIO  = (1 << 5),
  //GST_PLAY_FLAG_NATIVE_VIDEO  = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD      = (1 << 7),
  GST_PLAY_FLAG_BUFFERING     = (1 << 8),
  //GST_PLAY_FLAG_DEINTERLACE   = (1 << 9),
  //GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10),
  //GST_PLAY_FLAG_FORCE_FILTERS = (1 << 11),
  //GST_PLAY_FLAG_FORCE_SW_DECODERS = (1 << 12),
} GstPlayFlags;

#define NCODECS  3   /* renderers for h264,h265, and jpeg images */

struct video_renderer_s {
    GstElement *appsrc, *pipeline, *textsrc;
    GstBus *bus;
    const char *codec;
    bool autovideo;
    int id;
    char *uri;
    gboolean eos;
    gint64 duration;
    gint buffering_level;
    guint64 direct_generation;
    gboolean direct_started;
    gboolean screen_seek_pending;
    playback_diagnostics_t *diagnostics;
    hls_gap_repair_t *gap_repair;
    gboolean gap_only_audio;
    GstClockTime startup_buffer_time;
    gulong buffering_handler;
    guint64 screen_generation;
    GMutex overlay_lock;
    GstVideoOverlayComposition *overlay;
    GstElement *overlay_pipeline, *overlay_source, *overlay_text, *overlay_sink;
    guint64 overlay_frame;
    guint overlay_render_failures;
    gchar overlay_last_text[2048];
    gboolean overlay_known, overlay_supported, overlay_logged, overlay_failure_logged;
    gchar overlay_reason[48];
    gboolean screen_output_announced;
#ifdef  X_DISPLAY_FIX
    bool use_x11;
    const char * server_name;
    X11_Window_t * gst_window;
#endif
};

static video_renderer_t *renderer = NULL;
static video_renderer_t *renderer_type[NCODECS] = {0};
static int n_renderers = NCODECS;
static char h264[] = "h264";
static char h265[] = "h265";
static char hls[]  = "hls";
static char jpeg[] = "jpeg";
static gboolean pi4_profile = FALSE;
static gboolean direct_http_source = FALSE;

/* These pixels are generated offscreen on the main-loop side. The live
 * pipeline receives no new element or caps constraint: decoder selection and
 * negotiated memory remain exactly its own. Blending is explicitly a CPU copy
 * path, never an end-to-end zero-copy claim. */
#define SCREEN_PANEL_WIDTH 960
#define SCREEN_PANEL_HEIGHT 300

static gchar *screen_wrap_panel(const gchar *text, guint columns, guint *lines, guint *widest) {
    GString *wrapped = g_string_new(NULL);
    guint used = 0;
    *lines = 1;
    *widest = 0;
    const gchar *p = text;
    while (*p) {
        if (*p == '\n') {
            *widest = MAX(*widest, used);
            g_string_append_c(wrapped, '\n');
            used = 0; (*lines)++; p++; continue;
        }
        if (*p == ' ') { p++; continue; }
        const gchar *end = p;
        guint word_width = 0;
        while (*end && *end != ' ' && *end != '\n') {
            word_width += g_unichar_iswide(g_utf8_get_char(end)) ? 2 : 1;
            end = g_utf8_next_char(end);
        }
        if (used && used + 1 + word_width > columns) {
            *widest = MAX(*widest, used);
            g_string_append_c(wrapped, '\n'); used = 0; (*lines)++;
        }
        if (used) { g_string_append_c(wrapped, ' '); used++; }
        while (p < end) {
            guint cells = g_unichar_iswide(g_utf8_get_char(p)) ? 2 : 1;
            if (used && used + cells > columns) {
                *widest = MAX(*widest, used);
                g_string_append_c(wrapped, '\n'); used = 0; (*lines)++;
            }
            const gchar *next = g_utf8_next_char(p);
            g_string_append_len(wrapped, p, next - p);
            used += cells;
            p = next;
        }
    }
    *widest = MAX(*widest, used);
    return g_string_free(wrapped, FALSE);
}

static void screen_overlay_result(video_renderer_t *owner, gboolean supported,
                                  const gchar *reason) {
    owner->overlay_known = TRUE;
    owner->overlay_supported = supported;
    g_strlcpy(owner->overlay_reason, reason, sizeof(owner->overlay_reason));
}

static gboolean screen_buffer_is_system_memory(GstBuffer *buffer) {
    if (!gst_buffer_n_memory(buffer)) return FALSE;
    for (guint i = 0; i < gst_buffer_n_memory(buffer); i++) {
        if (!gst_memory_is_type(gst_buffer_peek_memory(buffer, i), "SystemMemory")) return FALSE;
    }
    return TRUE;
}

static gboolean screen_overlay_blend(video_renderer_t *owner, GstBuffer *buffer,
                                     const GstVideoInfo *video_info) {
    if (!screen_buffer_is_system_memory(buffer)) {
        const gchar *reason = "Opaque buffer overlay unqualified";
        for (guint i = 0; i < gst_buffer_n_memory(buffer); i++) {
            GstMemory *memory = gst_buffer_peek_memory(buffer, i);
            if (gst_memory_is_type(memory, "dmabuf")) { reason = "DMABuf overlay unqualified"; break; }
            if (gst_memory_is_type(memory, "GLMemory")) reason = "GLMemory overlay unqualified";
        }
        screen_overlay_result(owner, FALSE, reason);
        return FALSE;
    }
    if (!owner->overlay) return TRUE; /* normal mode intentionally has no panel */
    if (!gst_buffer_is_writable(buffer)) {
        screen_overlay_result(owner, FALSE, "Buffer is not writable");
        return FALSE;
    }
    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, video_info, buffer, GST_MAP_READWRITE)) {
        screen_overlay_result(owner, FALSE, "Buffer mapping unavailable");
        return FALSE;
    }
    gboolean blended = gst_video_overlay_composition_blend(owner->overlay, &frame);
    gst_video_frame_unmap(&frame);
    screen_overlay_result(owner, blended, blended ? "SystemMemory CPU blend" : "Pixel format blend unsupported");
    return blended;
}

static void screen_present_video_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    video_renderer_t *owner = data;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    GstVideoInfo video_info;
    gboolean raw = caps && gst_video_info_from_caps(&video_info, caps);
    g_mutex_lock(&owner->overlay_lock);
    if (!raw) {
        screen_overlay_result(owner, FALSE, "Raw video caps unavailable");
    } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        /* Never map/import or copy opaque decoder memory to draw text. */
        if (owner->overlay && screen_buffer_is_system_memory(buffer)) {
            buffer = gst_buffer_make_writable(buffer);
            GST_PAD_PROBE_INFO_DATA(info) = buffer;
        }
        screen_overlay_blend(owner, buffer, &video_info);
    } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
        if (owner->overlay) list = gst_buffer_list_make_writable(list);
        GST_PAD_PROBE_INFO_DATA(info) = list;
        for (guint i = 0; i < gst_buffer_list_length(list); i++) {
            GstBuffer *buffer = gst_buffer_list_get(list, i);
            if (owner->overlay && screen_buffer_is_system_memory(buffer)) buffer = gst_buffer_list_get_writable(list, i);
            screen_overlay_blend(owner, buffer, &video_info);
        }
    }
    g_mutex_unlock(&owner->overlay_lock);
    if (caps) gst_caps_unref(caps);
}

static gboolean screen_overlay_create(video_renderer_t *owner) {
#if GST_CHECK_VERSION(1, 10, 0)
    owner->overlay_pipeline = gst_pipeline_new(NULL);
    owner->overlay_source = gst_element_factory_make("appsrc", NULL);
    owner->overlay_text = gst_element_factory_make("textoverlay", NULL);
    owner->overlay_sink = gst_element_factory_make("appsink", NULL);
    if (!owner->overlay_pipeline || !owner->overlay_source || !owner->overlay_text || !owner->overlay_sink) {
        if (owner->overlay_source) gst_object_unref(owner->overlay_source);
        if (owner->overlay_text) gst_object_unref(owner->overlay_text);
        if (owner->overlay_sink) gst_object_unref(owner->overlay_sink);
        if (owner->overlay_pipeline) gst_object_unref(owner->overlay_pipeline);
        owner->overlay_pipeline = owner->overlay_source = owner->overlay_text = owner->overlay_sink = NULL;
        return FALSE;
    }
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_OVERLAY_COMPOSITION_FORMAT_RGB,
                             SCREEN_PANEL_WIDTH, SCREEN_PANEL_HEIGHT);
    info.fps_n = 1;
    info.fps_d = 1;
    GstCaps *caps = gst_video_info_to_caps(&info);
    g_object_set(owner->overlay_source, "caps", caps, "format", GST_FORMAT_TIME,
                 "is-live", FALSE, "block", FALSE, NULL);
    gst_caps_unref(caps);
    g_object_set(owner->overlay_text, "text", "", "font-desc", "Monospace 20px", "auto-resize", FALSE,
                 "halignment", 0, "valignment", 2, "xpad", 18, "ypad", 12,
                 "line-alignment", 0,
                 "shaded-background", FALSE, "draw-shadow", FALSE, "draw-outline", FALSE, NULL);
    gst_util_set_object_arg(G_OBJECT(owner->overlay_text), "wrap-mode", "none");
    g_object_set(owner->overlay_sink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, NULL);
    gst_bin_add_many(GST_BIN(owner->overlay_pipeline), owner->overlay_source,
                     owner->overlay_text, owner->overlay_sink, NULL);
    if (!gst_element_link_many(owner->overlay_source, owner->overlay_text, owner->overlay_sink, NULL) ||
        gst_element_set_state(owner->overlay_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(owner->overlay_pipeline, GST_STATE_NULL);
        gst_object_unref(owner->overlay_pipeline);
        owner->overlay_pipeline = owner->overlay_source = owner->overlay_text = owner->overlay_sink = NULL;
        return FALSE;
    }
    return TRUE;
#else
    (void) owner;
    return FALSE;
#endif
}

static GstVideoOverlayComposition *screen_overlay_render(video_renderer_t *owner,
    const gchar *text, guint width, guint height) {
#if GST_CHECK_VERSION(1, 10, 0)
    if (!owner->overlay_pipeline && !screen_overlay_create(owner)) return NULL;
    /* The common formatter returns plain text. Escape Pango independently so
     * neither markup nor a sender name can become a rendering instruction. */
    guint font_pixels = screen_mode == SCREEN_INFO_STATUS ? 32 : 20;
    guint lines = 0, widest = 0;
    gchar *wrapped = NULL;
    /* Fit the bounded full snapshot, not a one-line sample. Absolute font
     * sizes and explicit wrapping avoid textoverlay's auto-resize multiplier
     * and its wrap width ignoring xpad. Long diagnostic names reduce the font
     * modestly before any field is clipped. */
    for (;;) {
        guint columns = (SCREEN_PANEL_WIDTH - 36) * 100 / (font_pixels * 62);
        wrapped = screen_wrap_panel(text, columns, &lines, &widest);
        if (lines * font_pixels * 135 / 100 + 24 <= SCREEN_PANEL_HEIGHT || font_pixels <= 14) break;
        g_free(wrapped);
        font_pixels -= 2;
    }
    gchar *escaped = g_markup_escape_text(wrapped, -1);
    gchar *font = g_strdup_printf("Monospace %upx", font_pixels);
    g_object_set(owner->overlay_text, "font-desc", font, "text", escaped, NULL);
    g_free(font);
    g_free(wrapped);
    g_free(escaped);
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_OVERLAY_COMPOSITION_FORMAT_RGB,
                             SCREEN_PANEL_WIDTH, SCREEN_PANEL_HEIGHT);
    GstBuffer *blank = gst_buffer_new_allocate(NULL, info.size, NULL);
    GstMapInfo map;
    if (!gst_buffer_map(blank, &map, GST_MAP_WRITE)) { gst_buffer_unref(blank); return NULL; }
    /* Native composition RGB is BGRA on little endian and ARGB on big endian. */
    guint backing_width = MIN(SCREEN_PANEL_WIDTH, widest * font_pixels * 62 / 100 + 36);
    guint backing_height = MIN(SCREEN_PANEL_HEIGHT, lines * font_pixels * 135 / 100 + 24);
    for (gsize offset = 0; offset + 3 < map.size; offset += 4) {
        guint x = (offset / 4) % SCREEN_PANEL_WIDTH, y = (offset / 4) / SCREEN_PANEL_WIDTH;
        guint32 alpha = x < backing_width && y < backing_height ? 0xa0000000 : 0;
        guint32 pixel = GUINT32_TO_LE(alpha);
#if G_BYTE_ORDER == G_BIG_ENDIAN
        pixel = GUINT32_TO_BE(alpha);
#endif
        memcpy(map.data + offset, &pixel, sizeof(pixel));
    }
    gst_buffer_unmap(blank, &map);
    GST_BUFFER_PTS(blank) = owner->overlay_frame++ * GST_SECOND;
    GST_BUFFER_DURATION(blank) = GST_SECOND;
    if (gst_app_src_push_buffer(GST_APP_SRC(owner->overlay_source), blank) != GST_FLOW_OK) return NULL;
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(owner->overlay_sink), 250 * GST_MSECOND);
    if (!sample) return NULL;
    GstBuffer *pixels = gst_buffer_copy(gst_sample_get_buffer(sample));
    if (!gst_buffer_get_video_meta(pixels))
        gst_buffer_add_video_meta(pixels, GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_OVERLAY_COMPOSITION_FORMAT_RGB, SCREEN_PANEL_WIDTH, SCREEN_PANEL_HEIGHT);
    guint render_width = MAX(1u, width * 9 / 10);
    guint render_height = MAX(1u, render_width * SCREEN_PANEL_HEIGHT / SCREEN_PANEL_WIDTH);
    render_height = MIN(render_height, MAX(1u, height * 9 / 10));
    GstVideoOverlayRectangle *rectangle = gst_video_overlay_rectangle_new_raw(pixels,
        width / 20, height / 20, render_width, render_height, GST_VIDEO_OVERLAY_FORMAT_FLAG_NONE);
    GstVideoOverlayComposition *composition = rectangle ? gst_video_overlay_composition_new(rectangle) : NULL;
    if (rectangle) gst_video_overlay_rectangle_unref(rectangle);
    gst_buffer_unref(pixels);
    gst_sample_unref(sample);
    return composition;
#else
    (void) owner; (void) text; (void) width; (void) height;
    return NULL;
#endif
}

void video_renderer_configure_screen(screen_info_mode_t mode) {
    g_rec_mutex_lock(&direct_mutex);
    screen_mode = mode;
    g_rec_mutex_unlock(&direct_mutex);
}

void video_renderer_configure_pi4(logger_t *profile_logger) {
    pi4_profile = TRUE;
    /* The Pi 4 stateless HEVC driver can wait indefinitely for an IRQ during
     * STREAMOFF. Once that happens, even SIGKILL cannot reclaim the decoder,
     * and the renderer's teardown prevents later H.264 sessions from starting.
     * Exclude this exact factory before decodebin caches its candidate list.
     * This changes only this process, leaving H.264 acceleration available. */
    GstElementFactory *hardware = gst_element_factory_find("v4l2slh265dec");
    if (hardware) {
        gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(hardware), GST_RANK_NONE);
        gst_object_unref(hardware);
    }
    GstElementFactory *software = gst_element_factory_find("avdec_h265");
    logger_log(profile_logger, software ? LOGGER_INFO : LOGGER_WARNING,
               "Pi 4 profile: excluded v4l2slh265dec to avoid HEVC driver shutdown hangs; "
               "software_hevc_available=%d; H.264 decoder selection unchanged", software != NULL);
    if (software) gst_object_unref(software);
}

static void configure_direct_buffering(GstBin *pipeline, GstBin *parent,
                                        GstElement *element, gpointer data) {
    (void)pipeline;
    (void)parent;
    video_renderer_t *owner = data;
    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory || !owner->startup_buffer_time ||
        strcmp(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "hlsdemux2")) return;
    GParamSpec *property = g_object_class_find_property(G_OBJECT_GET_CLASS(element), "low-watermark-time");
    if (!property || G_PARAM_SPEC_VALUE_TYPE(property) != G_TYPE_UINT64 ||
        !(property->flags & G_PARAM_WRITABLE)) return;
    /* Direct sources can publish one short live fragment at a time. Waiting
     * for the adaptive demuxer's default ten seconds can require multiple
     * publication cycles. Keep its 30s download ceiling and ordinary buffering
     * state machine, but allow this Pi profile to start with three seconds.
     * Cached YouTube playback retains its existing buffering configuration. */
    g_object_set(element, "low-watermark-time", (guint64)owner->startup_buffer_time, NULL);
    logger_log(logger, LOGGER_INFO, "Direct playback: session=%" G_GUINT64_FORMAT
               " stage=buffering-profile route=direct-http minimum_ms=%" G_GUINT64_FORMAT,
               owner->direct_generation, owner->startup_buffer_time / GST_MSECOND);
}

/* Called with direct_mutex held from the main-loop bus watch or a serialized
 * control, never a streaming pad callback. Keep the renderer shell for normal
 * replacement, but release the failed pipeline's downloads and decoders. */
static void direct_fail_session(video_renderer_t *owner) {
    if (!hls_video || owner->direct_generation != direct_generation || direct_state.failed) return;
    direct_playback_state_fail(&direct_state);
    owner->direct_started = FALSE;
    owner->eos = FALSE;
    owner->gap_only_audio = FALSE;
    hls_playing = FALSE;
    hls_buffer_empty = TRUE;
    hls_buffer_full = FALSE;
    hls_seek_enabled = FALSE;
    hls_requested_start_position = 0;
    if (screen_mode != SCREEN_INFO_OFF && owner->screen_generation)
        screen_status_fail(owner->screen_generation, SCREEN_ERROR_BACKEND);
    GstStateChangeReturn result = gst_element_set_state(owner->pipeline, GST_STATE_NULL);
    logger_log(logger, LOGGER_INFO, "Direct playback: session=%" G_GUINT64_FORMAT
               " stage=failed action=stop state=NULL result=%s", direct_generation,
               gst_element_state_change_return_get_name(result));
}

/* Called with direct_mutex held. Buffering never changes requested intent. */
static void direct_apply_state(void) {
    if (!renderer || !hls_video || direct_state.failed || !renderer->direct_started ||
        renderer->direct_generation != direct_generation) {
        return;
    }
    direct_playback_target_t target = direct_playback_state_target(&direct_state);
    gboolean gap_only = hls_gap_repair_gap_only_audio(renderer->gap_repair);
    if (gap_only != renderer->gap_only_audio) {
        renderer->gap_only_audio = gap_only;
        logger_log(logger, LOGGER_INFO, "Direct playback: session=%" G_GUINT64_FORMAT
                   " stage=gap-only-audio active=%d", direct_generation, gap_only);
    }
    /* Until the first real audio sample, decodebin3 has no audio sink to hold
     * timed GAP events. Its empty adaptive audio queue cannot describe video
     * readiness. The GAP events still bound demux output to completed media;
     * resume normal buffering as soon as real audio arrives. */
    if (gap_only && direct_state.preroll_complete && direct_state.intent == DIRECT_PLAYBACK_PLAYING)
        target = DIRECT_PLAYBACK_PLAYING;
    GstState desired = target == DIRECT_PLAYBACK_PLAYING ? GST_STATE_PLAYING :
                       target == DIRECT_PLAYBACK_PAUSED ? GST_STATE_PAUSED : GST_STATE_READY;
    GstState current, pending;
    gst_element_get_state(renderer->pipeline, &current, &pending, 0);
    if (pending == desired || (current == desired && pending == GST_STATE_VOID_PENDING)) {
        return;
    }
    GstStateChangeReturn result = gst_element_set_state(renderer->pipeline, desired);
    if (result == GST_STATE_CHANGE_FAILURE) {
        direct_fail_session(renderer);
        return;
    }
    if (result == GST_STATE_CHANGE_NO_PREROLL) {
        direct_state.live = true;
        if (direct_state.intent == DIRECT_PLAYBACK_PLAYING && desired == GST_STATE_PAUSED) {
            gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING);
        }
    }
}

static void append_videoflip (GString *launch, const videoflip_t *flip, const videoflip_t *rot) {
    /* videoflip image transform */
    switch (*flip) {
    case INVERT:
        switch (*rot)  {
        case LEFT:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
	    break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        default:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_180 ! ");
	    break;
        }
        break;
    case HFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_HORIZ ! ");
            break;
        }
        break;
    case VFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_VERT ! ");
	  break;
	}
        break;
    default:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
            break;
        default:
            break;
        }
        break;
    }
}

/* apple uses colorimetry that is detected as  1:3:7:1           * //previously 1:3:5:1 was seen
 * (not recognized by v4l2 plugin in Gstreamer  < 1.20.4)        *
 * See .../gst-libs/gst/video/video-color.h in gst-plugins-base  *
 * range = 1   -> GST_VIDEO_COLOR_RANGE_0_255      ("full RGB")  * 
 * matrix = 3  -> GST_VIDEO_COLOR_MATRIX_BT709                   *
 * transfer = 7 -> GST_VIDEO_TRANSFER_SRGB                       * // previously GST_VIDEO_TRANSFER_BT709
 * primaries = 1 -> GST_VIDEO_COLOR_PRIMARIES_BT709              *
 * closest used by  GStreamer < 1.20.4 is BT709, 2:3:5:1 with    * // now use sRGB = 1:1:7:1    
 * range = 2 -> GST_VIDEO_COLOR_RANGE_16_235 ("limited RGB")     */  

static const char jpeg_caps[]="image/jpeg";
static const char h264_caps[]="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[]="video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height) {
    width_source = (unsigned short) *f_width_source;
    height_source = (unsigned short) *f_height_source;
    width = (unsigned short) *f_width;
    height = (unsigned short) *f_height;
    logger_log(logger, LOGGER_DEBUG, "begin video stream wxh = %dx%d; source %dx%d", width, height, width_source, height_source);
}

GstElement *make_video_sink(const char *videosink, const char *videosink_options) {
    /* used to build a videosink for playbin, using the user-specified string "videosink" */ 
    GstElement *video_sink = gst_element_factory_make(videosink, "videosink");
    if (!video_sink) {
        return NULL;
    }

    /* process the video_sink_options */
    size_t len = strlen(videosink_options);
    if (!len) {
        return video_sink;
    }

    char *options  = (char *) malloc(len + 1);
    strncpy(options, videosink_options, len + 1);

    /* remove any extension begining with "!" */
    char *end = strchr(options, '!');
    if (end) {   
      *end = '\0';
    }

    /* add any fullscreen options "property=pval" included in string videosink_options*/    
    /* OK to use strtok_r in Windows with MSYS2 (POSIX); use strtok_s for MSVC */
    char *token = NULL;
    char *text = options;

    while((token = strtok_r(text, " ", &text))) {
	char *pval = strchr(token, '=');
        if (pval) {
            *pval = '\0';
            pval++;
            const gchar *property_name = (const gchar *) token;
            const gchar *value = (const gchar *) pval;
            g_print("playbin_videosink property: \"%s\" \"%s\"\n", property_name, value);
            gst_util_set_object_arg(G_OBJECT (video_sink), property_name, value);
        }
    }
    free(options);
    return video_sink;
}

#ifdef NEED_G_STRING_REPLACE
guint
g_string_replace (GString     *string,
                  const gchar *find,
                  const gchar *replace,
                  guint        limit)
{
  if (find == NULL || find[0] == '\0')
    return 0;

  gchar **parts = g_strsplit (string->str, find, limit + 1);
  if (parts == NULL || parts[0] == NULL)
    {
      g_strfreev (parts);
      return 0;
    }

  gchar *joined = g_strjoinv (replace, parts);
  g_strfreev (parts);

  g_string_assign (string, joined);
  g_free (joined);

  return g_strv_length (parts);
}
#endif

static void video_renderer_init_unlocked(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser, const char * rtp_pipeline,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options, 
                          bool initial_fullscreen, bool video_sync, bool h265_support, bool coverart_support, guint playbin_version, const char *uri) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    bool rtp = (bool) strlen(rtp_pipeline);
    hls_video = (uri != NULL);
    /* videosink choices that are auto */
    auto_videosink = (strstr(videosink, "autovideosink") || strstr(videosink, "fpsdisplaysink"));

    logger = render_logger;
    logger_debug = (logger_get_level(logger) >= LOGGER_DEBUG);
    hls_seek_enabled = FALSE;
    hls_playing = FALSE;
    hls_seek_start = -1;
    hls_seek_end = -1;
    hls_duration = -1;
    hls_buffer_empty = TRUE;
    hls_buffer_full = FALSE;
    if (hls_video) {
        if (!direct_request_pending) {
            direct_playback_state_reset(&direct_state);
            direct_requested_at = g_get_monotonic_time();
            direct_generation++;
        }
        direct_request_pending = FALSE;
    }
    type_hls = -1;
    type_264 = -1;
    type_265 = -1;
    type_jpeg = -1;

    /* this call to g_set_application_name makes server_name appear in the  X11 display window title bar, */
    /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname,server_name))  g_set_application_name(server_name);
    appname = NULL;
    n_renderers = 1;
    /* the renderer for hls video will only be built if a HLS uri is provided in 
     * the call to video_renderer_init, in which case the h264/h265 mirror-mode and jpeg
     * audio-mode renderers will not be built.   This is because it appears that we cannot  
     * put playbin into GST_STATE_READY before knowing the uri (?), so cannot use a
     * unified renderer structure with h264, h265, jpeg and hls  */  
    if (hls_video) {
        type_hls = 0;
    } else {
        type_264 = 0;
        if (h265_support) {
            type_265 = n_renderers++;
        }
        if (coverart_support) {
            type_jpeg = n_renderers++;
        }
    }
    g_assert (n_renderers <= NCODECS);
    for (int i = 0; i < n_renderers; i++) {
        g_assert (i < 3);
        renderer_type[i] = (video_renderer_t *) calloc(1, sizeof(video_renderer_t));
        g_assert(renderer_type[i]);
        g_mutex_init(&renderer_type[i]->overlay_lock);
        if (screen_mode != SCREEN_INFO_OFF && hls_video) {
            screen_status_snapshot_t snapshot;
            screen_status_get_snapshot(&snapshot);
            renderer_type[i]->screen_generation = snapshot.generation;
        }
        renderer_type[i]->autovideo = auto_videosink;
        renderer_type[i]->id = i;
        renderer_type[i]->bus = NULL;
        renderer_type[i]->buffering_level = -1;
        renderer_type[i]->appsrc = NULL;
        renderer_type[i]->textsrc = NULL;
        renderer_type[i]->uri = NULL;
        renderer_type[i]->eos = FALSE;
        if (hls_video) {
            renderer_type[i]->direct_generation = direct_generation;
            renderer_type[i]->uri = (char *) calloc(strlen(uri) + 1, sizeof(char));
            memcpy(renderer_type[i]->uri, uri, strlen(uri));
            /* use playbin3 to play HLS video: replace "playbin3" by "playbin" to use playbin2 */
            switch (playbin_version)  {
            case 2:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin", "hls-playbin2");
                break;
            case 3:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin3", "hls-playbin3");
                break;
            default:
                logger_log(logger, LOGGER_ERR, "video_renderer_init: invalid playbin version %u", playbin_version);
                g_assert(0);
            }
            logger_log(logger, LOGGER_INFO, "Will use GStreamer playbin version %u to play HLS streamed video", playbin_version);
            g_assert(renderer_type[i]->pipeline);
            renderer_type[i]->codec = hls;
            renderer_type[i]->startup_buffer_time = pi4_profile && direct_http_source ? 3 * GST_SECOND : 0;
            if (renderer_type[i]->startup_buffer_time &&
                g_signal_lookup("deep-element-added", GST_TYPE_BIN)) {
                renderer_type[i]->buffering_handler = g_signal_connect(
                    renderer_type[i]->pipeline, "deep-element-added",
                    G_CALLBACK(configure_direct_buffering), renderer_type[i]);
            }
            /* if we are not using an autovideosink, build a videosink based on the string "videosink" */
            if (!auto_videosink) { 
                GstElement *playbin_videosink = make_video_sink(videosink, videosink_options);  
                if (!playbin_videosink) {
                    logger_log(logger, LOGGER_ERR, "video_renderer_init: failed to create playbin_videosink");
                } else {
                    logger_log(logger, LOGGER_DEBUG, "video_renderer_init: create playbin_videosink at %p", playbin_videosink);
                    g_object_set(G_OBJECT (renderer_type[i]->pipeline), "video-sink", playbin_videosink, NULL);
                }
            }
            gint flags = 0;
            g_object_get(renderer_type[i]->pipeline, "flags", &flags, NULL);
            flags |= GST_PLAY_FLAG_DOWNLOAD;
            flags |= GST_PLAY_FLAG_BUFFERING;    // set by default in playbin3, but not in playbin2; is it needed?
            g_object_set(renderer_type[i]->pipeline, "flags", flags, NULL);
            renderer_type[i]->diagnostics = playback_diagnostics_attach(
                renderer_type[i]->pipeline, logger, direct_requested_at, direct_generation);
            renderer_type[i]->gap_repair = hls_gap_repair_attach(
                renderer_type[i]->pipeline, logger, direct_generation);
        } else {
            bool jpeg_pipeline = false;
            if (i == type_264) {
                renderer_type[i]->codec = h264;
                caps = gst_caps_from_string(h264_caps);
            } else if (i == type_265) {
                renderer_type[i]->codec = h265;
                caps = gst_caps_from_string(h265_caps);
            } else if (i == type_jpeg) {
                jpeg_pipeline = true;
                renderer_type[i]->codec = jpeg;
                caps = gst_caps_from_string(jpeg_caps);
            } else {
                g_assert(0);
            }
            GString *launch = g_string_new("appsrc name=video_source ! ");
            if (jpeg_pipeline) {
                g_string_append(launch, "jpegdec ");
            } else {
                g_string_append(launch, "queue ! ");
                g_string_append(launch, parser);
                g_string_append(launch, " ! ");
                if (!rtp) {
                    g_string_append(launch, decoder);
                } else {
                    g_string_append(launch, "rtph264pay ");
                    g_string_append(launch, rtp_pipeline);
                }
            }
            if (!rtp || jpeg_pipeline) {
                g_string_append(launch, " ! ");
                append_videoflip(launch, &videoflip[0], &videoflip[1]);
                g_string_append(launch, converter);
                g_string_append(launch, " ! ");
                g_string_append(launch, "videoscale ! ");
                if (jpeg_pipeline) {
                    g_string_append(launch, " imagefreeze allow-replace=TRUE ! textoverlay name=metadata_overlay ! ");
                }
                g_string_append(launch, videosink);
                g_string_append(launch, " name=");
                g_string_append(launch, videosink);
                g_string_append(launch, "_");
                g_string_append(launch, renderer_type[i]->codec);
                g_string_append(launch, videosink_options);
                if (video_sync && !jpeg_pipeline) {
                    g_string_append(launch, " sync=true");
                    sync = true;
                } else {
                    g_string_append(launch, " sync=false");
                    sync = false;
                }
            }
            if (!strcmp(renderer_type[i]->codec, h264)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h265))){
                    pos +=3;
                    *pos = '4';
                }
            } else if (!strcmp(renderer_type[i]->codec, h265)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h264))){
                    pos +=3;
                    *pos = '5';
                }
            }

            logger_log(logger, LOGGER_DEBUG, "GStreamer video pipeline %d:\n\"%s\"", i + 1, launch->str);
            renderer_type[i]->pipeline = gst_parse_launch(launch->str, &error);
            if (error) {
                logger_log(logger, LOGGER_ERR, "GStreamer gst_parse_launch failed to create video pipeline %d\n"
                           "*** error message from gst_parse_launch was:\n%s\n"
                           "launch string parsed was \n[%s]", i + 1, error->message, launch->str);
                if (strstr(error->message, "no element")) {
                    logger_log(logger, LOGGER_ERR, "This error usually means that a uxplay option was mistyped\n"
                               "           or some requested part of GStreamer is not installed\n");
                }
                g_clear_error (&error);
            }
            g_assert (renderer_type[i]->pipeline);
            GstClock *clock = gst_system_clock_obtain();
            g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);
            gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);
            renderer_type[i]->appsrc = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "video_source");
            g_assert(renderer_type[i]->appsrc);
            g_object_set(renderer_type[i]->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
            g_string_free(launch, TRUE);
            gst_caps_unref(caps);
            gst_object_unref(clock);
            if (jpeg_pipeline) {
                 renderer_type[i]->textsrc = gst_bin_get_by_name(GST_BIN(renderer_type[i]->pipeline), "metadata_overlay");
                 g_object_set(G_OBJECT(renderer_type[i]->textsrc), "text", "", "shaded-background", TRUE, "font-desc", "Sans, 16",  NULL);
            }
        }	
#ifdef X_DISPLAY_FIX
        use_x11 = (strstr(videosink, "xvimagesink") || strstr(videosink, "ximagesink") || auto_videosink);
        fullscreen = initial_fullscreen;
        renderer_type[i]->server_name = server_name;
        renderer_type[i]->gst_window = NULL;
        renderer_type[i]->use_x11 = false;
        X11_search_attempts = 0;
        /* setting char *x11_display_name to NULL means the value is taken from $DISPLAY in the environment 
         * (a uxplay option to specify a different value is possible)  */
        char *x11_display_name = NULL;
        if (use_x11) {
            if (i == 0) {
                renderer_type[0]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[0]->gst_window);
                get_X11_Display(renderer_type[0]->gst_window, x11_display_name);
                if (renderer_type[0]->gst_window->display) {
                    renderer_type[0]->use_x11 = true;
                } else {
                    free(renderer_type[0]->gst_window);
                    renderer_type[0]->gst_window = NULL;
                }
            } else if (renderer_type[0]->use_x11) {
                renderer_type[i]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[i]->gst_window);
                memcpy(renderer_type[i]->gst_window, renderer_type[0]->gst_window, sizeof(X11_Window_t));
                renderer_type[i]->use_x11 = true;
            }
        }
#endif
        if (screen_mode != SCREEN_INFO_OFF) {
            if (!renderer_type[i]->diagnostics)
                renderer_type[i]->diagnostics = playback_diagnostics_attach(
                    renderer_type[i]->pipeline, logger, g_get_monotonic_time(),
                    renderer_type[i]->screen_generation);
            playback_diagnostics_enable_screen(renderer_type[i]->diagnostics,
                                                screen_present_video_buffer, renderer_type[i]);
        }
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);	
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_READY);
        GstState state;
        GstStateChangeReturn ret = gst_element_get_state (renderer_type[i]->pipeline, &state, NULL, 100 * GST_MSECOND);
        if (ret == GST_STATE_CHANGE_SUCCESS) {
            logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer %d", i + 1);
            if (hls_video && i == 0) {
                renderer = renderer_type[i];
            }
        } else {
            logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer %d", i + 1);
            logger_log(logger, LOGGER_INFO, "\nPerhaps your GStreamer installation is missing some required plugins,"
                       "\nor your choices of video options (-vs -vd -vc -fs etc.) are incompatible on"
                       "\nthis computer architecture.  (An example: kmssink with fullscreen option -fs"
                       "\nmay work on some systems, but fail on others)");
            exit(1);
        }
    }
}

static void video_renderer_pause_unlocked() {
    if (screen_mode != SCREEN_INFO_OFF && renderer && renderer->screen_generation)
        screen_status_event(renderer->screen_generation, SCREEN_EVENT_PAUSED);
    if (hls_video || direct_request_pending) {
        if (direct_state.failed) return;
        direct_state.intent = DIRECT_PLAYBACK_PAUSED;
        if (renderer && renderer->direct_generation == direct_generation)
            playback_diagnostics_control(renderer->diagnostics, &direct_state);
        direct_apply_state();
        return;
    }
    if (!renderer) {
        return;
    }
    GstStateChangeReturn ret = gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
    logger_log(logger, LOGGER_DEBUG, "video renderer pause: %s", gst_element_state_change_return_get_name(ret));
}

static void video_renderer_resume_unlocked() {
    if (screen_mode != SCREEN_INFO_OFF && renderer && renderer->screen_generation)
        screen_status_event(renderer->screen_generation, SCREEN_EVENT_RESUMED);
    if (hls_video || direct_request_pending) {
        if (direct_state.failed) return;
        direct_state.intent = DIRECT_PLAYBACK_PLAYING;
        if (renderer && renderer->direct_generation == direct_generation)
            playback_diagnostics_control(renderer->diagnostics, &direct_state);
        direct_apply_state();
        return;
    }
    if (!renderer) {
        return;
    }
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState state;
    /* wait with timeout 100 msec for pipeline to change state from PAUSED to PLAYING */
    gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
    const gchar *state_name = gst_element_state_get_name(state);
    logger_log(logger, LOGGER_DEBUG, "video renderer resumed: state %s", state_name);
    if (renderer->appsrc) {
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    }
}

static void video_renderer_start_unlocked() {
    GstState state;
    const gchar *state_name = NULL;
    screen_output_suspended = FALSE;
    if (hls_video) {
        if (direct_state.failed || direct_state.intent == DIRECT_PLAYBACK_STOPPED) {
            return; /* A stop received during rebuilding cancels startup. */
        }
        g_object_set (G_OBJECT (renderer->pipeline), "uri", renderer->uri, NULL);
        renderer->direct_started = TRUE;
        GstStateChangeReturn initial = gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
        if (initial == GST_STATE_CHANGE_FAILURE) {
            direct_fail_session(renderer);
            return;
        }
        /* Preroll completes on the bus. Waiting here holds the same mutex as
         * pause/stop/replacement, delaying every new HTTP playback request. */
        GstStateChangeReturn settled = gst_element_get_state(renderer->pipeline, &state, NULL, 0);
        direct_state.live = initial == GST_STATE_CHANGE_NO_PREROLL || settled == GST_STATE_CHANGE_NO_PREROLL;
        /* Asynchronous preroll is handled in bus order by ASYNC_DONE. */
        direct_state.preroll_complete = initial == GST_STATE_CHANGE_SUCCESS;
        direct_apply_state();
	state_name = gst_element_state_get_name(state);
	logger_log(logger, LOGGER_DEBUG, "video renderer_start: state %s", state_name);
        return;
    } 
    /* when not hls, start both h264 and h265 pipelines; will shut down the "wrong" one when we know the codec */
    for (int i = 0; i < n_renderers; i++) {
        if (!renderer_type[i]) continue;
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_PAUSED);
        gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 1000 * GST_MSECOND);
        state_name = gst_element_state_get_name(state);
        logger_log(logger, LOGGER_DEBUG, "video renderer_start: renderer %d %p state %s", i, renderer_type[i], state_name);
    }
    renderer = NULL;
    first_packet = true;
#ifdef X_DISPLAY_FIX
    X11_search_attempts = 0;
#endif
}

/* used to find any X11 Window used by the playbin (HLS) pipeline after it starts playing. 
*  if use_x11 is true, called every 100 ms after playbin state is READY until the x11 window is found*/
bool waiting_for_x11_window() {
    if (!hls_video) {
        return false;
    }
#ifdef X_DISPLAY_FIX
    if (use_x11 && renderer->gst_window) {
        get_x_window(renderer->gst_window, renderer->server_name);
        if (!renderer->gst_window->window) {
	    return true;    /* window still not found */
        }
    }
    if (fullscreen) {
         set_fullscreen(renderer->gst_window, &fullscreen);
    }
#endif
    return false;
}

/* use this to cycle the jpeg renderer to remove expired coverart when no new coverart has replaced it */
int video_renderer_cycle() {
    if (!renderer || !strstr(renderer->codec, jpeg)) {
        return -1;
    }
    GstState state, pending_state, target_state;
    GstStateChangeReturn ret;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    logger_log(logger, LOGGER_DEBUG, "renderer_cycle renderer %p: initial pipeline state is %s", renderer,
               gst_element_state_get_name(state));

    for (int i = 0 ; i < 2; i++) {
        int count = 0;
        if (i == 0 ) {
            target_state = GST_STATE_NULL;
            video_renderer_stop();
        } else {
            target_state = GST_STATE_PLAYING;
            gst_element_set_state (renderer->pipeline, target_state);
        }
        while (state != target_state) {
            ret = gst_element_get_state(renderer->pipeline, &state, &pending_state, 1000 * GST_MSECOND);
            if (ret == GST_STATE_CHANGE_SUCCESS) {
                logger_log(logger, LOGGER_DEBUG, "current pipeline state is %s", gst_element_state_get_name(state));
                if (pending_state != GST_STATE_VOID_PENDING) {
                    logger_log(logger, LOGGER_DEBUG, "pending pipeline state is %s", gst_element_state_get_name(pending_state));
                }
            } else if (ret == GST_STATE_CHANGE_FAILURE) {
                logger_log(logger, LOGGER_ERR, "pipeline %s: state change to %s failed", renderer->codec, gst_element_state_get_name(target_state));
                count++;
                if (count > 10) {
                    return -1;
                }
            } else if (ret == GST_STATE_CHANGE_ASYNC) {
                logger_log(logger, LOGGER_DEBUG, "state change to %s is asynchronous, waiting for completion ...",
                           gst_element_state_get_name(target_state));
            }
        }
    }
    return 0;
}

void video_renderer_display_jpeg(const void *data, int *data_len) {
    GstBuffer *buffer = NULL;
    if (type_jpeg == -1) {
        return;
    }
    if (renderer && !strcmp(renderer->codec, jpeg)) {
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
	g_assert(buffer != NULL);
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
    }  
}

uint64_t video_renderer_render_buffer(unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    GstBuffer *buffer = NULL;
    GstClockTime pts = (GstClockTime) *ntp_time; /*now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (sync) {
        if (pts >= gst_video_pipeline_base_time) {
            pts -= gst_video_pipeline_base_time;
        } else {
            // adjust timestamps to be >= gst_video_pipeline_base time
            logger_log(logger, LOGGER_DEBUG, "*** invalid ntp_time < gst_video_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            return  (uint64_t)  gst_video_pipeline_base_time - pts;
        }
    }
    g_assert(data_len != 0);
    /* first four bytes of valid  h264  video data are 0x00, 0x00, 0x00, 0x01.    *
     * nal_count is the number of NAL units in the data: short SPS, PPS, SEI NALs *
     * may  precede a VCL NAL. Each NAL starts with 0x00 0x00 0x00 0x01 and is    *
     * byte-aligned: the first byte of invalid data (decryption failed) is 0x01   */
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
    } else {
        if (first_packet) {
            logger_log(logger, LOGGER_INFO, "Begin streaming to GStreamer video pipeline");
            first_packet = false;
        }
        if (!renderer || !(renderer->appsrc)) {
            logger_log(logger, LOGGER_DEBUG, "*** no video renderer found");
            return 0;
        }
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        g_assert(buffer != NULL);
        //g_print("video latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
        if (sync) {
            GST_BUFFER_PTS(buffer) = pts;
        }
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window && !(renderer->gst_window->window) && renderer->use_x11) {
            X11_search_attempts++;
            logger_log(logger, LOGGER_DEBUG, "Looking for X11 UxPlay Window, attempt %d", (int) X11_search_attempts);
            get_x_window(renderer->gst_window, renderer->server_name);
            if (renderer->gst_window->window) {
                logger_log(logger, LOGGER_INFO, "\n*** X11 Windows: Use key F11 or (left Alt)+Enter to toggle full-screen mode\n");
                if (fullscreen) {
                    set_fullscreen(renderer->gst_window, &fullscreen);
                }
            }
        }
#endif
    }
    return 0;
}

void video_renderer_flush() {
}

static void video_renderer_hls_ready_unlocked() {
    GstState state;
    GstStateChangeReturn ret;
    if (hls_video || direct_request_pending) {
        direct_state.intent = DIRECT_PLAYBACK_STOPPED;
        if (direct_state.failed) return;
    }
    if (renderer && hls_video && renderer->direct_generation == direct_generation) {
        playback_diagnostics_control(renderer->diagnostics, &direct_state);
        renderer->direct_started = FALSE;
        logger_log(logger, LOGGER_DEBUG,"video_renderer_hls_ready");
        ret = gst_element_set_state (renderer->pipeline, GST_STATE_READY);
        logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                   gst_element_state_change_return_get_name(ret));
        gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
    }
}

static void video_renderer_stop_unlocked() {
    if (hls_video) {
        direct_state.intent = DIRECT_PLAYBACK_STOPPED;
        if (renderer) renderer->direct_started = FALSE;
    }
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_stop");
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        //gst_element_set_state (renderer->playbin, GST_STATE_NULL);
     }
}

void video_renderer_set_device_model(const char *model, const char *name) {
    // Device frame not supported in GStreamer renderer
    (void)model;
    (void)name;
}

void video_renderer_set_track_metadata(const char *title, const char *artist, const char *album) {
    // Track metadata display superimposed on coverart is now supported in GStreamer renderer
    GString *metadata = g_string_new("");
    if (artist) {
        g_string_append(metadata, artist);
    }
    if (artist && title) {
        g_string_append(metadata, ": ");
    }
    if (title) {
        g_string_append(metadata, "\"");
        g_string_append(metadata, title);
        g_string_append(metadata, "\"");
    }
    
    g_string_replace (metadata, "&", "&amp;", 0);   //fix pango problem with "&" in text
    if (renderer && renderer->textsrc && (artist || title)) {
        g_object_set(G_OBJECT(renderer->textsrc), "text", metadata->str, NULL);
    }
    g_string_free(metadata, TRUE);
}

static void video_renderer_destroy_instance(video_renderer_t *renderer) {
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"destroying renderer instance %p codec=%s ", renderer, renderer->codec);
        GstState state;
        GstStateChangeReturn ret;
        gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        if (state != GST_STATE_NULL) {
            if (!hls_video) {
                gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
            }
            ret = gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
            logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                       gst_element_state_change_return_get_name(ret));
            gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
            logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        }
        if (renderer->appsrc) {
            gst_object_unref (renderer->appsrc);
            renderer->appsrc = NULL;
        }
        if (renderer->textsrc) {
            gst_object_unref (renderer->textsrc);
            renderer->textsrc = NULL;
        }	
        if (renderer->buffering_handler) {
            g_signal_handler_disconnect(renderer->pipeline, renderer->buffering_handler);
            renderer->buffering_handler = 0;
        }
        hls_gap_repair_free(renderer->gap_repair);
        renderer->gap_repair = NULL;
        playback_diagnostics_free(renderer->diagnostics);
        renderer->diagnostics = NULL;
        if (renderer->overlay_pipeline) {
            gst_element_set_state(renderer->overlay_pipeline, GST_STATE_NULL);
            gst_object_unref(renderer->overlay_pipeline);
        }
        if (renderer->overlay) gst_video_overlay_composition_unref(renderer->overlay);
        g_mutex_clear(&renderer->overlay_lock);
        gst_object_unref(renderer->bus);
        gst_object_unref(renderer->pipeline);
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window){
	  // free_X11_Display(renderer->gst_window);   without this, a memory leak; with it, a coredump
            free(renderer->gst_window);
            renderer->gst_window = NULL;
        }
#endif
        if (renderer->uri) {
            free(renderer->uri);
        }
        free (renderer);
        renderer = NULL;
        logger_log(logger, LOGGER_DEBUG,"renderer destroyed\n");	
    }
}

static void video_renderer_destroy_unlocked() {
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i]) {
            video_renderer_destroy_instance(renderer_type[i]);
            renderer_type[i] = NULL;
        }
    }
    renderer = NULL;
}

static void get_stream_status_name(GstStreamStatusType type, char *name, size_t len) {
    switch (type) {
    case GST_STREAM_STATUS_TYPE_CREATE:
        strncpy(name, "CREATE", len);
        return;
    case GST_STREAM_STATUS_TYPE_ENTER:
        strncpy(name, "ENTER", len);
        return;
    case GST_STREAM_STATUS_TYPE_LEAVE:
        strncpy(name, "LEAVE", len);
        return;
    case GST_STREAM_STATUS_TYPE_DESTROY:
        strncpy(name, "DESTROY", len);
        return;
    case GST_STREAM_STATUS_TYPE_START:
        strncpy(name, "START", len);
        return;
    case GST_STREAM_STATUS_TYPE_PAUSE:
        strncpy(name, "PAUSE", len);
        return;
    case GST_STREAM_STATUS_TYPE_STOP:
        strncpy(name, "STOP", len);
        return;
    default:
        strncpy(name, "", len);
        return;
    }
}

static void hls_video_seek_to_start_position(GstElement *pipeline) {
    if (hls_requested_start_position && hls_seek_enabled && hls_requested_start_position >= hls_seek_start
        && hls_requested_start_position  <= hls_seek_end) {
        g_print("***************** seek to hls_requested_start_position %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(hls_requested_start_position));
        if (gst_element_seek_simple (pipeline, GST_FORMAT_TIME,
				 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, hls_requested_start_position)) {
            hls_requested_start_position = 0;
        } else {
            g_print("*** seek to requested_start_position failed\n"); 
        }
    } 
}

static gboolean gstreamer_video_pipeline_bus_callback_unlocked(GstBus *bus, GstMessage *message, void *loop) {
    GstState old_state, new_state;
    const gchar no_state[] = "";
    const gchar *old_state_name = no_state, *new_state_name = no_state;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STATE_CHANGED) {
        GstState old_state, new_state;
        gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
        old_state_name = gst_element_state_get_name (old_state);
        new_state_name = gst_element_state_get_name (new_state);
    }

    /* identify which pipeline sent the message */ 
    int type = -1;
    for (int i = 0 ; i < n_renderers ; i ++ ) {
        if (renderer_type[i] && renderer_type[i]->bus == bus) {
            type = i;
            break;
        }
    }

    /* if the bus sending the message is not found, the renderer may already have been destroyed */
    if (type == -1) {
        if (logger_debug) {
            g_print("GStreamer(UNKNOWN, now destroyed?) bus message: %s %s %s %s\n",
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }     
        return TRUE;
    }

    video_renderer_t *renderer = renderer_type[type];
    if (hls_video && renderer->direct_generation != direct_generation) {
        return TRUE; /* A new /play request superseded this pipeline. */
    }
    if (hls_video && direct_state.failed) return TRUE;
    playback_diagnostics_message(renderer->diagnostics, message);
    if (screen_mode != SCREEN_INFO_OFF && renderer->screen_generation &&
        GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
        screen_status_fail(renderer->screen_generation, SCREEN_ERROR_BACKEND);
    if (hls_video && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        direct_fail_session(renderer);
        return TRUE;
    }

    gint64 pos = -1;
    if (hls_video) {
        gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos);
        if (hls_requested_start_position && pos >= hls_requested_start_position) {
            hls_requested_start_position = 0;
        }
    }

    if (logger_debug) {
        gchar *name = NULL;
        GstElement *element = NULL;
        gchar type_name[8] = { 0 };
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STREAM_STATUS) {
            GstStreamStatusType type;
            gst_message_parse_stream_status(message, &type, &element);
            name = gst_element_get_name(element);
            get_stream_status_name(type, type_name, 8);
            old_state_name = name;
            new_state_name = type_name;
        }
        if (GST_CLOCK_TIME_IS_VALID(pos)) {
            g_print("GStreamer %s  bus message %s %s %s %s; position: %" GST_TIME_FORMAT "\n" ,renderer->codec,
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name, GST_TIME_ARGS(pos));
        } else {
            g_print("GStreamer %s bus message %s %s %s %s\n", renderer->codec,
                    GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }
        if (name) {
            g_free(name);
        }
    }

    if (hls_video && !GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration);
    }

    if (hls_requested_start_position && hls_seek_enabled) {
        hls_video_seek_to_start_position(renderer->pipeline);
    }

    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_DURATION:
        hls_duration = GST_CLOCK_TIME_NONE;
        break;
    case GST_MESSAGE_BUFFERING:
        if (hls_video) {
            gint percent = -1;
            gst_message_parse_buffering(message, &percent);
            if (direct_playback_state_buffer(&direct_state, percent)) {
                hls_buffer_empty = percent == 0;
                hls_buffer_full = percent == 100;
                renderer->buffering_level = percent;
                if (screen_mode != SCREEN_INFO_OFF && renderer->screen_generation) {
                    if (percent < 100) screen_status_event(renderer->screen_generation, SCREEN_EVENT_BUFFERING);
                    else if (direct_state.intent == DIRECT_PLAYBACK_PLAYING)
                        screen_status_event(renderer->screen_generation, SCREEN_EVENT_RESUMED);
                }
                logger_log(logger, LOGGER_DEBUG, "Buffering :%d percent done", percent);
                direct_apply_state();
            }
        }
	break;      
    case GST_MESSAGE_ASYNC_DONE:
        if (hls_video && GST_MESSAGE_SRC(message) == GST_OBJECT(renderer->pipeline)) {
            direct_state.preroll_complete = true;
            if (renderer->screen_seek_pending) {
                renderer->screen_seek_pending = FALSE;
                screen_status_event(renderer->screen_generation, SCREEN_EVENT_SEEK_COMPLETE);
            }
            direct_apply_state();
        }
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gboolean flushing = FALSE;
        gboolean closed_window = FALSE;
        gst_message_parse_error (message, &err, &debug);
        /* Direct-playback diagnostics already report safe factory/domain/code
         * details. GStreamer's raw error text may contain a signed media URI. */
        if (!hls_video)
            logger_log(logger, LOGGER_INFO, "GStreamer error (video): %s %s", GST_MESSAGE_SRC_NAME(message),err->message);
        if (strstr(err->message, "Output window was closed")) {
            closed_window = TRUE;
        }
        if (!hls_video && strstr(err->message,"Internal data stream error")) {
            logger_log(logger, LOGGER_INFO,
                     "*** This is a generic GStreamer error that usually means that GStreamer\n"
                     "*** was unable to construct a working video pipeline.\n\n"
                     "*** If you are letting the default autovideosink select the videosink,\n"
                     "*** GStreamer may be trying to use non-functional hardware h264 video decoding.\n"
                     "*** Try using option -avdec to force software decoding or use -vs <videosink>\n"
                     "*** to select a videosink of your choice (see \"man uxplay\").\n\n"
                     "*** Raspberry Pi models 4B and earlier using Video4Linux2 may need \"-bt709\" uxplay option");
        }
        g_error_free (err);
        g_free (debug);
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        if (!hls_video || closed_window) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
            g_main_loop_quit( (GMainLoop *) loop);
        }
        break;
    }
    case GST_MESSAGE_EOS:
        /* end-of-stream */
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream (video)");
        if (hls_video) {
            direct_state.intent = DIRECT_PLAYBACK_STOPPED;
            renderer->direct_started = FALSE;
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
            renderer->eos = TRUE;
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (hls_video && GST_MESSAGE_SRC(message) == GST_OBJECT(renderer->pipeline)) {
            GstState old_state, new_state;
            gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
            if (logger_debug) {
            g_print ("****** hls_playbin: Element %s changed state from %s to %s.\n", GST_OBJECT_NAME (message->src),
                     gst_element_state_get_name (old_state),
                     gst_element_state_get_name (new_state));
            } 
            if (new_state != GST_STATE_PLAYING) {
                hls_playing = FALSE;
                break;
            } 
            hls_playing = TRUE;
            if (!direct_state.buffering) {
                hls_buffer_empty = FALSE;
            }
            GstQuery *query = NULL;
            query = gst_query_new_seeking(GST_FORMAT_TIME);
                if (gst_element_query(renderer->pipeline, query)) {
	        gst_query_parse_seeking (query, NULL, &hls_seek_enabled, &hls_seek_start, &hls_seek_end);
                if (hls_seek_enabled) {
                    g_print ("Seeking is ENABLED from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT "\n",
			     GST_TIME_ARGS (hls_seek_start), GST_TIME_ARGS (hls_seek_end));
                } else {
                    g_print ("Seeking is DISABLED for this stream.\n");
                }
            } else {
                g_printerr ("Seeking query failed.");
            }
            gst_query_unref (query);

            if (hls_requested_start_position && hls_seek_enabled) {
                hls_video_seek_to_start_position(renderer->pipeline);
            }

        }
        if (renderer->autovideo) {
            char *sink = strstr(GST_MESSAGE_SRC_NAME(message), "-actual-sink-");
            if (sink) {
                sink += strlen("-actual-sink-");
                if (strstr(GST_MESSAGE_SRC_NAME(message), renderer->codec)) {
                    logger_log(logger, LOGGER_DEBUG, "GStreamer: automatically-selected videosink"
                               " (renderer %d: %s) is \"%ssink\"", renderer->id + 1,
                               renderer->codec, sink);
#ifdef X_DISPLAY_FIX
                    renderer->use_x11 = (strstr(sink, "ximage") || strstr(sink, "xvimage"));
#endif
                    renderer->autovideo = false;
                }
            }
        }
        break;
#ifdef  X_DISPLAY_FIX
    case GST_MESSAGE_ELEMENT:
        if (renderer->gst_window && renderer->gst_window->window) {
            GstNavigationMessageType message_type = gst_navigation_message_get_type (message);
            if (message_type == GST_NAVIGATION_MESSAGE_EVENT) {
                GstEvent *event = NULL;
                if (gst_navigation_message_parse_event (message, &event)) {
                    GstNavigationEventType event_type = gst_navigation_event_get_type (event);
                    const gchar *key = NULL;
                    switch (event_type) {
                    case GST_NAVIGATION_EVENT_KEY_PRESS:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if ((strcmp (key, "F11") == 0) || (alt_keypress && strcmp (key, "Return") == 0)) {
                                fullscreen = !(fullscreen);
                                set_fullscreen(renderer->gst_window, &fullscreen);
                            } else if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = true;
                            }
                        }
                        break;
                    case GST_NAVIGATION_EVENT_KEY_RELEASE:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = false;
                            }
                        }
                    default:
                        break;
                    }
                }
                if (event) {
                    gst_event_unref (event);
                }
            }
        }
        break;
#endif
    default:
      /* unhandled message */
        break;
    }
    return TRUE;
}

static int video_renderer_choose_codec_unlocked(bool video_is_jpeg, bool video_is_h265) {
    video_renderer_t *renderer_used = NULL;
    g_assert(!hls_video);
    if (video_is_jpeg) {
        g_assert(type_jpeg != -1);
        renderer_used = renderer_type[type_jpeg];
    } else {
        if (video_is_h265) {
            if (type_265 == -1) {
                logger_log(logger, LOGGER_ERR, "video is h265 but the -h265 option was not used");
                return -1;
            }
            renderer_used = renderer_type[type_265];
        } else {
            g_assert(type_264 != -1);
            renderer_used = renderer_type[type_264];
        }
    }
    if (renderer_used == NULL) {
        return -1;
    } else if (renderer_used == renderer && !screen_output_suspended) {
        return 0;
    } else if (renderer && renderer_used != renderer) {
        return -1;
    }
    renderer = renderer_used;
    screen_output_suspended = FALSE;
    if (screen_mode != SCREEN_INFO_OFF && !renderer->screen_generation) {
        /* Idle mirror shells have no session when constructed. Bind once,
         * immediately before their first activation, then never relabel late
         * buffers as belonging to a replacement sender. */
        screen_status_snapshot_t snapshot;
        screen_status_get_snapshot(&snapshot);
        renderer->screen_generation = snapshot.generation;
    }
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState old_state, new_state;
    if (gst_element_get_state(renderer->pipeline, &old_state, &new_state, 100 * GST_MSECOND) == GST_STATE_CHANGE_FAILURE) {
        g_error("video pipeline failed to go into playing state");
        return -1;
    }
    logger_log(logger, LOGGER_DEBUG, "video_pipeline state change from %s to %s\n",
               gst_element_state_get_name (old_state),gst_element_state_get_name (new_state));
    gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    if (strstr(renderer->codec, h265)) {
        logger_log(logger, LOGGER_INFO, "*** video format is h265 high definition (HD/4K) video %dx%d", width, height);
    }
    /* destroy unused renderers */
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i] == renderer) {
            continue;
        }
	if (renderer_type[i]) {
            video_renderer_t *renderer_unused = renderer_type[i];
            renderer_type[i] = NULL;
            video_renderer_destroy_instance(renderer_unused);
        }
    }
    return 0;
}
    

static bool video_get_playback_info_unlocked(double *duration, double *position, double *seek_start, double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full) {
    gint64 pos = 0;
    GstState state;
    *duration = 0.0;
    *position = -1.0;
    *seek_start = 0.0;
    *seek_duration = 0.0;
    *rate = 0.0f;
    *buffer_empty = true;
    *buffer_full = false;
    if (!renderer) {
        return true;
    }
    /* Do not report an old pipeline's state while its replacement is pending.
     * Keep a failed session's control connection available for the next /play:
     * returning false invokes broader HTTP playlist/connection teardown. */
    if (hls_video && renderer->direct_generation != direct_generation) return true;
    if (hls_video && direct_state.failed) {
        *position = 0.0;
        return true;
    }

    if (hls_seek_enabled && hls_seek_start >= 0 && hls_seek_end >= hls_seek_start) {
        *seek_start = ((double) hls_seek_start) / GST_SECOND;
        *seek_duration = ((double) (hls_seek_end - hls_seek_start)) / GST_SECOND;     
    }

    *buffer_empty = (bool) hls_buffer_empty;
    *buffer_full = (bool) hls_buffer_full;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    *rate = 0.0f;
    switch (state) {
    case GST_STATE_PLAYING:
        *rate = 1.0f;
    default:
        break;
    }

    if (!GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        gst_element_query_duration(renderer->pipeline, GST_FORMAT_TIME, &hls_duration);
    }
    if (hls_duration > 0) {
        *duration = ((double) hls_duration) / GST_SECOND;
    }
    /* Live streams can report their position without a finite duration. */
    if (gst_element_query_position(renderer->pipeline, GST_FORMAT_TIME, &pos) && pos >= 0) {
        *position = ((double) pos) / GST_SECOND;
    }

    logger_log(logger, LOGGER_DEBUG, "******* video_get_playback_info: position %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " %s rate %f *****",
               GST_TIME_ARGS (pos), GST_TIME_ARGS (hls_duration), gst_element_state_get_name(state), *rate);

    return true;
}

static void video_renderer_set_start_unlocked(float position) {
    int64_t requested = 0;
    direct_playback_seconds_to_ns(position, &requested);
    hls_requested_start_position = requested;
    direct_playback_state_reset(&direct_state);
    direct_request_pending = TRUE;
    direct_generation++;
    direct_requested_at = g_get_monotonic_time();
    if (logger) logger_log(logger, LOGGER_INFO,
        "Direct playback: session=%" G_GUINT64_FORMAT " stage=play-request start_seconds=%.3f previous_session=%" G_GUINT64_FORMAT,
        direct_generation, position, renderer ? renderer->direct_generation : 0);
    logger_log(logger, LOGGER_DEBUG, "register HLS video start position %f %lld", position,
               hls_requested_start_position);    
}

static void video_renderer_seek_unlocked(float position) {
    int64_t converted;
    if (!renderer || !hls_video || direct_state.failed || renderer->direct_generation != direct_generation ||
        !direct_playback_seconds_to_ns(position, &converted)) {
        return;
    }
    gint64 seek_position = converted;
    gint64 lower = 1000, upper;
    /* don't seek to within 1  microsecond  of beginning or end of video */
    if (hls_duration >= 2000) {
        upper = hls_duration - 1000;
    } else if (hls_seek_enabled && hls_seek_start >= 0 && hls_seek_end > hls_seek_start &&
               hls_seek_end - hls_seek_start >= 2000) {
        lower = hls_seek_start + 1000;
        upper = hls_seek_end - 1000;
    } else {
        return;
    }
    seek_position = CLAMP(seek_position, lower, upper);
    g_print("SCRUB: seek to %f secs =  %" GST_TIME_FORMAT ", duration = %" GST_TIME_FORMAT "\n", position,
            GST_TIME_ARGS(seek_position),  GST_TIME_ARGS(hls_duration));
    gboolean result = gst_element_seek_simple(renderer->pipeline, GST_FORMAT_TIME,
                                              (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                              seek_position);
    if (result) {
        g_print("seek succeeded\n");
        if (screen_mode != SCREEN_INFO_OFF && renderer->screen_generation) {
            renderer->screen_seek_pending = TRUE;
            screen_status_event(renderer->screen_generation, SCREEN_EVENT_SEEKING);
        }
        direct_apply_state();
    } else {
        g_print("seek failed\n");
    }
}

static gboolean gstreamer_video_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {
    g_rec_mutex_lock(&direct_mutex);
    gboolean result = gstreamer_video_pipeline_bus_callback_unlocked(bus, message, loop);
    g_rec_mutex_unlock(&direct_mutex);
    return result;
}

unsigned int video_renderer_listen(void *loop, int id) {
    g_assert(id >= 0 && id < n_renderers);
    /* Codec selection retires unused mirror shells. A canceled reset can
     * re-enter the loop while an mpv replacement retains output ownership;
     * registration must not require rebuilding those retired pipelines. */
    if (!renderer_type[id] || !renderer_type[id]->bus) return 0;
    return (unsigned int) gst_bus_add_watch(renderer_type[id]->bus,(GstBusFunc)
                                            gstreamer_video_pipeline_bus_callback, (gpointer) loop);    
}

static bool video_renderer_eos_watch_unlocked() {
    if (hls_video && renderer && renderer->direct_generation == direct_generation) {
        direct_apply_state();
        playback_diagnostics_tick(renderer->diagnostics, &direct_state);
    }
    if (hls_video && renderer && renderer->direct_generation == direct_generation && renderer->eos) {
        renderer->eos = FALSE;
	return true;
    }
    return false; 
}

static void video_renderer_hls_set_volume_unlocked(double volume) {
    if (!renderer || strcmp(renderer->codec, hls)) {
       return;
    }
    volume = (volume > 10.0) ? 10.0 : volume;
    volume = (volume < 0.0) ? 0.0 : volume;
    g_object_set(renderer->pipeline, "volume", volume, NULL);
}

/* Keep direct HTTP controls, bus handling and renderer lifetime serialized.
 * Recursive locking allows existing lifecycle helpers to call one another.
 */
#define SERIALIZED_VOID0(name) \
    void name(void) { \
        g_rec_mutex_lock(&direct_mutex); \
        name##_unlocked(); \
        g_rec_mutex_unlock(&direct_mutex); \
    }
SERIALIZED_VOID0(video_renderer_pause)
SERIALIZED_VOID0(video_renderer_resume)
SERIALIZED_VOID0(video_renderer_start)
SERIALIZED_VOID0(video_renderer_hls_ready)
SERIALIZED_VOID0(video_renderer_stop)
SERIALIZED_VOID0(video_renderer_destroy)
#undef SERIALIZED_VOID0

void video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2],
                         const char *parser, const char *rtp_pipeline, const char *decoder,
                         const char *converter, const char *videosink, const char *videosink_options,
                         bool initial_fullscreen, bool video_sync, bool h265_support,
                         bool coverart_support, guint playbin_version, const char *uri) {
    g_rec_mutex_lock(&direct_mutex);
    video_renderer_init_unlocked(render_logger, server_name, videoflip, parser, rtp_pipeline,
        decoder, converter, videosink, videosink_options, initial_fullscreen, video_sync,
        h265_support, coverart_support, playbin_version, uri);
    g_rec_mutex_unlock(&direct_mutex);
}

void video_renderer_set_start(float position) {
    video_renderer_set_start_with_source(position, false);
}

void video_renderer_set_start_with_source(float position, bool direct_http) {
    g_rec_mutex_lock(&direct_mutex);
    direct_http_source = direct_http;
    video_renderer_set_start_unlocked(position);
    g_rec_mutex_unlock(&direct_mutex);
}

void video_renderer_seek(float position) {
    g_rec_mutex_lock(&direct_mutex);
    video_renderer_seek_unlocked(position);
    g_rec_mutex_unlock(&direct_mutex);
}

bool video_get_playback_info(double *duration, double *position, double *seek_start,
                             double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full) {
    return video_get_playback_info_with_readiness(duration, position, seek_start, seek_duration,
                                                  rate, buffer_empty, buffer_full, NULL, NULL);
}

bool video_get_playback_info_with_readiness(double *duration, double *position, double *seek_start,
    double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full,
    bool *ready_to_play, bool *likely_to_keep_up) {
    g_rec_mutex_lock(&direct_mutex);
    bool result = video_get_playback_info_unlocked(duration, position, seek_start, seek_duration,
                                                  rate, buffer_empty, buffer_full);
    /* Preserve the previous readiness policy for healthy sessions, while a
     * terminal failure or pending replacement cannot claim to be playable. */
    bool ready = renderer && (!hls_video ||
        (!direct_state.failed && renderer->direct_generation == direct_generation));
    if (ready_to_play) *ready_to_play = ready;
    if (likely_to_keep_up) *likely_to_keep_up = ready;
    g_rec_mutex_unlock(&direct_mutex);
    return result;
}

bool video_renderer_eos_watch(void) {
    g_rec_mutex_lock(&direct_mutex);
    bool result = video_renderer_eos_watch_unlocked();
    g_rec_mutex_unlock(&direct_mutex);
    return result;
}

void video_renderer_hls_set_volume(double volume) {
    g_rec_mutex_lock(&direct_mutex);
    video_renderer_hls_set_volume_unlocked(volume);
    g_rec_mutex_unlock(&direct_mutex);
}

int video_renderer_choose_codec(bool video_is_jpeg, bool video_is_h265) {
    g_rec_mutex_lock(&direct_mutex);
    int result = video_renderer_choose_codec_unlocked(video_is_jpeg, video_is_h265);
    g_rec_mutex_unlock(&direct_mutex);
    return result;
}

bool video_renderer_suspend_output(void) {
    g_rec_mutex_lock(&direct_mutex);
    gboolean released = TRUE;
    /* Transition every eager mirror/cover-art shell, not just the chosen
     * renderer. READY/PAUSED kmssink can already hold the DRM device. */
    for (int i = 0; i < n_renderers; i++) {
        if (!renderer_type[i]) continue;
        GstElement *pipeline = renderer_type[i]->pipeline;
        if (hls_video) renderer_type[i]->direct_started = FALSE;
        GstStateChangeReturn result = gst_element_set_state(pipeline, GST_STATE_NULL);
        GstState current = GST_STATE_VOID_PENDING;
        GstStateChangeReturn settled = gst_element_get_state(pipeline, &current, NULL, 250 * GST_MSECOND);
        if (result == GST_STATE_CHANGE_FAILURE || settled == GST_STATE_CHANGE_FAILURE || current != GST_STATE_NULL)
            released = FALSE;
    }
    screen_output_suspended = released;
    g_rec_mutex_unlock(&direct_mutex);
    return released;
}

void video_renderer_screen_refresh(void) {
    g_rec_mutex_lock(&direct_mutex);
    if (screen_mode == SCREEN_INFO_OFF || !renderer || !renderer->diagnostics || !renderer->screen_generation) {
        g_rec_mutex_unlock(&direct_mutex);
        return;
    }
    video_renderer_t *owner = renderer;
    screen_status_snapshot_t snapshot;
    screen_status_get_snapshot(&snapshot);
    if (snapshot.generation != owner->screen_generation || !snapshot.session_active) {
        g_rec_mutex_unlock(&direct_mutex);
        return;
    }
    playback_diagnostics_snapshot_t observed;
    playback_diagnostics_get_snapshot(owner->diagnostics, &observed);
    screen_status_video_t video = observed.video;
    g_strlcpy(video.backend, "GStreamer", sizeof(video.backend));
    g_strlcpy(video.decode_policy, snapshot.video.decode_policy, sizeof(video.decode_policy));
    g_mutex_lock(&owner->overlay_lock);
    video.overlay_known = owner->overlay_known;
    video.overlay_supported = owner->overlay_supported;
    g_strlcpy(video.overlay_reason, owner->overlay_reason, sizeof(video.overlay_reason));
    if (owner->overlay_known && (!owner->overlay_logged ||
        (!owner->overlay_supported && !owner->overlay_failure_logged))) {
        logger_log(logger, owner->overlay_supported ? LOGGER_INFO : LOGGER_WARNING,
            "Screen info: session=%" G_GUINT64_FORMAT " overlay=%s reason=%s decoder_unchanged=1",
            owner->screen_generation, owner->overlay_supported ? "supported" : "unqualified",
            owner->overlay_reason);
        owner->overlay_logged = TRUE;
        if (!owner->overlay_supported) owner->overlay_failure_logged = TRUE;
    }
    g_mutex_unlock(&owner->overlay_lock);
    screen_status_set_video(owner->screen_generation, &video);
    if (hls_video) {
        screen_status_audio_t audio = observed.audio;
        audio.volume_known = snapshot.audio.volume_known;
        audio.volume = snapshot.audio.volume;
        audio.muted = snapshot.audio.muted;
        screen_status_set_audio(owner->screen_generation, &audio);
        screen_status_progress_t progress = snapshot.progress;
        double duration = 0, position = -1, seek_start = 0, seek_duration = 0;
        float rate = 0;
        bool empty = false, full = false;
        video_get_playback_info_unlocked(&duration, &position, &seek_start, &seek_duration,
                                        &rate, &empty, &full);
        progress.position_known = position >= 0 && isfinite(position);
        progress.position_seconds = progress.position_known ? position : 0;
        progress.duration_known = duration > 0 && isfinite(duration);
        progress.duration_seconds = progress.duration_known ? duration : 0;
        /* NO_PREROLL establishes live; unknown duration alone does not. */
        progress.live_known = direct_state.live;
        progress.live = direct_state.live;
        progress.buffer_known = owner->buffering_level >= 0;
        progress.buffer_percent = owner->buffering_level;
        screen_status_set_progress(owner->screen_generation, &progress);
    }
    if (observed.video.output_buffers > snapshot.video.output_buffers &&
        !(hls_video && direct_state.buffering && !direct_state.live && !owner->gap_only_audio))
        screen_status_event(owner->screen_generation, SCREEN_EVENT_OUTPUT_PROGRESS);
    if (observed.failed) screen_status_fail(owner->screen_generation, SCREEN_ERROR_BACKEND);
    screen_status_get_snapshot(&snapshot);
    gchar text[sizeof(owner->overlay_last_text)];
    screen_status_format(&snapshot, text, sizeof(text), true);
    if (strcmp(text, owner->overlay_last_text) && video.output_width && video.output_height &&
        (!video.overlay_known || video.overlay_supported)) {
        GstVideoOverlayComposition *next = NULL;
        if (text[0]) next = screen_overlay_render(owner, text, video.output_width, video.output_height);
        g_mutex_lock(&owner->overlay_lock);
        if (!text[0] || next) {
            if (owner->overlay) gst_video_overlay_composition_unref(owner->overlay);
            owner->overlay = next;
            g_strlcpy(owner->overlay_last_text, text, sizeof(owner->overlay_last_text));
            owner->overlay_render_failures = 0;
        } else {
            /* A font cache may need more than one bounded pull at startup.
             * Retry briefly without changing playback or blocking its stream;
             * a missing plugin or repeated failure is explicitly unqualified. */
            owner->overlay_render_failures++;
            if (!owner->overlay_pipeline || owner->overlay_render_failures >= 3)
                screen_overlay_result(owner, FALSE, "Text renderer unavailable");
        }
        g_mutex_unlock(&owner->overlay_lock);
    }
    g_rec_mutex_unlock(&direct_mutex);
}
