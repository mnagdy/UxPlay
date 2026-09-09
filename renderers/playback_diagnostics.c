/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "playback_diagnostics.h"
#include <gst/video/video-format.h>
#include <string.h>

/* A broken stream must not create unbounded observers or diagnostics output. */
#define MAX_OBSERVED_ELEMENTS 16
#define MAX_ERROR_REPORTS 8

typedef enum {
    OBSERVE_DECODER_INPUT,
    OBSERVE_DECODER_OUTPUT,
    OBSERVE_VIDEO_SINK
} observation_kind_t;

typedef struct {
    playback_diagnostics_t *owner;
    GstPad *pad;
    gulong probe_id;
    gboolean fired;
    observation_kind_t kind;
    gchar *factory;
} pad_observation_t;

struct playback_diagnostics_s {
    logger_t *logger;
    GstElement *pipeline;
    gint64 requested_at_us;
    GMutex lock;
    GHashTable *elements;
    GPtrArray *probes;
    gulong element_added_id;
    gboolean saw_decoder_output;
    gboolean saw_video_sink;
    gboolean saw_preroll;
    gboolean saw_playing;
    gint buffering_bucket;
    guint error_reports;
};

static gint64 elapsed_ms(playback_diagnostics_t *d) {
    return MAX((gint64) 0, g_get_monotonic_time() - d->requested_at_us) / 1000;
}

static const gchar *error_domain_name(const GError *error) {
    if (!error) return "unknown";
    if (error->domain == GST_STREAM_ERROR) return "stream";
    if (error->domain == GST_RESOURCE_ERROR) return "resource";
    if (error->domain == GST_CORE_ERROR) return "core";
    if (error->domain == GST_LIBRARY_ERROR) return "library";
    return "other";
}

/* GStreamer debug strings can include signed source URLs and arbitrary paths.
 * Extract only known flow-return descriptions, never the full debug payload.
 */
static const gchar *error_flow_reason(const gchar *debug) {
    static const struct {
        const gchar *description;
        const gchar *name;
    } reasons[] = {
        {"reason not-linked (-1)", "not-linked"},
        {"reason flushing (-2)", "flushing"},
        {"reason eos (-3)", "eos"},
        {"reason not-negotiated (-4)", "not-negotiated"},
        {"reason error (-5)", "error"},
        {"reason not-supported (-6)", "not-supported"}
    };
    if (!debug || !debug[0]) return "unavailable";
    for (guint i = 0; i < G_N_ELEMENTS(reasons); i++) {
        if (strstr(debug, reasons[i].description)) return reasons[i].name;
    }
    return "unclassified";
}

static const gchar *message_factory_name(GstMessage *message) {
    GstObject *source = GST_MESSAGE_SRC(message);
    if (source && GST_IS_ELEMENT(source)) {
        GstElementFactory *factory = gst_element_get_factory(GST_ELEMENT(source));
        if (factory) return gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    }
    return "unknown";
}

static const gchar *codec_name(const GstStructure *s) {
    const gchar *name = gst_structure_get_name(s);
    if (!strcmp(name, "video/x-h264")) return "H264";
    if (!strcmp(name, "video/x-h265")) return "HEVC";
    if (!strcmp(name, "video/x-av1")) return "AV1";
    if (!strcmp(name, "video/x-vp8")) return "VP8";
    if (!strcmp(name, "video/x-vp9")) return "VP9";
    if (!strcmp(name, "video/mpeg")) return "MPEG";
    if (!strcmp(name, "image/jpeg")) return "JPEG";
    if (!strcmp(name, "video/x-raw")) return "raw";
    return "other";
}

static const gchar *profile_name(const GstStructure *s) {
    static const gchar *known[] = {
        "baseline", "constrained-baseline", "main", "extended", "high",
        "high-10", "high-10-intra", "high-4:2:2", "high-4:2:2-intra",
        "high-4:4:4", "high-4:4:4-intra", "high-4:4:4-predictive",
        "main-10", "main-still-picture", "main-12", "main-422-10",
        "main-422-12", "main-444", "main-444-10", "main-444-12",
        "simple", "advanced-simple", "professional", "0", "1", "2", "3"
    };
    const gchar *profile = gst_structure_get_string(s, "profile");
    if (!profile) return "unknown";
    for (guint i = 0; i < G_N_ELEMENTS(known); i++) {
        if (!strcmp(profile, known[i])) return known[i];
    }
    return "other";
}

