/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "playback_diagnostics.h"
#include <gst/audio/audio-format.h>
#include <gst/video/video-format.h>
#include <string.h>

/* A broken stream must not create unbounded observers or diagnostics output. */
#define MAX_OBSERVED_ELEMENTS 16
#define MAX_ERROR_REPORTS 8
#define MAX_NETWORK_REPORTS 32
#define MAX_BUFFERING_REPORTS 64
#define MAX_WAIT_REPORTS 24

typedef enum {
    OBSERVE_DECODER_INPUT,
    OBSERVE_DECODER_OUTPUT,
    OBSERVE_VIDEO_SINK,
    OBSERVE_AUDIO_DECODER_INPUT,
    OBSERVE_AUDIO_DECODER_OUTPUT,
    OBSERVE_AUDIO_SINK,
    OBSERVE_SOURCE,
    OBSERVE_MEDIA_INPUT,
    OBSERVE_MANIFEST
} observation_kind_t;

typedef struct {
    playback_diagnostics_t *owner;
    GstPad *pad;
    gulong probe_id;
    gboolean fired;
    gboolean persistent;
    observation_kind_t kind;
    gchar *factory;
} pad_observation_t;

struct playback_diagnostics_s {
    logger_t *logger;
    GstElement *pipeline;
    gint64 requested_at_us;
    guint64 session_id;
    GMutex lock;
    GHashTable *elements;
    GPtrArray *probes;
    gulong element_added_id;
    gboolean saw_decoder_output;
    gboolean saw_video_sink;
    gboolean saw_audio_decoder_input;
    gboolean saw_audio_decoder_output;
    gboolean saw_audio_sink;
    gboolean saw_preroll;
    gboolean saw_playing;
    GHashTable *buffering_sources;
    guint buffering_reports;
    guint error_reports;
    guint network_reports;
    guint control_reports;
    guint wait_reports;
    guint http_responses;
    guint manifest_reports;
    GByteArray *manifest;
    gboolean manifest_truncated;
    guint64 source_bytes;
    guint64 media_bytes;
    gint64 last_source_us;
    gint64 last_media_us;
    gint64 next_wait_us;
    GstElement *adaptive_demux;
};

/* Every stage can be correlated across pipeline replacement. The text after
 * the prefix contains only explicitly selected fields, never raw structures. */
#define DLOG(d, level, format, ...) \
    logger_log((d)->logger, level, "Direct playback: session=%" G_GUINT64_FORMAT " " format, \
               (d)->session_id, __VA_ARGS__)

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
        DLOG(d, LOGGER_INFO, "stage=%s factory=%s elapsed_ms=%" G_GINT64_FORMAT " caps=unavailable",
                   stage, observation->factory, elapsed_ms(d));
    } else {
        const GstStructure *s = gst_caps_get_structure(caps, 0);
        gint width = 0, height = 0, fps_n = 0, fps_d = 1;
        gst_structure_get_int(s, "width", &width);
        gst_structure_get_int(s, "height", &height);
        gst_structure_get_fraction(s, "framerate", &fps_n, &fps_d);
        if (observation->kind == OBSERVE_DECODER_INPUT) {
            DLOG(d, LOGGER_INFO, "stage=decoder-input factory=%s elapsed_ms=%" G_GINT64_FORMAT
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
            DLOG(d, LOGGER_INFO, "stage=decoder-output factory=%s elapsed_ms=%" G_GINT64_FORMAT
                       " width=%d height=%d fps=%d/%d format=%s caps_memory=%s buffer_memory=%s",
                       observation->factory, elapsed_ms(d), width, height, fps_n, fps_d,
                       safe_format, caps_memory_name(gst_caps_get_features(caps, 0)),
                       buffer_memory_name(buffer));
        }
    }
    if (caps) gst_caps_unref(caps);
}

