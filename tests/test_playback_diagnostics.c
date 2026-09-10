/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
#include <glib/gstdio.h>
#include <string.h>
#include "../renderers/playback_diagnostics.h"

typedef struct { GstBaseSink parent; } DiagnosticVideoSink;
typedef struct { GstBaseSinkClass parent; } DiagnosticVideoSinkClass;
G_DEFINE_TYPE(DiagnosticVideoSink, diagnostic_video_sink, GST_TYPE_BASE_SINK)
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw"));
static GstFlowReturn render(GstBaseSink *sink, GstBuffer *buffer) { (void)sink; (void)buffer; return GST_FLOW_OK; }
static void diagnostic_video_sink_class_init(DiagnosticVideoSinkClass *klass) {
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element_class, "Diagnostic test video sink", "Sink/Video", "Headless sink for diagnostics tests", "UxPlay tests");
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    GST_BASE_SINK_CLASS(klass)->render = render;
}
static void diagnostic_video_sink_init(DiagnosticVideoSink *sink) { gst_base_sink_set_sync(GST_BASE_SINK(sink), FALSE); }
typedef struct { GstBaseSink parent; } DiagnosticAudioSink;
typedef struct { GstBaseSinkClass parent; } DiagnosticAudioSinkClass;
G_DEFINE_TYPE(DiagnosticAudioSink, diagnostic_audio_sink, GST_TYPE_BASE_SINK)
static GstStaticPadTemplate audio_sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("audio/x-raw"));
static void diagnostic_audio_sink_class_init(DiagnosticAudioSinkClass *klass) {
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element_class, "Diagnostic test audio sink", "Sink/Audio", "Headless sink for audio observations", "UxPlay tests");
    gst_element_class_add_static_pad_template(element_class, &audio_sink_template);
    GST_BASE_SINK_CLASS(klass)->render = render;
}
static void diagnostic_audio_sink_init(DiagnosticAudioSink *sink) {
    gst_base_sink_set_sync(GST_BASE_SINK(sink), FALSE);
    gst_base_sink_set_async_enabled(GST_BASE_SINK(sink), FALSE);
}

/* Preserve buffer lists through a classified audio decoder to test the real
 * observation pads with HEADER/GAP/empty buffers preceding actual samples.
 * Real decoding is covered separately by Vorbis below. */