static const gchar *caps_memory_name(const GstCapsFeatures *features) {
    if (!features || gst_caps_features_is_any(features)) return "unknown";
    if (gst_caps_features_contains(features, "memory:DMABuf")) return "DMABuf";
    if (gst_caps_features_contains(features, "memory:GLMemory")) return "GLMemory";
    if (gst_caps_features_contains(features, "memory:VASurface")) return "VASurface";
    if (gst_caps_features_contains(features, "memory:SystemMemory")) return "SystemMemory";
    return "other";
}

static const gchar *buffer_memory_name(GstBuffer *buffer) {
    const gchar *result = NULL;
    guint count = gst_buffer_n_memory(buffer);
    if (!count) return "none";
    for (guint i = 0; i < count; i++) {
        GstMemory *memory = gst_buffer_peek_memory(buffer, i);
        const gchar *kind = "other";
        if (gst_memory_is_type(memory, "dmabuf")) kind = "DMABuf";
        else if (gst_memory_is_type(memory, "SystemMemory")) kind = "SystemMemory";
        else if (gst_memory_is_type(memory, "GLMemory")) kind = "GLMemory";
        if (result && strcmp(result, kind)) return "mixed";
        result = kind;
    }
    return result;
}

/* Only fixed, explicitly selected fields are logged. In particular, caps may
 * contain codec data or upstream metadata and must never be stringified here.
 * Inspecting memory types does not map, copy or read the decoded pixel data.
 */
static void log_caps(pad_observation_t *observation, GstBuffer *buffer) {
    playback_diagnostics_t *d = observation->owner;
    GstCaps *caps = gst_pad_get_current_caps(observation->pad);
    const gchar *stage = observation->kind == OBSERVE_DECODER_INPUT ? "decoder-input" : "decoder-output";
    if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps)) {
        logger_log(d->logger, LOGGER_INFO,
                   "Direct playback: stage=%s factory=%s elapsed_ms=%" G_GINT64_FORMAT " caps=unavailable",
                   stage, observation->factory, elapsed_ms(d));
    } else {
        const GstStructure *s = gst_caps_get_structure(caps, 0);
        gint width = 0, height = 0, fps_n = 0, fps_d = 1;
        gst_structure_get_int(s, "width", &width);
        gst_structure_get_int(s, "height", &height);
        gst_structure_get_fraction(s, "framerate", &fps_n, &fps_d);
        if (observation->kind == OBSERVE_DECODER_INPUT) {
            logger_log(d->logger, LOGGER_INFO,
                       "Direct playback: stage=decoder-input factory=%s elapsed_ms=%" G_GINT64_FORMAT
                       " codec=%s profile=%s width=%d height=%d fps=%d/%d",
                       observation->factory, elapsed_ms(d), codec_name(s), profile_name(s),
                       width, height, fps_n, fps_d);
        } else {
            const gchar *format = gst_structure_get_string(s, "format");
            const gchar *safe_format = "unknown";
            if (format) {
                GstVideoFormat video_format = gst_video_format_from_string(format);
                if (video_format != GST_VIDEO_FORMAT_UNKNOWN) {
                    safe_format = gst_video_format_to_string(video_format);
                } else if (!strcmp(format, "DMA_DRM")) {
                    safe_format = "DMA_DRM";
                }
            }
            logger_log(d->logger, LOGGER_INFO,
                       "Direct playback: stage=decoder-output factory=%s elapsed_ms=%" G_GINT64_FORMAT
                       " width=%d height=%d fps=%d/%d format=%s caps_memory=%s buffer_memory=%s",
                       observation->factory, elapsed_ms(d), width, height, fps_n, fps_d,
                       safe_format, caps_memory_name(gst_caps_get_features(caps, 0)),
                       buffer_memory_name(buffer));
        }
    }
    if (caps) gst_caps_unref(caps);
}