static gboolean audio_observation(observation_kind_t kind) {
    return kind == OBSERVE_AUDIO_DECODER_INPUT || kind == OBSERVE_AUDIO_DECODER_OUTPUT ||
        kind == OBSERVE_AUDIO_SINK;
}

static gboolean audio_media_buffer(GstBuffer *buffer) {
    return buffer && gst_buffer_get_size(buffer) > 0 &&
        !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_GAP) &&
        !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_HEADER);
}

/* Payloads and arbitrary caps never enter the log. Decoder input establishes
 * compressed media arrival; decoded PCM and sink arrival are separate stages.
 * None of these observations establishes that a physical speaker made sound. */
static void log_audio_caps(pad_observation_t *observation) {
    playback_diagnostics_t *d = observation->owner;
    GstCaps *caps = gst_pad_get_current_caps(observation->pad);
    const GstStructure *s = caps && !gst_caps_is_empty(caps) && !gst_caps_is_any(caps) ?
        gst_caps_get_structure(caps, 0) : NULL;
    const gchar *codec = "unknown", *format = "unknown";
    gint rate = 0, channels = 0, version = 0, layer = 0;
    if (s) {
        gst_structure_get_int(s, "rate", &rate);
        gst_structure_get_int(s, "channels", &channels);
        if (gst_structure_has_name(s, "audio/mpeg")) {
            gst_structure_get_int(s, "mpegversion", &version);
            gst_structure_get_int(s, "layer", &layer);
            codec = version == 2 || version == 4 ? "AAC" :
                version == 1 && layer == 2 ? "MP2" :
                version == 1 && layer == 3 ? "MP3" : "MPEG";
        } else if (gst_structure_has_name(s, "audio/x-ac3")) codec = "AC3";
        else if (gst_structure_has_name(s, "audio/x-eac3")) codec = "EAC3";
        else if (gst_structure_has_name(s, "audio/x-alac")) codec = "ALAC";
        else if (gst_structure_has_name(s, "audio/x-opus")) codec = "Opus";
        else if (gst_structure_has_name(s, "audio/x-vorbis")) codec = "Vorbis";
        else if (gst_structure_has_name(s, "audio/x-flac")) codec = "FLAC";
        else if (gst_structure_has_name(s, "audio/x-raw")) codec = "PCM";
        const gchar *raw_format = gst_structure_get_string(s, "format");
        GstAudioFormat audio_format = raw_format ? gst_audio_format_from_string(raw_format) : GST_AUDIO_FORMAT_UNKNOWN;
        if (audio_format != GST_AUDIO_FORMAT_UNKNOWN && audio_format != GST_AUDIO_FORMAT_ENCODED)
            format = gst_audio_format_to_string(audio_format);
    }
    const gchar *stage = observation->kind == OBSERVE_AUDIO_DECODER_INPUT ? "audio-decoder-input" :
        observation->kind == OBSERVE_AUDIO_DECODER_OUTPUT ? "audio-decoder-output" : "first-audio-sink-buffer";
    DLOG(d, LOGGER_INFO, "stage=%s factory=%s elapsed_ms=%" G_GINT64_FORMAT
         " codec=%s rate=%d channels=%d format=%s%s", stage, observation->factory, elapsed_ms(d),
         codec, rate, channels, format,
         observation->kind == OBSERVE_AUDIO_SINK ? " (arrival, not physical audibility)" : "");
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
        for (guint i = 0; i < gst_buffer_list_length(list); i++) {
            GstBuffer *part = gst_buffer_list_get(list, i);
            if (!audio_observation(observation->kind) || audio_media_buffer(part)) {
                buffer = part;
                break;
            }
        }
    }
    if (!buffer) return GST_PAD_PROBE_OK;
    if (audio_observation(observation->kind) && !audio_media_buffer(buffer)) return GST_PAD_PROBE_OK;
    (void) pad;
    g_mutex_lock(&d->lock);
    if (observation->persistent) {
        guint64 bytes = GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST ?
            gst_buffer_list_calculate_size(GST_PAD_PROBE_INFO_BUFFER_LIST(info)) : gst_buffer_get_size(buffer);
        if (observation->kind == OBSERVE_SOURCE) {
            d->source_bytes += bytes;
            d->last_source_us = g_get_monotonic_time();
        } else if (observation->kind == OBSERVE_MEDIA_INPUT) {
            d->media_bytes += bytes;
            d->last_media_us = g_get_monotonic_time();
        } else {
            GstBufferList *list = GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST ?
                GST_PAD_PROBE_INFO_BUFFER_LIST(info) : NULL;
            guint count = list ? gst_buffer_list_length(list) : 1;
            for (guint i = 0; i < count; i++) {
                GstBuffer *part = list ? gst_buffer_list_get(list, i) : buffer;
                gsize size = gst_buffer_get_size(part);
                gsize keep = MIN(size, 65536 - d->manifest->len);
                guint offset = d->manifest->len;
                g_byte_array_set_size(d->manifest, offset + keep);
                if (keep) gst_buffer_extract(part, 0, d->manifest->data + offset, keep);
                if (keep < size) d->manifest_truncated = TRUE;
            }
        }
        if (observation->fired) {
            g_mutex_unlock(&d->lock);
            return GST_PAD_PROBE_OK;
        }
    }
    observation->fired = TRUE;
    if (observation->kind >= OBSERVE_SOURCE) {
        const gchar *stage = observation->kind == OBSERVE_SOURCE ? "first-source-data" :
            observation->kind == OBSERVE_MEDIA_INPUT ? "first-media-input" : "manifest-input";
        DLOG(d, LOGGER_INFO, "stage=%s factory=%s elapsed_ms=%" G_GINT64_FORMAT " bytes=%" G_GSIZE_FORMAT,
             stage, observation->factory, elapsed_ms(d), gst_buffer_get_size(buffer));
    } else if (observation->kind == OBSERVE_VIDEO_SINK) {
        if (!d->saw_video_sink) {
            d->saw_video_sink = TRUE;
            DLOG(d, LOGGER_INFO, "stage=first-video-sink-buffer factory=%s elapsed_ms=%" G_GINT64_FORMAT
                       " buffer_memory=%s (arrival, not physical presentation)",
                       observation->factory, elapsed_ms(d), buffer_memory_name(buffer));
        }
    } else if (audio_observation(observation->kind)) {
        gboolean *seen = observation->kind == OBSERVE_AUDIO_DECODER_INPUT ? &d->saw_audio_decoder_input :
            observation->kind == OBSERVE_AUDIO_DECODER_OUTPUT ? &d->saw_audio_decoder_output : &d->saw_audio_sink;
        if (!*seen) {
            *seen = TRUE;
            log_audio_caps(observation);
        }
    } else {
        if (observation->kind == OBSERVE_DECODER_OUTPUT) d->saw_decoder_output = TRUE;
        log_caps(observation, buffer);
    }
    g_mutex_unlock(&d->lock);
    return observation->persistent ? GST_PAD_PROBE_OK : GST_PAD_PROBE_REMOVE;
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
    observation->persistent = kind >= OBSERVE_SOURCE;
    observation->factory = g_strdup(factory);
    g_ptr_array_add(d->probes, observation);
    observation->probe_id = gst_pad_add_probe(pad,
                                              GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST,
                                              first_buffer, observation, NULL);
}

