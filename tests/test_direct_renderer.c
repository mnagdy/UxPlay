/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Include the production renderer so synthetic bus messages exercise the real
 * control path, without exporting test-only entry points or owning a display. */
#include "../renderers/video_renderer.c"
#include <glib/gstdio.h>

static logger_t *test_logger;
static char *fixture_uri;
static guint failed_reports;
static guint waiting_reports;

static void test_pi4_decoder_selection(void) {
    const char *preserved[] = {"v4l2h264dec", "avdec_h264", "avdec_h265", "vp8dec"};
    GstElementFactory *factories[G_N_ELEMENTS(preserved)] = {0};
    guint ranks[G_N_ELEMENTS(preserved)] = {0};
    for (guint i = 0; i < G_N_ELEMENTS(preserved); i++) {
        factories[i] = gst_element_factory_find(preserved[i]);
        if (factories[i]) ranks[i] = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factories[i]));
    }
    video_renderer_configure_pi4(test_logger);
    GstElementFactory *hevc = gst_element_factory_find("v4l2slh265dec");
    if (hevc) {
        g_assert_cmpuint(gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(hevc)), ==, GST_RANK_NONE);
        gst_object_unref(hevc);
    }
    for (guint i = 0; i < G_N_ELEMENTS(preserved); i++) {
        if (!factories[i]) continue;
        g_assert_cmpuint(gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factories[i])), ==, ranks[i]);
        gst_object_unref(factories[i]);
    }
}

static void capture_log(void *cls, int level, const char *text) {
    (void)cls;
    (void)level;
    g_assert_null(strstr(text, "PRIVATE_FAILURE_FIXTURE"));
    if (strstr(text, "stage=failed ")) failed_reports++;
    if (strstr(text, "stage=waiting ")) waiting_reports++;
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

static void inject_error(GstBus *bus, GstElement *pipeline) {
    GError *error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_TYPE_NOT_FOUND,
                                       "PRIVATE_FAILURE_FIXTURE");
    GstMessage *message = gst_message_new_error(GST_OBJECT(pipeline), error,
                                               "https://PRIVATE_FAILURE_FIXTURE.invalid/");
    gstreamer_video_pipeline_bus_callback(bus, message, NULL);
    gst_message_unref(message);
    g_error_free(error);
}

static void assert_failed_info(void) {
    double duration = -1, position = -1, seek_start = -1, seek_duration = -1;
    float rate = -1;
    bool empty = false, full = true, ready = true, likely = true;
    /* A valid nonplaying response avoids the caller's broad session teardown. */
    g_assert_true(video_get_playback_info_with_readiness(&duration, &position, &seek_start, &seek_duration,
                                                        &rate, &empty, &full, &ready, &likely));
    g_assert_cmpfloat(duration, ==, 0);
    g_assert_cmpfloat(position, ==, 0);
    g_assert_cmpfloat(seek_start, ==, 0);
    g_assert_cmpfloat(seek_duration, ==, 0);
    g_assert_cmpfloat(rate, ==, 0);
    g_assert_true(empty);
    g_assert_false(full);
    g_assert_false(ready);
    g_assert_false(likely);
}

static void test_terminal_failure(gboolean already_playing) {
    video_renderer_set_start(0);
    init_renderer();
    video_renderer_start();
    if (already_playing) wait_state(GST_STATE_PLAYING);
    guint before_failures = failed_reports;
    inject_error(renderer->bus, renderer->pipeline);
    wait_state(GST_STATE_NULL);
    g_assert_true(direct_state.failed);
    g_assert_cmpint(direct_playback_state_target(&direct_state), ==, DIRECT_PLAYBACK_STOPPED);
    g_assert_false(renderer->direct_started);
    g_assert_cmpuint(failed_reports, ==, before_failures + 1);
    assert_failed_info();

    /* Already queued events and sender controls cannot revive a failed URI. */
    video_renderer_pause();
    video_renderer_resume();
    video_renderer_seek(1);
    video_renderer_start();
    video_renderer_hls_ready();
    buffering(0);
    buffering(100);
    GstMessage *message = gst_message_new_async_done(GST_OBJECT(renderer->pipeline), GST_CLOCK_TIME_NONE);
    gstreamer_video_pipeline_bus_callback(renderer->bus, message, NULL);
    gst_message_unref(message);
    message = gst_message_new_state_changed(GST_OBJECT(renderer->pipeline), GST_STATE_PAUSED,
                                            GST_STATE_PLAYING, GST_STATE_VOID_PENDING);
    gstreamer_video_pipeline_bus_callback(renderer->bus, message, NULL);
    gst_message_unref(message);
    inject_error(renderer->bus, renderer->pipeline);
    guint before_waits = waiting_reports;
    g_assert_false(video_renderer_eos_watch());
    g_assert_cmpuint(waiting_reports, ==, before_waits);
    g_assert_cmpuint(failed_reports, ==, before_failures + 1);
    g_assert_true(direct_state.failed);
    g_assert_cmpint(direct_state.intent, ==, DIRECT_PLAYBACK_STOPPED);
    g_assert_false(direct_state.buffering);
    g_assert_false(direct_state.preroll_complete);
    g_assert_false(hls_playing);
    wait_state(GST_STATE_NULL);
    assert_failed_info();

    /* Register replacement before destruction, exactly as on_video_play does.
     * A late error from the old renderer cannot poison that pending request. */
    GstBus *old_bus = gst_object_ref(renderer->bus);
    GstElement *old_pipeline = gst_object_ref(renderer->pipeline);
    video_renderer_set_start(0);
    g_assert_false(direct_state.failed);
    video_renderer_pause();
    inject_error(old_bus, old_pipeline);
    g_assert_false(direct_state.failed);
    g_assert_cmpint(direct_state.intent, ==, DIRECT_PLAYBACK_PAUSED);
    g_assert_cmpuint(failed_reports, ==, before_failures + 1);
    video_renderer_destroy();
    init_renderer();
    video_renderer_start();
    wait_state(GST_STATE_PAUSED);
    inject_error(old_bus, old_pipeline);
    g_assert_false(direct_state.failed);
    video_renderer_resume();
    wait_state(GST_STATE_PLAYING);
    double duration, position, seek_start, seek_duration;
    float rate;
    bool empty, full, ready, likely;
    g_assert_true(video_get_playback_info_with_readiness(&duration, &position, &seek_start, &seek_duration,
                                                        &rate, &empty, &full, &ready, &likely));
    g_assert_cmpfloat(rate, ==, 1);
    g_assert_true(ready);
    g_assert_true(likely);
    video_renderer_destroy();
    gst_object_unref(old_bus);
    gst_object_unref(old_pipeline);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    test_logger = logger_init();
    logger_set_callback(test_logger, capture_log, NULL);
    logger_set_level(test_logger, LOGGER_INFO);
    test_pi4_decoder_selection();
    if (argc == 2 && !strcmp(argv[1], "--check-pi4")) {
        logger_destroy(test_logger);
        gst_deinit();
        return 0;
    }
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
    test_terminal_failure(FALSE);
    test_terminal_failure(TRUE);
    g_remove(path);
    g_rmdir(directory);
    g_free(path);
    g_free(directory);
    g_free(fixture_uri);
    logger_destroy(test_logger);
    gst_deinit();
    return 0;
}