static GstPadProbeReturn first_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    pad_observation_t *observation = user_data;
    playback_diagnostics_t *d = observation->owner;
    GstBuffer *buffer = NULL;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
        if (gst_buffer_list_length(list)) buffer = gst_buffer_list_get(list, 0);
    }
    if (!buffer) return GST_PAD_PROBE_OK;
    (void) pad;
    g_mutex_lock(&d->lock);
    observation->fired = TRUE;
    if (observation->kind == OBSERVE_VIDEO_SINK) {
        if (!d->saw_video_sink) {
            d->saw_video_sink = TRUE;
            logger_log(d->logger, LOGGER_INFO,
                       "Direct playback: stage=first-video-sink-buffer factory=%s elapsed_ms=%" G_GINT64_FORMAT
                       " buffer_memory=%s (arrival, not physical presentation)",
                       observation->factory, elapsed_ms(d), buffer_memory_name(buffer));
        }
    } else {
        if (observation->kind == OBSERVE_DECODER_OUTPUT) d->saw_decoder_output = TRUE;
        log_caps(observation, buffer);
    }
    g_mutex_unlock(&d->lock);
    return GST_PAD_PROBE_REMOVE;
}

/* Called with d->lock held. BUFFER probes never execute synchronously in
 * gst_pad_add_probe (unlike IDLE probes), so the callback can take this lock.
 */
static void observe_pad(playback_diagnostics_t *d, GstElement *element,
                        const gchar *pad_name, observation_kind_t kind,
                        const gchar *factory) {
    GstPad *pad = gst_element_get_static_pad(element, pad_name);
    if (!pad) return;
    pad_observation_t *observation = g_new0(pad_observation_t, 1);
    observation->owner = d;
    observation->pad = pad;
    observation->kind = kind;
    observation->factory = g_strdup(factory);
    g_ptr_array_add(d->probes, observation);
    observation->probe_id = gst_pad_add_probe(pad,
                                              GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST,
                                              first_buffer, observation, NULL);
}

static void observe_element(playback_diagnostics_t *d, GstElement *element) {
    if (GST_IS_BIN(element)) return; /* Probe the concrete sink, not a ghost pad. */
    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory) return;
    gboolean decoder = gst_element_factory_list_is_type(factory,
                           GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO);
    gboolean sink = gst_element_factory_list_is_type(factory,
                        GST_ELEMENT_FACTORY_TYPE_SINK | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO);
    if (!decoder && !sink) return;
    const gchar *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    g_mutex_lock(&d->lock);
    if (!g_hash_table_lookup(d->elements, element) &&
        g_hash_table_size(d->elements) < MAX_OBSERVED_ELEMENTS) {
        /* Keep a reference so a removed element's address cannot be reused. */
        g_hash_table_insert(d->elements, gst_object_ref(element), element);
        if (decoder) {
            observe_pad(d, element, "sink", OBSERVE_DECODER_INPUT, name);
            observe_pad(d, element, "src", OBSERVE_DECODER_OUTPUT, name);
        }
        if (sink) observe_pad(d, element, "sink", OBSERVE_VIDEO_SINK, name);
    }
    g_mutex_unlock(&d->lock);
}

static void observe_existing(playback_diagnostics_t *d, GstBin *bin) {
    GstIterator *iterator = gst_bin_iterate_recurse(bin);
    GValue value = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(iterator, &value)) {
        case GST_ITERATOR_OK:
            observe_element(d, GST_ELEMENT(g_value_get_object(&value)));
            g_value_reset(&value);
            break;
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(iterator);
            break;
        default:
            done = TRUE;
            break;
        }
    }
    if (G_VALUE_TYPE(&value)) g_value_unset(&value);
    gst_iterator_free(iterator);
}

static void element_added(GstBin *pipeline, GstBin *sub_bin,
                          GstElement *element, gpointer user_data) {
    playback_diagnostics_t *d = user_data;
    (void) pipeline;
    (void) sub_bin;
    observe_element(d, element);
    /* A bin can already contain elements when it is added to playbin. */
    if (GST_IS_BIN(element)) observe_existing(d, GST_BIN(element));
}