static void observe_element(playback_diagnostics_t *d, GstElement *element) {
    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory) return;
    gboolean decoder = gst_element_factory_list_is_type(factory,
                           GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO);
    gboolean sink = gst_element_factory_list_is_type(factory,
                        GST_ELEMENT_FACTORY_TYPE_SINK | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO);
    gboolean audio_decoder = gst_element_factory_list_is_type(factory,
                        GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO);
    gboolean audio_sink = gst_element_factory_list_is_type(factory,
                        GST_ELEMENT_FACTORY_TYPE_SINK | GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO);
    const gchar *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    gboolean source = !strcmp(name, "souphttpsrc");
    gboolean media = FALSE;
    if (!strcmp(name, "parsebin")) {
        GstObject *parent = gst_object_get_parent(GST_OBJECT(element));
        if (parent && GST_IS_ELEMENT(parent)) {
            GstElementFactory *pf = gst_element_get_factory(GST_ELEMENT(parent));
            const gchar *pn = pf ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(pf)) : "";
            media = !strcmp(pn, "hlsdemux2") || !strcmp(pn, "hlsdemux");
        }
        if (parent) gst_object_unref(parent);
    }
    gboolean adaptive = !strcmp(name, "hlsdemux2") || !strcmp(name, "hlsdemux");
    if (!decoder && !sink && !audio_decoder && !audio_sink && !source && !media && !adaptive) return;
    if (GST_IS_BIN(element) && !adaptive && !media) return;
    g_mutex_lock(&d->lock);
    if (!g_hash_table_lookup(d->elements, element) &&
        g_hash_table_size(d->elements) < MAX_OBSERVED_ELEMENTS) {
        /* Keep a reference so a removed element's address cannot be reused. */
        g_hash_table_insert(d->elements, gst_object_ref(element), element);
        DLOG(d, LOGGER_INFO, "stage=element-added factory=%s elapsed_ms=%" G_GINT64_FORMAT,
             name, elapsed_ms(d));
        if (source) observe_pad(d, element, "src", OBSERVE_SOURCE, name);
        if (media) observe_pad(d, element, "sink", OBSERVE_MEDIA_INPUT, name);
        if (adaptive) {
            observe_pad(d, element, "sink", OBSERVE_MANIFEST, name);
            if (!d->adaptive_demux) d->adaptive_demux = gst_object_ref(element);
        }
        if (decoder) {
            observe_pad(d, element, "sink", OBSERVE_DECODER_INPUT, name);
            observe_pad(d, element, "src", OBSERVE_DECODER_OUTPUT, name);
        }
        if (sink) observe_pad(d, element, "sink", OBSERVE_VIDEO_SINK, name);
        if (audio_decoder) {
            observe_pad(d, element, "sink", OBSERVE_AUDIO_DECODER_INPUT, name);
            observe_pad(d, element, "src", OBSERVE_AUDIO_DECODER_OUTPUT, name);
        }
        if (audio_sink) observe_pad(d, element, "sink", OBSERVE_AUDIO_SINK, name);
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
                                                   gint64 requested_at_us,
                                                   guint64 session_id) {
    g_return_val_if_fail(GST_IS_BIN(playbin), NULL);
    g_return_val_if_fail(logger != NULL, NULL);
    playback_diagnostics_t *d = g_new0(playback_diagnostics_t, 1);
    d->logger = logger;
    d->pipeline = gst_object_ref(playbin);
    d->requested_at_us = requested_at_us > 0 ? requested_at_us : g_get_monotonic_time();
    d->session_id = session_id;
    d->next_wait_us = d->requested_at_us + 5 * G_USEC_PER_SEC;
    g_mutex_init(&d->lock);
    d->elements = g_hash_table_new_full(g_direct_hash, g_direct_equal, gst_object_unref, NULL);
    d->buffering_sources = g_hash_table_new_full(g_direct_hash, g_direct_equal, gst_object_unref, g_free);
    d->probes = g_ptr_array_new();
    d->manifest = g_byte_array_new();
    if (g_signal_lookup("deep-element-added", GST_TYPE_BIN)) {
        d->element_added_id = g_signal_connect(playbin, "deep-element-added", G_CALLBACK(element_added), d);
    } else {
        logger_log(logger, LOGGER_INFO,
                   "Direct playback: dynamic decoder/sink observations require GStreamer 1.10 or later");
    }
    observe_existing(d, GST_BIN(playbin));
    DLOG(d, LOGGER_INFO, "stage=pipeline-setup elapsed_ms=%" G_GINT64_FORMAT, elapsed_ms(d));
    return d;
}

