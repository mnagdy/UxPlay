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
static guint input_count, output_count, sink_count, buffering_count;
static gboolean seen_system_memory;
static guint error_count;
static gboolean seen_not_negotiated, seen_not_linked, seen_missing_debug, seen_custom_error;
static void log_callback(void *opaque, int level, const char *line) {
    (void)opaque; (void)level;
    g_assert_null(strstr(line, "SECRET_NOT_TO_LOG"));
    if (strstr(line, "stage=decoder-input")) input_count++;
    if (strstr(line, "stage=decoder-output")) {
        output_count++;
        if (strstr(line, "buffer_memory=SystemMemory")) seen_system_memory = TRUE;
    }
    if (strstr(line, "stage=first-video-sink-buffer")) sink_count++;
    if (strstr(line, "stage=buffering")) buffering_count++;
    if (strstr(line, "stage=error ")) {
        error_count++;
        g_assert_nonnull(strstr(line, "elapsed_ms="));
        g_assert_nonnull(strstr(line, "factory=identity"));
        if (strstr(line, "domain=stream code=1 flow_reason=not-negotiated")) seen_not_negotiated = TRUE;
        if (strstr(line, "flow_reason=not-linked")) seen_not_linked = TRUE;
        if (strstr(line, "domain=resource") && strstr(line, "flow_reason=unavailable")) seen_missing_debug = TRUE;
        if (strstr(line, "domain=other code=77 flow_reason=unclassified")) seen_custom_error = TRUE;
    }
    g_print("%s\n", line);
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
    playback_diagnostics_t *d = playback_diagnostics_attach(pipeline, logger, 0);
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
    if (dynamic) d = playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time());
    GstElement *contents = gst_parse_bin_from_description("videotestsrc num-buffers=10 ! vp8enc deadline=1 ! video/x-vp8,private-data=(string)SECRET_NOT_TO_LOG ! vp8dec ! diagnosticvideosink", FALSE, &error);
    g_assert_no_error(error);
    gst_bin_add(GST_BIN(pipeline), contents);
    if (!dynamic) d = playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time());
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
static void run_playbin_session(logger_t *logger, const gchar *uri) {
    GError *error = NULL;
    GstElement *pipeline = gst_element_factory_make("playbin3", "SECRET_NOT_TO_LOG");
    GstElement *sink = gst_element_factory_make("diagnosticvideosink", NULL);
    g_object_set(pipeline, "uri", uri, "video-sink", sink, "flags", 1, NULL);
    playback_diagnostics_t *d = playback_diagnostics_attach(pipeline, logger, g_get_monotonic_time());
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
    logger_t *logger = logger_init();
    logger_set_callback(logger, log_callback, NULL);
    logger_set_level(logger, LOGGER_INFO);
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
    for (guint i = 0; i < 10; i++) run_playbin_session(logger, uri);
    g_assert_cmpint(g_unlink(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_free(uri);
    g_free(filename);
    g_free(directory);
    /* Free observers without ever starting a stream, including pending probes. */
    GstElement *pipeline = gst_parse_launch("videotestsrc ! vp8enc deadline=1 ! vp8dec ! diagnosticvideosink", NULL);
    playback_diagnostics_t *d = playback_diagnostics_attach(pipeline, logger, 0);
    playback_diagnostics_free(d);
    gst_object_unref(pipeline);
    g_assert_cmpuint(input_count, ==, 20);
    g_assert_cmpuint(output_count, ==, 20);
    g_assert_cmpuint(sink_count, ==, 20);
    g_assert_cmpuint(buffering_count, ==, 50);
    g_assert_true(seen_system_memory);
    logger_destroy(logger);
    gst_deinit();
    return 0;
}