playback_diagnostics_t *playback_diagnostics_attach(GstElement *playbin,
                                                   logger_t *logger,
                                                   gint64 requested_at_us) {
    g_return_val_if_fail(GST_IS_BIN(playbin), NULL);
    g_return_val_if_fail(logger != NULL, NULL);
    playback_diagnostics_t *d = g_new0(playback_diagnostics_t, 1);
    d->logger = logger;
    d->pipeline = gst_object_ref(playbin);
    d->requested_at_us = requested_at_us > 0 ? requested_at_us : g_get_monotonic_time();
    d->buffering_bucket = -1;
    g_mutex_init(&d->lock);
    d->elements = g_hash_table_new_full(g_direct_hash, g_direct_equal, gst_object_unref, NULL);
    d->probes = g_ptr_array_new();
    if (g_signal_lookup("deep-element-added", GST_TYPE_BIN)) {
        d->element_added_id = g_signal_connect(playbin, "deep-element-added", G_CALLBACK(element_added), d);
    } else {
        logger_log(logger, LOGGER_INFO,
                   "Direct playback: dynamic decoder/sink observations require GStreamer 1.10 or later");
    }
    observe_existing(d, GST_BIN(playbin));
    logger_log(logger, LOGGER_INFO,
               "Direct playback: stage=pipeline-setup elapsed_ms=%" G_GINT64_FORMAT,
               elapsed_ms(d));
    return d;
}

void playback_diagnostics_message(playback_diagnostics_t *d, GstMessage *message) {
    if (!d || !message) return;
    g_mutex_lock(&d->lock);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR:
        if (d->error_reports < MAX_ERROR_REPORTS) {
            GError *error = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(message, &error, &debug);
            d->error_reports++;
            logger_log(d->logger, LOGGER_ERR,
                       "Direct playback: stage=error factory=%s elapsed_ms=%" G_GINT64_FORMAT
                       " domain=%s code=%d flow_reason=%s",
                       message_factory_name(message), elapsed_ms(d), error_domain_name(error),
                       error ? error->code : 0, error_flow_reason(debug));
            g_clear_error(&error);
            g_free(debug);
        }
        break;
    case GST_MESSAGE_BUFFERING: {
        gint percent = 0;
        gst_message_parse_buffering(message, &percent);
        percent = CLAMP(percent, 0, 100);
        gint bucket = percent / 25;
        if (bucket > d->buffering_bucket) {
            d->buffering_bucket = bucket;
            logger_log(d->logger, LOGGER_INFO,
                       "Direct playback: stage=buffering percent=%d elapsed_ms=%" G_GINT64_FORMAT,
                       percent, elapsed_ms(d));
        }
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(d->pipeline)) {
            GstState old_state, new_state, pending;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending);
            if (new_state == GST_STATE_PAUSED && !d->saw_preroll) {
                d->saw_preroll = TRUE;
                logger_log(d->logger, LOGGER_INFO,
                           "Direct playback: stage=paused-ready elapsed_ms=%" G_GINT64_FORMAT
                           " (preroll complete, or live source without preroll)", elapsed_ms(d));
            }
            if (new_state == GST_STATE_PLAYING && !d->saw_playing) {
                d->saw_playing = TRUE;
                logger_log(d->logger, LOGGER_INFO,
                           "Direct playback: stage=playing elapsed_ms=%" G_GINT64_FORMAT,
                           elapsed_ms(d));
            }
        }
        break;
    default:
        break;
    }
    g_mutex_unlock(&d->lock);
}

void playback_diagnostics_free(playback_diagnostics_t *d) {
    if (!d) return;
    if (d->element_added_id) g_signal_handler_disconnect(d->pipeline, d->element_added_id);
    /* NULL has stopped streaming before any observer storage is freed. */
    for (guint i = 0; i < d->probes->len; i++) {
        pad_observation_t *observation = g_ptr_array_index(d->probes, i);
        if (observation->probe_id && !observation->fired) {
            gst_pad_remove_probe(observation->pad, observation->probe_id);
        }
        gst_object_unref(observation->pad);
        g_free(observation->factory);
        g_free(observation);
    }
    logger_log(d->logger, LOGGER_INFO,
               "Direct playback: stage=session-ended elapsed_ms=%" G_GINT64_FORMAT
               " decoder_output_seen=%d video_sink_buffer_seen=%d",
               elapsed_ms(d), d->saw_decoder_output, d->saw_video_sink);
    g_ptr_array_free(d->probes, TRUE);
    g_hash_table_destroy(d->elements);
    gst_object_unref(d->pipeline);
    g_mutex_clear(&d->lock);
    g_free(d);
}