typedef struct { GstElement parent; GstPad *src; } DiagnosticAudioDecoder;
typedef struct { GstElementClass parent; } DiagnosticAudioDecoderClass;
G_DEFINE_TYPE(DiagnosticAudioDecoder, diagnostic_audio_decoder, GST_TYPE_ELEMENT)
static GstStaticPadTemplate audio_src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("audio/x-raw"));
static GstFlowReturn audio_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer) {
    (void)pad;
    return gst_pad_push(((DiagnosticAudioDecoder *)parent)->src, buffer);
}
static GstFlowReturn audio_chain_list(GstPad *pad, GstObject *parent, GstBufferList *list) {
    (void)pad;
    return gst_pad_push_list(((DiagnosticAudioDecoder *)parent)->src, list);
}
static gboolean audio_event(GstPad *pad, GstObject *parent, GstEvent *event) {
    (void)pad;
    return gst_pad_push_event(((DiagnosticAudioDecoder *)parent)->src, event);
}
static void diagnostic_audio_decoder_class_init(DiagnosticAudioDecoderClass *klass) {
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element_class, "Diagnostic audio decoder", "Codec/Decoder/Audio", "Buffer-list transport for observation tests", "UxPlay tests");
    gst_element_class_add_static_pad_template(element_class, &audio_sink_template);
    gst_element_class_add_static_pad_template(element_class, &audio_src_template);
}
static void diagnostic_audio_decoder_init(DiagnosticAudioDecoder *decoder) {
    GstPad *sink = gst_pad_new_from_static_template(&audio_sink_template, "sink");
    decoder->src = gst_pad_new_from_static_template(&audio_src_template, "src");
    gst_pad_set_chain_function(sink, audio_chain);
    gst_pad_set_chain_list_function(sink, audio_chain_list);
    gst_pad_set_event_function(sink, audio_event);
    gst_element_add_pad(GST_ELEMENT(decoder), sink);
    gst_element_add_pad(GST_ELEMENT(decoder), decoder->src);
}
static guint input_count, output_count, sink_count, buffering_count;
static guint audio_input_count, audio_output_count, audio_sink_count;
static gboolean seen_vorbis, seen_audio_format;
static gint expected_audio_summary = -1;
static gboolean seen_system_memory;
static guint error_count;
static gboolean seen_not_negotiated, seen_not_linked, seen_missing_debug, seen_custom_error;
static guint http_count, wait_count, manifest_count;
static gboolean seen_http_404, seen_private_type;
static gboolean network_run;
static void log_callback(void *opaque, int level, const char *line) {
    (void)opaque; (void)level;
    g_assert_null(strstr(line, "SECRET_NOT_TO_LOG"));
    if (strstr(line, "stage=http-response")) {
        http_count++;
        if (strstr(line, "status=404")) seen_http_404 = TRUE;
        if (strstr(line, "content_type=other") && strstr(line, "content_length=-1")) seen_private_type = TRUE;
    }
    if (strstr(line, "stage=waiting")) {
        wait_count++;
        g_assert_nonnull(strstr(line, "session=42"));
        g_assert_nonnull(strstr(line, "pending="));
        g_assert_nonnull(strstr(line, "media_input_bytes="));
    }
    if (strstr(line, "stage=manifest-processed")) manifest_count++;
    if (strstr(line, "stage=decoder-input")) input_count++;
    if (strstr(line, "stage=decoder-output")) {
        output_count++;
        if (strstr(line, "buffer_memory=SystemMemory")) seen_system_memory = TRUE;
    }
    if (strstr(line, "stage=first-video-sink-buffer")) sink_count++;
    if (strstr(line, "stage=audio-decoder-input")) {
        audio_input_count++;
        g_assert_nonnull(strstr(line, "codec="));
        if (strstr(line, "codec=Vorbis")) seen_vorbis = TRUE;
    }
    if (strstr(line, "stage=audio-decoder-output")) {
        audio_output_count++;
        g_assert_nonnull(strstr(line, "rate=48000"));
        g_assert_nonnull(strstr(line, "channels=2"));
        if (strstr(line, "format=F32LE") || strstr(line, "format=S16LE")) seen_audio_format = TRUE;
    }
    if (strstr(line, "stage=first-audio-sink-buffer")) {
        audio_sink_count++;
        g_assert_nonnull(strstr(line, "arrival, not physical audibility"));
    }
    if (strstr(line, "stage=session-ended") && expected_audio_summary >= 0) {
        const gchar *fields[] = {"audio_decoder_input_seen", "audio_decoder_output_seen", "audio_sink_buffer_seen"};
        for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
            gchar *expected = g_strdup_printf("%s=%d", fields[i], expected_audio_summary);
            g_assert_nonnull(strstr(line, expected));
            g_free(expected);
        }
    }
    if (strstr(line, "stage=buffering")) buffering_count++;
    if (strstr(line, "stage=error ")) {
        error_count++;
        g_assert_nonnull(strstr(line, "elapsed_ms="));
        if (!network_run) g_assert_nonnull(strstr(line, "factory=identity"));
        if (strstr(line, "domain=stream code=1 flow_reason=not-negotiated")) seen_not_negotiated = TRUE;
        if (strstr(line, "flow_reason=not-linked")) seen_not_linked = TRUE;
        if (strstr(line, "domain=resource") && strstr(line, "flow_reason=unavailable")) seen_missing_debug = TRUE;
        if (strstr(line, "domain=other code=77 flow_reason=unclassified")) seen_custom_error = TRUE;
    }
    g_print("%s\n", line);
}