static const gchar *header_value(const GstStructure *headers, const gchar *name) {
    if (!headers) return NULL;
    for (gint i = 0; i < gst_structure_n_fields(headers); i++) {
        const gchar *field = gst_structure_nth_field_name(headers, i);
        if (!g_ascii_strcasecmp(field, name)) return gst_structure_get_string(headers, field);
    }
    return NULL;
}

static gint64 numeric_header(const GstStructure *headers, const gchar *name) {
    const gchar *value = header_value(headers, name);
    if (!value || !value[0] || strlen(value) > 18) return -1;
    for (const gchar *p = value; *p; p++) if (!g_ascii_isdigit(*p)) return -1;
    return g_ascii_strtoll(value, NULL, 10);
}

static const gchar *safe_content_type(const gchar *value) {
    static const gchar *known[] = { "application/vnd.apple.mpegurl", "application/x-mpegurl",
        "application/x-hls", "video/mp2t", "video/mp4", "audio/mp4", "video/webm", "application/octet-stream" };
    if (!value) return "unknown";
    for (guint i = 0; i < G_N_ELEMENTS(known); i++) {
        gsize length = strlen(known[i]);
        if (!g_ascii_strncasecmp(value, known[i], length) &&
            (!value[length] || value[length] == ';' || value[length] == ' ')) return known[i];
    }
    return "other";
}

