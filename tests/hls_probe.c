/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Opt-in device probe. Never changes the plugin registry or receiver service.
 * Build: cc -Wall -Wextra hls_probe.c $(pkg-config --cflags --libs gstreamer-1.0) -o hls_probe
 * Usage: hls_probe auto|software kmssink|fakevideosink|fakesink SECONDS URI
 * Software mode suppresses the Pi HEVC hardware decoder only in this process.
 * Audio uses a clocked fakesink; kmssink really displays video. URIs, headers,
 * encoded codec data, and raw GStreamer error messages are not printed.
 */
#include <gst/gst.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    GstPad *pad;
    gulong id;
} PadWatch;

typedef struct {
    GstElement *video_sink;
    GPtrArray *watches;
    GMutex mutex;
    gint64 started;
    gint64 first_video;
    gboolean saw_hardware;
    gboolean saw_software;
} Probe;

static gint64 elapsed_ms(Probe *probe) {
    return (g_get_monotonic_time() - probe->started) / 1000;
}

static void watch_free(gpointer value) {
    PadWatch *watch = value;
    gst_pad_remove_probe(watch->pad, watch->id);
    gst_object_unref(watch->pad);
    g_free(watch);
}

static void report_caps(Probe *probe, GstPad *pad) {
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps)) {
        if (caps) gst_caps_unref(caps);
        return;
    }
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *format = gst_structure_get_string(s, "format");
    const gchar *drm = gst_structure_get_string(s, "drm-format");
    gint width = 0, height = 0, numerator = 0, denominator = 1;
    gst_structure_get_int(s, "width", &width);
    gst_structure_get_int(s, "height", &height);
    gst_structure_get_fraction(s, "framerate", &numerator, &denominator);
    gboolean dmabuf = gst_caps_features_contains(gst_caps_get_features(caps, 0), "memory:DMABuf");
    g_print("PROBE video-caps elapsed_ms=%" G_GINT64_FORMAT " width=%d height=%d fps=%d/%d format=%s drm_format=%s dmabuf_caps=%d\n",
            elapsed_ms(probe), width, height, numerator, denominator,
            format ? format : "unknown", drm ? drm : "none", dmabuf);
    gst_caps_unref(caps);
}

static GstPadProbeReturn video_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    Probe *probe = data;
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    g_mutex_lock(&probe->mutex);
    if (!probe->first_video) {
        probe->first_video = g_get_monotonic_time();
        report_caps(probe, pad);
        g_print("PROBE first-video-buffer elapsed_ms=%" G_GINT64_FORMAT "\n", elapsed_ms(probe));
    }
    g_mutex_unlock(&probe->mutex);
    return GST_PAD_PROBE_OK;
}

static void element_added(GstBin *top, GstBin *parent, GstElement *element, gpointer data) {
    (void)top;
    (void)parent;
    Probe *probe = data;
    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory || !gst_element_factory_list_is_type(factory, GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO)) return;
    gboolean hardware = gst_element_factory_list_is_type(factory, GST_ELEMENT_FACTORY_TYPE_HARDWARE);
    g_mutex_lock(&probe->mutex);
    probe->saw_hardware |= hardware;
    probe->saw_software |= !hardware;
    g_print("PROBE decoder factory=%s hardware=%d elapsed_ms=%" G_GINT64_FORMAT "\n",
            gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), hardware, elapsed_ms(probe));
    g_mutex_unlock(&probe->mutex);
}

static void safe_error(GstMessage *message) {
    GError *error = NULL;
    gchar *debug = NULL;
    gst_message_parse_error(message, &error, &debug);
    const gchar *flow = "unknown";
    const gchar *operation = "unknown";
    gint system_errno = 0;
    if (debug && strstr(debug, "reason not-negotiated (-4)")) flow = "not-negotiated";
    else if (debug && strstr(debug, "reason not-linked (-1)")) flow = "not-linked";
    const gchar *operations[] = {"drmModeSetPlane", "drmModeSetCrtc", "drmModePageFlip", "drmModeAddFB2", NULL};
    for (guint i = 0; debug && operations[i]; i++) {
        const gchar *match = strstr(debug, operations[i]);
        if (!match) continue;
        operation = operations[i];
        const gchar *numeric = strrchr(match, '(');
        if (numeric && sscanf(numeric, "(%d)", &system_errno) == 1 &&
            (system_errno < 1 || system_errno > 4095)) system_errno = 0;
        break;
    }
    const gchar *domain = error && error->domain == GST_STREAM_ERROR ? "stream" :
                          error && error->domain == GST_RESOURCE_ERROR ? "resource" :
                          error && error->domain == GST_CORE_ERROR ? "core" : "other";
    const gchar *factory_name = "unknown";
    if (GST_IS_ELEMENT(GST_MESSAGE_SRC(message))) {
        GstElementFactory *factory = gst_element_get_factory(GST_ELEMENT(GST_MESSAGE_SRC(message)));
        if (factory) factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    }
    g_print("PROBE error factory=%s domain=%s code=%d flow=%s operation=%s errno=%d\n", factory_name, domain, error ? error->code : 0, flow, operation, system_errno);
    g_clear_error(&error);
    g_free(debug);
}