static void test_network_and_silent_wait(logger_t *logger) {
    GstElement *pipeline = gst_pipeline_new("SECRET_NOT_TO_LOG");
    GstElement *source = gst_element_factory_make("identity", "SECRET_NOT_TO_LOG");
    gst_bin_add(GST_BIN(pipeline), source);
    /* Backdate only the test clock origin to exercise silent waiting without
     * requiring a real timeout or any bus messages to trigger the snapshot. */
    playback_diagnostics_t *d = playback_diagnostics_attach(pipeline, logger,
        g_get_monotonic_time() - 6 * G_USEC_PER_SEC, 42);
    direct_playback_state_t state;
    direct_playback_state_reset(&state);
    state.buffering = TRUE;
    playback_diagnostics_tick(d, &state);
    playback_diagnostics_tick(d, &state);
    g_assert_cmpuint(wait_count, ==, 1);
    GstState current;
    gst_element_get_state(pipeline, &current, NULL, 0);
    g_assert_cmpint(current, ==, GST_STATE_NULL); /* Logging did not start playback. */
    for (guint i = 0; i < 80; i++) {
        GstStructure *headers = gst_structure_new("response-headers",
            "Content-Type", G_TYPE_STRING, "SECRET_NOT_TO_LOG", "Content-Length", G_TYPE_STRING, "SECRET_NOT_TO_LOG",
            "Set-Cookie", G_TYPE_STRING, "SECRET_NOT_TO_LOG", NULL);
        GstStructure *s = gst_structure_new("http-headers", "http-status-code", G_TYPE_UINT, 404,
            "uri", G_TYPE_STRING, "https://SECRET_NOT_TO_LOG.invalid/?token=SECRET_NOT_TO_LOG",
            "response-headers", GST_TYPE_STRUCTURE, headers, NULL);
        gst_structure_free(headers);
        GstMessage *message = gst_message_new_element(GST_OBJECT(source), s);
        playback_diagnostics_message(d, message);
        gst_message_unref(message);
    }
    g_assert_cmpuint(http_count, ==, 32);
    g_assert_true(seen_http_404);
    g_assert_true(seen_private_type);
    playback_diagnostics_free(d);
    /* Per-session limits and numbering start fresh on the next pipeline. */
    d = playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time() - 6 * G_USEC_PER_SEC, 43);
    GstMessage *message = gst_message_new_element(GST_OBJECT(source), gst_structure_new("adaptive-streaming-statistics",
        "manifest-uri", G_TYPE_STRING, "SECRET_NOT_TO_LOG", "manifest-download-stop", G_TYPE_UINT64, (guint64)100, NULL));
    playback_diagnostics_message(d, message);
    gst_message_unref(message);
    g_assert_cmpuint(manifest_count, ==, 1);
    state.intent = DIRECT_PLAYBACK_STOPPED;
    playback_diagnostics_tick(d, &state);
    g_assert_cmpuint(wait_count, ==, 1);
    playback_diagnostics_free(d);
    gst_object_unref(pipeline);
}

static void emit_test_error(playback_diagnostics_t *d, GstElement *source,
                            GQuark domain, gint code, const gchar *debug) {
    GError *error = g_error_new_literal(domain, code,
        "SECRET_NOT_TO_LOG error for https://SECRET_NOT_TO_LOG.invalid/video?token=SECRET_NOT_TO_LOG");
    GstMessage *message = gst_message_new_error(GST_OBJECT(source), error, debug);
    g_error_free(error);
    playback_diagnostics_message(d, message);
    gst_message_unref(message);
}