/* Native HTTP bus messages expose the initial source response. Adaptive HLS
 * segment requests are internal to hlsdemux2 on 1.26; do not pretend these are
 * a complete segment trace. parser-input probes and demux levels cover that path. */
static void network_message(playback_diagnostics_t *d, GstMessage *message) {
    const GstStructure *s = gst_message_get_structure(message);
    if (!s) return;
    if (gst_structure_has_name(s, "http-headers")) {
        d->http_responses++;
        if (d->network_reports++ >= MAX_NETWORK_REPORTS) return;
        guint status = 0;
        gst_structure_get_uint(s, "http-status-code", &status);
        const GValue *value = gst_structure_get_value(s, "response-headers");
        const GstStructure *headers = value && GST_VALUE_HOLDS_STRUCTURE(value) ? gst_value_get_structure(value) : NULL;
        const gchar *transfer = header_value(headers, "Transfer-Encoding");
        DLOG(d, LOGGER_INFO, "stage=http-response factory=%s elapsed_ms=%" G_GINT64_FORMAT
             " response=%u status=%u content_type=%s content_length=%" G_GINT64_FORMAT " chunked=%d",
             message_factory_name(message), elapsed_ms(d), d->http_responses, status,
             safe_content_type(header_value(headers, "Content-Type")), numeric_header(headers, "Content-Length"),
             transfer && !g_ascii_strcasecmp(transfer, "chunked"));
    } else if (gst_structure_has_name(s, "adaptive-streaming-statistics")) {
        /* The manifest message is emitted after processing, even on failure:
         * seeing it proves processing was attempted, not a valid playlist. */
        if (gst_structure_has_field(s, "manifest-download-stop")) {
            d->manifest_reports++;
            if (d->network_reports++ < MAX_NETWORK_REPORTS)
                DLOG(d, LOGGER_INFO, "stage=manifest-processed factory=%s elapsed_ms=%" G_GINT64_FORMAT
                     " count=%u", message_factory_name(message), elapsed_ms(d), d->manifest_reports);
        }
    }
}

static const gchar *buffer_mode_name(GstBufferingMode mode) {
    switch (mode) {
    case GST_BUFFERING_STREAM: return "stream";
    case GST_BUFFERING_DOWNLOAD: return "download";
    case GST_BUFFERING_TIMESHIFT: return "timeshift";
    case GST_BUFFERING_LIVE: return "live";
    default: return "unknown";
    }
}