int main(int argc, char **argv) {
    /* Parse before gst_init so --gst-debug cannot bypass the safe output contract. */
    if (argc != 5 || (strcmp(argv[1], "auto") && strcmp(argv[1], "software")) ||
        (strcmp(argv[2], "kmssink") && strcmp(argv[2], "fakevideosink") && strcmp(argv[2], "fakesink"))) {
        g_printerr("usage: hls_probe auto|software kmssink|fakevideosink|fakesink SECONDS URI\n");
        return 2;
    }
    gchar *end = NULL;
    gint64 seconds = g_ascii_strtoll(argv[3], &end, 10);
    if (!end || *end || seconds < 1 || seconds > 60) return 2;
    /* playbin3 in GStreamer 1.26 does not implement FORCE_SW_DECODERS.
     * This environment setting affects this probe only, not another process or
     * the on-disk registry. Keep other codec hardware available for comparison.
     */
    if (!strcmp(argv[1], "software")) {
        const gchar *existing = g_getenv("GST_PLUGIN_FEATURE_RANK");
        gchar *rank = g_strdup_printf("%s%sv4l2slh265dec:0", existing ? existing : "", existing && *existing ? "," : "");
        g_setenv("GST_PLUGIN_FEATURE_RANK", rank, TRUE);
        g_free(rank);
    }
    gst_init(NULL, NULL);
    /* Diagnostic environment variables can include private source information. */
    gst_debug_set_active(FALSE);
    Probe probe = {0};
    g_mutex_init(&probe.mutex);
    probe.watches = g_ptr_array_new_with_free_func(watch_free);
    probe.started = g_get_monotonic_time();
    GstElement *pipeline = gst_element_factory_make("playbin3", "device-probe");
    probe.video_sink = gst_element_factory_make(argv[2], "probe-video");
    GstElement *audio_sink = gst_element_factory_make("fakesink", "probe-audio");
    if (!pipeline || !probe.video_sink || !audio_sink) {
        g_printerr("PROBE missing required plugin\n");
        if (pipeline) gst_object_unref(pipeline);
        if (probe.video_sink) gst_object_unref(probe.video_sink);
        if (audio_sink) gst_object_unref(audio_sink);
        g_ptr_array_unref(probe.watches);
        g_mutex_clear(&probe.mutex);
        return 2;
    }
    gst_object_ref_sink(probe.video_sink);
    gst_object_ref_sink(audio_sink);
    g_object_set(probe.video_sink, "sync", TRUE, "async", TRUE, NULL);
    g_object_set(audio_sink, "sync", TRUE, "async", TRUE, NULL);
    gint flags;
    g_object_get(pipeline, "flags", &flags, NULL);
    flags |= (1 << 7) | (1 << 8); /* DOWNLOAD, BUFFERING: production defaults. */
    g_object_set(pipeline, "uri", argv[4], "video-sink", probe.video_sink, "audio-sink", audio_sink, "flags", flags, NULL);
    g_signal_connect(pipeline, "deep-element-added", G_CALLBACK(element_added), &probe);
    PadWatch *watch = g_new0(PadWatch, 1);
    watch->pad = gst_element_get_static_pad(probe.video_sink, "sink");
    watch->id = gst_pad_add_probe(watch->pad, GST_PAD_PROBE_TYPE_BUFFER, video_buffer, &probe, NULL);
    g_ptr_array_add(probe.watches, watch);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstStateChangeReturn state_result = gst_element_set_state(pipeline, GST_STATE_PAUSED);
    gboolean live = state_result == GST_STATE_CHANGE_NO_PREROLL;
    gboolean failed = state_result == GST_STATE_CHANGE_FAILURE;
    gboolean buffering = FALSE;
    gint previous_percent = -1;
    gint64 deadline = probe.started + seconds * G_USEC_PER_SEC;
    if (live) gst_element_set_state(pipeline, GST_STATE_PLAYING);
    while (!failed && g_get_monotonic_time() < deadline) {
        GstMessage *message = gst_bus_timed_pop(bus, 100 * GST_MSECOND);
        if (!message) continue;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            safe_error(message);
            failed = TRUE;
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_BUFFERING) {
            gint percent;
            gst_message_parse_buffering(message, &percent);
            if (percent / 25 != previous_percent) {
                previous_percent = percent / 25;
                g_print("PROBE buffering=%d elapsed_ms=%" G_GINT64_FORMAT "\n", percent, elapsed_ms(&probe));
            }
            buffering = percent < 100;
            if (!live) gst_element_set_state(pipeline, buffering ? GST_STATE_PAUSED : GST_STATE_PLAYING);
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ASYNC_DONE && !buffering) {
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            gst_message_unref(message);
            break;
        }
        gst_message_unref(message);
    }
    GstStructure *stats = NULL;
    guint64 rendered = 0, dropped = 0;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(probe.video_sink), "stats")) {
        g_object_get(probe.video_sink, "stats", &stats, NULL);
        if (stats) {
            gst_structure_get_uint64(stats, "rendered", &rendered);
            gst_structure_get_uint64(stats, "dropped", &dropped);
            gst_structure_free(stats);
        }
    }
    g_mutex_lock(&probe.mutex);
    gboolean succeeded = !failed && probe.first_video && rendered > 0;
    g_print("PROBE result success=%d failed=%d first_video_ms=%" G_GINT64_FORMAT " rendered=%" G_GUINT64_FORMAT " dropped=%" G_GUINT64_FORMAT " saw_hardware=%d saw_software=%d\n",
            succeeded, failed, probe.first_video ? (probe.first_video - probe.started) / 1000 : -1,
            rendered, dropped, probe.saw_hardware, probe.saw_software);
    g_mutex_unlock(&probe.mutex);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_ptr_array_unref(probe.watches);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    gst_object_unref(probe.video_sink);
    gst_object_unref(audio_sink);
    g_mutex_clear(&probe.mutex);
    gst_deinit();
    return succeeded ? 0 : 1;
}
