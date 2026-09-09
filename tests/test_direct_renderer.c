/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Include the production renderer so synthetic bus messages exercise the real
 * control path, without exporting test-only entry points or owning a display. */
#include "../renderers/video_renderer.c"
#include <glib/gstdio.h>

static logger_t *test_logger;
static char *fixture_uri;

static void capture_log(void *cls, int level, const char *text) {
    (void)cls;
    (void)level;
    g_print("%s\n", text);
}

static void check_message(GstMessage *message) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = NULL;
        gst_message_parse_error(message, &error, NULL);
        g_error("Unexpected pipeline error: %s", error->message);
    }
}

static void wait_state(GstState target) {
    gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    do {
        GstMessage *message = gst_bus_timed_pop(renderer->bus, 10 * GST_MSECOND);
        if (message) {
            check_message(message);
            gstreamer_video_pipeline_bus_callback(renderer->bus, message, NULL);
            gst_message_unref(message);
        }
        GstState state, pending;
        gst_element_get_state(renderer->pipeline, &state, &pending, 0);
        if (state == target && pending == GST_STATE_VOID_PENDING) return;
    } while (g_get_monotonic_time() < deadline);
    g_error("Timed out waiting for %s", gst_element_state_get_name(target));
}

static void buffering(int percent) {
    GstMessage *message = gst_message_new_buffering(GST_OBJECT(renderer->pipeline), percent);
    gstreamer_video_pipeline_bus_callback(renderer->bus, message, NULL);
    gst_message_unref(message);
}

static void init_renderer(void) {
    videoflip_t flips[2] = {NONE, NONE};
    video_renderer_init(test_logger, "Headless test", flips, "h264parse", "", "avdec_h264",
                        "videoconvert", "fakesink", "", false, false, false, false, 3, fixture_uri);
}

static void test_controls(void) {
    logger = test_logger;
    video_renderer_set_start(0);
    init_renderer();
    /* A pause between construction and URI installation must not start playbin. */
    video_renderer_pause();
    GstState state;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    g_assert_cmpint(state, ==, GST_STATE_READY);
    video_renderer_start();
    wait_state(GST_STATE_PAUSED);
    buffering(100);
    wait_state(GST_STATE_PAUSED);
    video_renderer_seek(1);
    wait_state(GST_STATE_PAUSED);
    video_renderer_resume();
    wait_state(GST_STATE_PLAYING);
    buffering(0);
    wait_state(GST_STATE_PAUSED);
    video_renderer_resume();
    wait_state(GST_STATE_PAUSED);
    buffering(100);
    wait_state(GST_STATE_PLAYING);
    video_renderer_hls_ready();
    buffering(100);
    wait_state(GST_STATE_READY);
    video_renderer_destroy();
    g_assert_null(renderer);
    g_assert_null(renderer_type[0]);
}

static void test_replacement_and_early_stop(void) {
    video_renderer_set_start(0);
    init_renderer();
    video_renderer_start();
    /* Local sources need no BUFFERING event to begin playback. */
    wait_state(GST_STATE_PLAYING);
    renderer->eos = TRUE;
    video_renderer_set_start(0);
    g_assert_false(video_renderer_eos_watch());
    video_renderer_pause();
    buffering(100); /* Old pipeline messages must not affect the next request. */
    video_renderer_destroy();
    init_renderer();
    video_renderer_start();
    wait_state(GST_STATE_PAUSED);
    g_assert_cmpint(direct_state.intent, ==, DIRECT_PLAYBACK_PAUSED);
    video_renderer_destroy();

    hls_video = false; /* A direct request can arrive while the mirror is idle. */
    video_renderer_set_start(0);
    video_renderer_hls_ready();
    init_renderer();
    video_renderer_start();
    wait_state(GST_STATE_READY);
    g_assert_false(renderer->direct_started);
    video_renderer_destroy();
}

static void test_unknown_duration(void) {
    GError *error = NULL;
    video_renderer_set_start(0);
    direct_request_pending = FALSE;
    hls_video = true;
    hls_duration = -1;
    hls_seek_enabled = FALSE;
    n_renderers = 1;
    renderer = calloc(1, sizeof(*renderer));
    renderer_type[0] = renderer;
    renderer->codec = hls;
    renderer->direct_generation = direct_generation;
    renderer->direct_started = TRUE;
    renderer->pipeline = gst_parse_launch("videotestsrc is-live=true ! fakesink sync=true", &error);
    g_assert_no_error(error);
    renderer->bus = gst_element_get_bus(renderer->pipeline);
    direct_state.live = true;
    direct_apply_state();
    wait_state(GST_STATE_PLAYING);
    double duration, position, seek_start, seek_duration;
    float rate;
    bool empty, full;
    g_usleep(50000);
    video_get_playback_info(&duration, &position, &seek_start, &seek_duration, &rate, &empty, &full);
    g_assert_cmpfloat(duration, ==, 0);
    g_assert_cmpfloat(position, >=, 0);
    g_assert_cmpfloat(rate, ==, 1);
    buffering(0);
    wait_state(GST_STATE_PLAYING);
    video_renderer_destroy();
    empty = false;
    full = true;
    video_get_playback_info(&duration, &position, &seek_start, &seek_duration, &rate, &empty, &full);
    g_assert_true(empty);
    g_assert_false(full);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    test_logger = logger_init();
    logger_set_callback(test_logger, capture_log, NULL);
    logger_set_level(test_logger, LOGGER_INFO);
    GError *error = NULL;
    char *directory = g_dir_make_tmp("uxplay-renderer-XXXXXX", &error);
    g_assert_no_error(error);
    char *path = g_build_filename(directory, "fixture.webm", NULL);
    fixture_uri = g_filename_to_uri(path, NULL, &error);
    g_assert_no_error(error);
    GstElement *fixture = gst_parse_launch(
        "videotestsrc num-buffers=300 ! video/x-raw,width=64,height=64,framerate=30/1 "
        "! vp8enc deadline=1 ! webmmux ! filesink name=output", &error);
    g_assert_no_error(error);
    GstElement *output = gst_bin_get_by_name(GST_BIN(fixture), "output");
    g_object_set(output, "location", path, NULL);
    gst_object_unref(output);
    GstBus *bus = gst_element_get_bus(fixture);
    gst_element_set_state(fixture, GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    g_assert_nonnull(message);
    check_message(message);
    gst_message_unref(message);
    gst_element_set_state(fixture, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(fixture);
    test_controls();
    test_replacement_and_early_stop();
    test_unknown_duration();
    g_remove(path);
    g_rmdir(directory);
    g_free(path);
    g_free(directory);
    g_free(fixture_uri);
    logger_destroy(test_logger);
    gst_deinit();
    return 0;
}