typedef struct { gint bucket; guint id; } buffering_source_t;

/* Summarise only HLS directives from a bounded copy of the initial manifest.
 * Never output playlist lines, URIs, tags, keys, or arbitrary attribute values. */
static void manifest_summary(playback_diagnostics_t *d) {
    gchar *body = g_strndup((const gchar *)d->manifest->data, d->manifest->len);
    gchar **lines = g_strsplit(body ? body : "", "\n", -1);
    gint64 target = -1, sequence = -1;
    guint entries = 0, variants = 0;
    gboolean endlist = FALSE;
    for (guint i = 0; lines[i]; i++) {
        gchar *line = g_strchomp(lines[i]);
        if (g_str_has_prefix(line, "#EXTINF:")) entries++;
        if (g_str_has_prefix(line, "#EXT-X-STREAM-INF:")) variants++;
        if (!strcmp(line, "#EXT-X-ENDLIST")) endlist = TRUE;
        const gchar *number = NULL;
        gint64 *out = NULL;
        if (g_str_has_prefix(line, "#EXT-X-TARGETDURATION:")) { number = line + 22; out = &target; }
        if (g_str_has_prefix(line, "#EXT-X-MEDIA-SEQUENCE:")) { number = line + 22; out = &sequence; }
        if (number && *number && strlen(number) <= 18) {
            gboolean valid = TRUE;
            for (const gchar *p = number; *p; p++) if (!g_ascii_isdigit(*p)) valid = FALSE;
            if (valid) *out = g_ascii_strtoll(number, NULL, 10);
        }
    }
    DLOG(d, LOGGER_INFO, "stage=manifest-summary elapsed_ms=%" G_GINT64_FORMAT
         " captured_bytes=%u truncated=%d entries=%u variants=%u target_seconds=%" G_GINT64_FORMAT
         " media_sequence=%" G_GINT64_FORMAT " endlist=%d",
         elapsed_ms(d), d->manifest->len, d->manifest_truncated, entries, variants, target, sequence, endlist);
    g_strfreev(lines);
    g_free(body);
}

void playback_diagnostics_message(playback_diagnostics_t *d, GstMessage *message) {
    if (!d || !message) return;
    g_mutex_lock(&d->lock);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ELEMENT:
        network_message(d, message);
        if (d->manifest_reports == 1 && d->manifest->len) {
            manifest_summary(d);
            /* No need to keep any URI-bearing text after this summary. */
            g_byte_array_set_size(d->manifest, 0);
        }
        break;
    case GST_MESSAGE_WARNING:
    case GST_MESSAGE_ERROR:
        if (d->error_reports < MAX_ERROR_REPORTS) {
            GError *error = NULL;
            gchar *debug = NULL;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) gst_message_parse_error(message, &error, &debug);
            else gst_message_parse_warning(message, &error, &debug);
            guint http_status = 0;
#if GST_CHECK_VERSION(1, 10, 0)
            const GstStructure *details = NULL;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) gst_message_parse_error_details(message, &details);
            else gst_message_parse_warning_details(message, &details);
            if (details) gst_structure_get_uint(details, "http-status-code", &http_status);