static void test_error_details_without_private_data(logger_t *logger) {
    GstElement *pipeline = gst_pipeline_new("SECRET_NOT_TO_LOG");
    GstElement *source = gst_element_factory_make("identity", "SECRET_NOT_TO_LOG");
    g_assert_nonnull(source);
    gst_bin_add(GST_BIN(pipeline), source);
    playback_diagnostics_t *d = playback_diagnostics_attach(pipeline, logger, 0, 42);
    emit_test_error(d, source, GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
        "SECRET_NOT_TO_LOG.c(99): streaming stopped, reason not-negotiated (-4); "
        "https://SECRET_NOT_TO_LOG.invalid/?token=SECRET_NOT_TO_LOG");
    emit_test_error(d, source, GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
        "SECRET_NOT_TO_LOG.c(99): streaming stopped, reason not-linked (-1)");
    emit_test_error(d, source, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND, NULL);
    emit_test_error(d, source, g_quark_from_static_string("SECRET_NOT_TO_LOG"), 77,
        "SECRET_NOT_TO_LOG custom detail");
    g_assert_true(seen_not_negotiated);
    g_assert_true(seen_not_linked);
    g_assert_true(seen_missing_debug);
    g_assert_true(seen_custom_error);
    for (guint i = 0; i < 20; i++) {
        emit_test_error(d, source, GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
            "streaming stopped, reason not-negotiated (-4)");
    }
    /* Repeated failures must not flood the receiver log. */
    g_assert_cmpuint(error_count, ==, 8);
    playback_diagnostics_free(d);
    gst_object_unref(pipeline);
}
static void run_session(logger_t *logger, gboolean dynamic) {
    GError *error = NULL;
    GstElement *pipeline = gst_pipeline_new("SECRET_NOT_TO_LOG");
    playback_diagnostics_t *d = NULL;
    if (dynamic) d = playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time(), 42);
    GstElement *contents = gst_parse_bin_from_description("videotestsrc num-buffers=10 ! vp8enc deadline=1 ! video/x-vp8,private-data=(string)SECRET_NOT_TO_LOG ! vp8dec ! diagnosticvideosink", FALSE, &error);
    g_assert_no_error(error);
    gst_bin_add(GST_BIN(pipeline), contents);
    if (!dynamic) d = playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time(), 42);
    const gint percents[] = { 0, 1, 25, 25, 26, 50, 75, 100, 0, 100 };
    for (guint i = 0; i < G_N_ELEMENTS(percents); i++) {
        GstMessage *message = gst_message_new_buffering(GST_OBJECT(pipeline), percents[i]);
        playback_diagnostics_message(d, message);
        gst_message_unref(message);
    }
    GstBus *bus = gst_element_get_bus(pipeline);
    g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
    gboolean finished = FALSE;
    while (!finished) {
        GstMessage *message = gst_bus_timed_pop(bus, 5 * GST_SECOND);
        g_assert_nonnull(message);
        playback_diagnostics_message(d, message);
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            gchar *debug = NULL;
            gst_message_parse_error(message, &error, &debug);
            g_error("pipeline error: %s (%s)", error->message, debug);
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) finished = TRUE;
        gst_message_unref(message);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_assert_cmpint(gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE), ==, GST_STATE_CHANGE_SUCCESS);
    playback_diagnostics_free(d);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
}
static void run_playbin_session(logger_t *logger, const gchar *uri, gboolean expect_error) {
    GstElement *pipeline = gst_element_factory_make("playbin3", "SECRET_NOT_TO_LOG");
    GstElement *sink = gst_element_factory_make("diagnosticvideosink", NULL);
    g_object_set(pipeline, "uri", uri, "video-sink", sink, "flags", 1, NULL);
    playback_diagnostics_t *d = playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time(), 42);
    GstBus *bus = gst_element_get_bus(pipeline);
    g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
    gboolean finished = FALSE;
    gboolean failed = FALSE;
    gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    direct_playback_state_t state;
    direct_playback_state_reset(&state);
    while (!finished) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        playback_diagnostics_tick(d, &state);
        GstMessage *message = gst_bus_timed_pop(bus, 100 * GST_MSECOND);
        if (!message) continue;
        playback_diagnostics_message(d, message);
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            failed = TRUE;
            finished = TRUE;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) finished = TRUE;
        gst_message_unref(message);
    }
    g_assert_cmpint(failed, ==, expect_error);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_assert_cmpint(gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE), ==, GST_STATE_CHANGE_SUCCESS);
    playback_diagnostics_free(d);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
}
static void run_audio_session(logger_t *logger, gboolean dynamic) {
    GError *error = NULL;
    GstElement *pipeline = gst_pipeline_new("SECRET_NOT_TO_LOG");
    guint before_input = audio_input_count, before_output = audio_output_count, before_sink = audio_sink_count;
    playback_diagnostics_t *d = dynamic ? playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time(), 42) : NULL;
    GstElement *contents = gst_parse_bin_from_description(
        "audiotestsrc num-buffers=20 samplesperbuffer=480 ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2 ! vorbisenc ! "
        "audio/x-vorbis,private-data=(string)SECRET_NOT_TO_LOG ! vorbisdec ! diagnosticaudiosink",
        FALSE, &error);
    g_assert_no_error(error);
    g_assert_nonnull(contents);
    gst_bin_add(GST_BIN(pipeline), contents);
    if (!dynamic) d = playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time(), 42);
    GstBus *bus = gst_element_get_bus(pipeline);
    g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    g_assert_nonnull(message);
    g_assert_cmpint(GST_MESSAGE_TYPE(message), ==, GST_MESSAGE_EOS);
    gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_assert_cmpint(gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE), ==, GST_STATE_CHANGE_SUCCESS);
    /* Many media buffers produce exactly one observation at each audio stage;
     * the next independently attached session must produce its own markers. */
    g_assert_cmpuint(audio_input_count - before_input, ==, 1);
    g_assert_cmpuint(audio_output_count - before_output, ==, 1);
    g_assert_cmpuint(audio_sink_count - before_sink, ==, 1);
    expected_audio_summary = 1;
    playback_diagnostics_free(d);
    expected_audio_summary = -1;
    gst_object_unref(bus);
    gst_object_unref(pipeline);
}