#endif
            d->error_reports++;
            DLOG(d, LOGGER_WARNING, "stage=%s factory=%s elapsed_ms=%" G_GINT64_FORMAT
                       " domain=%s code=%d flow_reason=%s http_status=%u",
                       GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR ? "error" : "warning",
                       message_factory_name(message), elapsed_ms(d), error_domain_name(error),
                       error ? error->code : 0, error_flow_reason(debug), http_status);
            g_clear_error(&error);
            g_free(debug);
        }
        break;
    case GST_MESSAGE_BUFFERING: {
        gint percent = 0;
        gst_message_parse_buffering(message, &percent);
        percent = CLAMP(percent, 0, 100);
        gint bucket = percent / 25;
        GstObject *source = GST_MESSAGE_SRC(message);
        buffering_source_t *record = g_hash_table_lookup(d->buffering_sources, source);
        if (!record && source && g_hash_table_size(d->buffering_sources) < MAX_OBSERVED_ELEMENTS) {
            record = g_new0(buffering_source_t, 1);
            record->bucket = -1;
            record->id = g_hash_table_size(d->buffering_sources) + 1;
            g_hash_table_insert(d->buffering_sources, gst_object_ref(source), record);
        }
        if (record && bucket != record->bucket && d->buffering_reports < MAX_BUFFERING_REPORTS) {
            GstBufferingMode mode;
            gint avg_in, avg_out;
            gint64 left;
            gst_message_parse_buffering_stats(message, &mode, &avg_in, &avg_out, &left);
            d->buffering_reports++;
            DLOG(d, LOGGER_INFO, "stage=buffering percent=%d elapsed_ms=%" G_GINT64_FORMAT
                 " source=%u factory=%s mode=%s avg_in=%d avg_out=%d remaining_ms=%" G_GINT64_FORMAT,
                 percent, elapsed_ms(d), record->id, message_factory_name(message), buffer_mode_name(mode), avg_in, avg_out, left);
        }
        if (record) record->bucket = bucket;
        break;
    }
    case GST_MESSAGE_EOS:
        DLOG(d, LOGGER_INFO, "stage=eos factory=%s elapsed_ms=%" G_GINT64_FORMAT,
             message_factory_name(message), elapsed_ms(d));
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(d->pipeline)) {
            GstState old_state, new_state, pending;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending);
            if (new_state == GST_STATE_PAUSED && !d->saw_preroll) {
                d->saw_preroll = TRUE;
                DLOG(d, LOGGER_INFO, "stage=paused-ready elapsed_ms=%" G_GINT64_FORMAT
                           " (preroll complete, or live source without preroll)", elapsed_ms(d));
            }
            if (new_state == GST_STATE_PLAYING && !d->saw_playing) {
                d->saw_playing = TRUE;
                DLOG(d, LOGGER_INFO, "stage=playing elapsed_ms=%" G_GINT64_FORMAT,
                           elapsed_ms(d));
            }
        }
        break;
    default:
        break;
    }
    g_mutex_unlock(&d->lock);
}

static const gchar *intent_name(direct_playback_target_t intent) {
    return intent == DIRECT_PLAYBACK_PLAYING ? "play" : intent == DIRECT_PLAYBACK_PAUSED ? "pause" : "stop";
}

void playback_diagnostics_control(playback_diagnostics_t *d, const direct_playback_state_t *state) {
    if (!d) return;
    g_mutex_lock(&d->lock);
    if (d->control_reports++ < 32)
        DLOG(d, LOGGER_INFO, "stage=control elapsed_ms=%" G_GINT64_FORMAT " intent=%s buffering=%d preroll=%d live=%d",
             elapsed_ms(d), intent_name(state->intent), state->buffering, state->preroll_complete, state->live);
    g_mutex_unlock(&d->lock);
}

void playback_diagnostics_tick(playback_diagnostics_t *d, const direct_playback_state_t *state) {
    if (!d || state->intent != DIRECT_PLAYBACK_PLAYING) return;
    gint64 now = g_get_monotonic_time();
    g_mutex_lock(&d->lock);
    if (now < d->next_wait_us || d->wait_reports >= MAX_WAIT_REPORTS ||
        (d->saw_video_sink && !state->buffering)) {
        g_mutex_unlock(&d->lock);
        return;
    }
    d->wait_reports++;
    d->next_wait_us = now + (elapsed_ms(d) < 60000 ? 5 : 30) * G_USEC_PER_SEC;
    GstElement *adaptive = d->adaptive_demux ? gst_object_ref(d->adaptive_demux) : NULL;
    g_mutex_unlock(&d->lock);
    /* Sample properties without holding the observer lock: GStreamer streaming
     * callbacks can be delivering probe data at the same time. No URI, buffer
     * contents, or synchronous network/position queries are requested. */
    GstState current, pending;
    gst_element_get_state(d->pipeline, &current, &pending, 0);
    guint bandwidth = 0;
    guint64 audio_level = GST_CLOCK_TIME_NONE, video_level = GST_CLOCK_TIME_NONE;
    if (adaptive) {
        GObjectClass *klass = G_OBJECT_GET_CLASS(adaptive);
        if (g_object_class_find_property(klass, "current-bandwidth"))
            g_object_get(adaptive, "current-bandwidth", &bandwidth, NULL);
        if (g_object_class_find_property(klass, "current-level-time-audio"))
            g_object_get(adaptive, "current-level-time-audio", &audio_level, NULL);
        if (g_object_class_find_property(klass, "current-level-time-video"))
            g_object_get(adaptive, "current-level-time-video", &video_level, NULL);
        gst_object_unref(adaptive);
    }
    g_mutex_lock(&d->lock);
    DLOG(d, LOGGER_INFO, "stage=waiting elapsed_ms=%" G_GINT64_FORMAT
         " state=%s pending=%s intent=%s buffering=%d preroll=%d live=%d"
         " http_responses=%u manifests=%u source_bytes=%" G_GUINT64_FORMAT " media_input_bytes=%" G_GUINT64_FORMAT
         " source_idle_ms=%" G_GINT64_FORMAT " media_idle_ms=%" G_GINT64_FORMAT
         " bandwidth_bps=%u audio_buffer_ms=%" G_GINT64_FORMAT " video_buffer_ms=%" G_GINT64_FORMAT,
         elapsed_ms(d), gst_element_state_get_name(current), gst_element_state_get_name(pending),
         intent_name(state->intent), state->buffering, state->preroll_complete, state->live,
         d->http_responses, d->manifest_reports, d->source_bytes, d->media_bytes,
         d->last_source_us ? MAX((gint64)0, now - d->last_source_us) / 1000 : -1,
         d->last_media_us ? MAX((gint64)0, now - d->last_media_us) / 1000 : -1,
         bandwidth, GST_CLOCK_TIME_IS_VALID(audio_level) ? (gint64)(audio_level / GST_MSECOND) : -1,
         GST_CLOCK_TIME_IS_VALID(video_level) ? (gint64)(video_level / GST_MSECOND) : -1);
    g_mutex_unlock(&d->lock);
}

void playback_diagnostics_free(playback_diagnostics_t *d) {
    if (!d) return;
    if (d->element_added_id) g_signal_handler_disconnect(d->pipeline, d->element_added_id);
    /* NULL has stopped streaming before any observer storage is freed. */
    for (guint i = 0; i < d->probes->len; i++) {
        pad_observation_t *observation = g_ptr_array_index(d->probes, i);
        if (observation->probe_id && (observation->persistent || !observation->fired)) {
            gst_pad_remove_probe(observation->pad, observation->probe_id);
        }
        gst_object_unref(observation->pad);
        g_free(observation->factory);
        g_free(observation);
    }
    DLOG(d, LOGGER_INFO, "stage=session-ended elapsed_ms=%" G_GINT64_FORMAT
               " decoder_output_seen=%d video_sink_buffer_seen=%d"
               " audio_decoder_input_seen=%d audio_decoder_output_seen=%d audio_sink_buffer_seen=%d",
               elapsed_ms(d), d->saw_decoder_output, d->saw_video_sink,
               d->saw_audio_decoder_input, d->saw_audio_decoder_output, d->saw_audio_sink);
    g_ptr_array_free(d->probes, TRUE);
    g_byte_array_unref(d->manifest);
    g_hash_table_destroy(d->elements);
    g_hash_table_destroy(d->buffering_sources);
    if (d->adaptive_demux) gst_object_unref(d->adaptive_demux);
    gst_object_unref(d->pipeline);
    g_mutex_clear(&d->lock);
    g_free(d);
}