static GstBuffer *audio_test_buffer(gsize size, GstBufferFlags flags) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
    GST_BUFFER_PTS(buffer) = 0;
    GST_BUFFER_DURATION(buffer) = GST_MSECOND;
    GST_BUFFER_FLAG_SET(buffer, flags);
    return buffer;
}

static GstBufferList *audio_test_list(gboolean media) {
    GstBufferList *list = gst_buffer_list_new();
    gst_buffer_list_add(list, audio_test_buffer(16, GST_BUFFER_FLAG_GAP));
    gst_buffer_list_add(list, audio_test_buffer(16, GST_BUFFER_FLAG_HEADER));
    gst_buffer_list_add(list, audio_test_buffer(0, 0));
    if (media) gst_buffer_list_add(list, audio_test_buffer(16, 0));
    return list;
}

static void test_audio_nonmedia_and_bufferlists(logger_t *logger, gboolean media) {
    GstElement *pipeline = gst_pipeline_new("SECRET_NOT_TO_LOG");
    GstElement *decoder = gst_element_factory_make("diagnosticaudiodecoder", "SECRET_NOT_TO_LOG");
    GstElement *sink = gst_element_factory_make("diagnosticaudiosink", NULL);
    g_assert_nonnull(decoder);
    g_assert_nonnull(sink);
    gst_bin_add_many(GST_BIN(pipeline), decoder, sink, NULL);
    g_assert_true(gst_element_link(decoder, sink));
    GstPad *sender = gst_pad_new_from_static_template(&audio_src_template, "src");
    GstPad *input = gst_element_get_static_pad(decoder, "sink");
    g_assert_cmpint(gst_pad_link(sender, input), ==, GST_PAD_LINK_OK);
    gst_object_unref(input);
    g_assert_true(gst_pad_set_active(sender, TRUE));
    playback_diagnostics_t *d = playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time(), 42);
    guint before_input = audio_input_count, before_output = audio_output_count, before_sink = audio_sink_count;
    g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
    g_assert_true(gst_pad_push_event(sender, gst_event_new_stream_start("SECRET_NOT_TO_LOG")));
    GstCaps *caps = gst_caps_from_string("audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved,private-data=(string)SECRET_NOT_TO_LOG");
    g_assert_true(gst_pad_push_event(sender, gst_event_new_caps(caps)));
    gst_caps_unref(caps);
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    g_assert_true(gst_pad_push_event(sender, gst_event_new_segment(&segment)));
    g_assert_true(gst_pad_push_event(sender, gst_event_new_tag(gst_tag_list_new(GST_TAG_COMMENT, "SECRET_NOT_TO_LOG", NULL))));
    g_assert_cmpint(gst_pad_push(sender, audio_test_buffer(16, GST_BUFFER_FLAG_HEADER)), ==, GST_FLOW_OK);
    g_assert_cmpint(gst_pad_push(sender, audio_test_buffer(16, GST_BUFFER_FLAG_GAP)), ==, GST_FLOW_OK);
    g_assert_cmpint(gst_pad_push(sender, audio_test_buffer(0, 0)), ==, GST_FLOW_OK);
    g_assert_cmpint(gst_pad_push_list(sender, audio_test_list(FALSE)), ==, GST_FLOW_OK);
    g_assert_cmpuint(audio_input_count, ==, before_input);
    g_assert_cmpuint(audio_output_count, ==, before_output);
    g_assert_cmpuint(audio_sink_count, ==, before_sink);
    if (media) {
        g_assert_cmpint(gst_pad_push_list(sender, audio_test_list(TRUE)), ==, GST_FLOW_OK);
        g_assert_cmpint(gst_pad_push_list(sender, audio_test_list(TRUE)), ==, GST_FLOW_OK);
        g_assert_cmpint(gst_pad_push(sender, audio_test_buffer(16, 0)), ==, GST_FLOW_OK);
    }
    g_assert_cmpuint(audio_input_count - before_input, ==, media ? 1 : 0);
    g_assert_cmpuint(audio_output_count - before_output, ==, media ? 1 : 0);
    g_assert_cmpuint(audio_sink_count - before_sink, ==, media ? 1 : 0);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_assert_cmpint(gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE), ==, GST_STATE_CHANGE_SUCCESS);
    expected_audio_summary = media ? 1 : 0;
    playback_diagnostics_free(d);
    expected_audio_summary = -1;
    gst_pad_set_active(sender, FALSE);
    gst_object_unref(sender);
    gst_object_unref(pipeline);
}
static void create_fixture(const gchar *filename) {
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch("videotestsrc num-buffers=10 ! vp8enc deadline=1 ! webmmux ! filesink name=fixture", &error);
    g_assert_no_error(error);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "fixture");
    g_assert_nonnull(sink);
    g_object_set(sink, "location", filename, NULL);
    gst_object_unref(sink);
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    g_assert_nonnull(message);
    g_assert_cmpint(GST_MESSAGE_TYPE(message), ==, GST_MESSAGE_EOS);
    gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    g_assert_true(gst_element_register(NULL, "diagnosticvideosink", GST_RANK_NONE, diagnostic_video_sink_get_type()));
    g_assert_true(gst_element_register(NULL, "diagnosticaudiosink", GST_RANK_NONE, diagnostic_audio_sink_get_type()));
    g_assert_true(gst_element_register(NULL, "diagnosticaudiodecoder", GST_RANK_NONE, diagnostic_audio_decoder_get_type()));
    logger_t *logger = logger_init();
    logger_set_callback(logger, log_callback, NULL);
    logger_set_level(logger, LOGGER_INFO);
    if (argc == 3 && !strcmp(argv[1], "--fixture")) {
        create_fixture(argv[2]);
        logger_destroy(logger);
        gst_deinit();
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--http")) {
        network_run = TRUE;
        gboolean expect_error = !strcmp(argv[3], "error");
        run_playbin_session(logger, argv[2], expect_error);
        g_assert_cmpuint(http_count, >, 0);
        if (expect_error) g_assert_true(seen_http_404);
        if (!strcmp(argv[3], "slow")) g_assert_cmpuint(wait_count, >, 0);
        logger_destroy(logger);
        gst_deinit();
        return 0;
    }
    test_network_and_silent_wait(logger);
    test_error_details_without_private_data(logger);
    for (guint i = 0; i < 10; i++) run_session(logger, i % 2);
    GError *error = NULL;
    gchar *directory = g_dir_make_tmp("uxplay-diagnostics-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(directory);
    gchar *filename = g_build_filename(directory, "SECRET_NOT_TO_LOG.webm", NULL);
    gchar *uri = g_filename_to_uri(filename, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(uri);
    create_fixture(filename);
    for (guint i = 0; i < 10; i++) run_playbin_session(logger, uri, FALSE);
    g_assert_cmpint(g_unlink(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_free(uri);
    g_free(filename);
    g_free(directory);
    /* Free observers without ever starting a stream, including pending probes. */
    GstElement *pipeline = gst_parse_launch("videotestsrc ! vp8enc deadline=1 ! vp8dec ! diagnosticvideosink", NULL);
    playback_diagnostics_t *d = playback_diagnostics_attach(pipeline, logger, 0, 42);
    playback_diagnostics_free(d);
    gst_object_unref(pipeline);
    g_assert_cmpuint(input_count, ==, 20);
    g_assert_cmpuint(output_count, ==, 20);
    g_assert_cmpuint(sink_count, ==, 20);
    g_assert_cmpuint(buffering_count, ==, 70);
    g_assert_true(seen_system_memory);
    test_audio_nonmedia_and_bufferlists(logger, FALSE);
    test_audio_nonmedia_and_bufferlists(logger, TRUE);
    run_audio_session(logger, FALSE);
    run_audio_session(logger, TRUE);
    g_assert_cmpuint(audio_input_count, ==, 3);
    g_assert_cmpuint(audio_output_count, ==, 3);
    g_assert_cmpuint(audio_sink_count, ==, 3);
    g_assert_true(seen_vorbis);
    g_assert_true(seen_audio_format);
    logger_destroy(logger);
    gst_deinit();
    return 0;
}
