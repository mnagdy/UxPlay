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
        /* The streaming thread can settle PAUSED before the main loop has
         * consumed its ASYNC_DONE. Readiness intentionally follows that bus
         * event, so wait for both sides of the transition in this harness. */
        bool preroll_pending = target == GST_STATE_PAUSED && hls_video && renderer->direct_started &&
            !direct_state.preroll_complete && !direct_state.live;
        if (state == target && pending == GST_STATE_VOID_PENDING && !preroll_pending) return;
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

static void assert_readiness(bool expected_ready, bool expected_likely) {
    double duration, position, seek_start, seek_duration;
    float rate;
    bool empty, full, ready, likely;
    g_assert_true(video_get_playback_info_with_readiness(&duration, &position, &seek_start, &seek_duration,
                                                        &rate, &empty, &full, &ready, &likely));
    g_assert_cmpint(ready, ==, expected_ready);
    g_assert_cmpint(likely, ==, expected_likely);
}

static void test_controls(void) {
    logger = test_logger;
    video_renderer_set_start(0);
    init_renderer();
    assert_readiness(false, false);
    /* A pause between construction and URI installation must not start playbin. */
    video_renderer_pause();
    GstState state;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    g_assert_cmpint(state, ==, GST_STATE_READY);
    video_renderer_start();
    wait_state(GST_STATE_PAUSED);
    assert_readiness(true, true);
    buffering(100);
    wait_state(GST_STATE_PAUSED);
    video_renderer_seek(1);
    wait_state(GST_STATE_PAUSED);
    video_renderer_resume();
    wait_state(GST_STATE_PLAYING);
    buffering(0);
    wait_state(GST_STATE_PAUSED);
    assert_readiness(true, false);
    video_renderer_resume();
    wait_state(GST_STATE_PAUSED);
    buffering(100);
    wait_state(GST_STATE_PLAYING);
    assert_readiness(true, true);
    video_renderer_hls_ready();
    buffering(100);
    wait_state(GST_STATE_READY);
    assert_readiness(false, false);
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
    assert_readiness(false, false);
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

static void test_mirror_startup_failure_is_recoverable(void) {
    GError *error = NULL;
    hls_video = false;
    type_264 = 0;
    n_renderers = 1;
    renderer = NULL;
    video_renderer_t *owner = calloc(1, sizeof(*owner));
    g_mutex_init(&owner->overlay_lock);
    owner->codec = h264;
    owner->pipeline = gst_parse_launch(
        "appsrc name=video_source is-live=true format=time ! "
        "fakesink name=test_output async=false sync=false state-error=3", &error);
    g_assert_no_error(error);
    owner->bus = gst_element_get_bus(owner->pipeline);
    owner->appsrc = gst_bin_get_by_name(GST_BIN(owner->pipeline), "video_source");
    renderer_type[0] = owner;
    /* GstFakeSink deliberately fails PAUSED -> PLAYING. The caller must get
     * an error instead of process termination or a falsely active renderer. */
    g_assert_cmpint(video_renderer_choose_codec(false, false), ==, -1);
    g_assert_null(renderer);
    GstState state;
    gst_element_get_state(owner->pipeline, &state, NULL, 0);
    g_assert_cmpint(state, ==, GST_STATE_NULL);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(owner->pipeline), "test_output");
    g_object_set(sink, "state-error", 0, NULL);
    gst_object_unref(sink);
    g_assert_cmpint(video_renderer_choose_codec(false, false), ==, 0);
    g_assert_true(renderer == owner);
    gst_element_get_state(owner->pipeline, &state, NULL, 0);
    g_assert_cmpint(state, ==, GST_STATE_PLAYING);
    video_renderer_destroy();
}

typedef struct {
    GstClockTime pts;
    gint received;
} mirror_timestamp_t;

static GstPadProbeReturn capture_mirror_timestamp(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void)pad;
    mirror_timestamp_t *observed = data;
    observed->pts = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info));
    g_atomic_int_set(&observed->received, TRUE);
    return GST_PAD_PROBE_OK;
}

static void test_mirror_sync_with_coverart(bool coverart) {
    videoflip_t flips[2] = {NONE, NONE};
    /* Construct the real mirror and optional JPEG renderers, using identity
     * stages and raw pixels to inspect pushed PTS without a codec fixture. */
    video_renderer_init(test_logger, "Headless test", flips, "identity", "", "identity",
                        "videoconvert", "fakesink", "", false, true, false, coverart, 3, NULL);
    GstElement *source = renderer_type[type_264]->appsrc;
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=RGBx,width=2,height=2,framerate=30/1");
    g_object_set(source, "caps", caps, NULL);
    gst_caps_unref(caps);
    g_assert_cmpint(video_renderer_choose_codec(false, false), ==, 0);
    mirror_timestamp_t observed = {GST_CLOCK_TIME_NONE, FALSE};
    GstPad *pad = gst_element_get_static_pad(source, "src");
    gulong probe = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, capture_mirror_timestamp, &observed, NULL);
    unsigned char pixels[16] = {0};
    int length = sizeof(pixels), nals = 1;
    g_assert_cmpuint(gst_video_pipeline_base_time, >, GST_MSECOND);
    uint64_t timestamp = gst_video_pipeline_base_time - GST_MSECOND;
    g_assert_cmpuint(video_renderer_render_buffer(pixels, &length, &nals, &timestamp), ==, GST_MSECOND);
    timestamp = gst_video_pipeline_base_time + 20 * GST_MSECOND;
    g_assert_cmpuint(video_renderer_render_buffer(pixels, &length, &nals, &timestamp), ==, 0);
    gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (!g_atomic_int_get(&observed.received) && g_get_monotonic_time() < deadline) g_usleep(1000);
    g_assert_true(g_atomic_int_get(&observed.received));
    g_assert_cmpuint(observed.pts, ==, 20 * GST_MSECOND);
    gst_pad_remove_probe(pad, probe);
    gst_object_unref(pad);
    video_renderer_destroy();
}

static void test_coverart_cycle_has_deadline(void) {
    GError *error = NULL;
    hls_video = false;
    n_renderers = 1;
    type_jpeg = 0;
    renderer = calloc(1, sizeof(*renderer));
    renderer_type[0] = renderer;
    g_mutex_init(&renderer->overlay_lock);
    renderer->codec = jpeg;
    /* This non-live source cannot finish preroll until another cover arrives. */
    renderer->pipeline = gst_parse_launch(
        "appsrc name=video_source format=time ! fakesink name=test_output sync=false", &error);
    g_assert_no_error(error);
    renderer->bus = gst_element_get_bus(renderer->pipeline);
    renderer->appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_source");
    /* Cycling applies to a renderer that has already run, so initialize the
     * source task before its first stop clears the old stream's EOS flag. */
    g_assert_cmpint(gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING), ==, GST_STATE_CHANGE_ASYNC);
    gint64 before = g_get_monotonic_time();
    g_assert_cmpint(video_renderer_cycle(), ==, -1);
    gint64 elapsed = g_get_monotonic_time() - before;
    g_assert_cmpint(elapsed, >=, 900000);
    g_assert_cmpint(elapsed, <, 2000000);
    GstState state, pending;
    g_assert_cmpint(gst_element_get_state(renderer->pipeline, &state, &pending, 0), ==, GST_STATE_CHANGE_ASYNC);
    /* The timeout frees the caller; late coverart can still complete preroll. */
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, 16, NULL);
    g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(renderer->appsrc), buffer), ==, GST_FLOW_OK);
    g_assert_cmpint(gst_element_get_state(renderer->pipeline, &state, &pending, GST_SECOND), ==, GST_STATE_CHANGE_SUCCESS);
    g_assert_cmpint(state, ==, GST_STATE_PLAYING);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "test_output");
    g_object_set(sink, "async", FALSE, NULL);
    before = g_get_monotonic_time();
    g_assert_cmpint(video_renderer_cycle(), ==, 0);
    g_assert_cmpint(g_get_monotonic_time() - before, <, 500000);
    g_object_set(sink, "state-error", 3, NULL);
    before = g_get_monotonic_time();
    g_assert_cmpint(video_renderer_cycle(), ==, -1);
    g_assert_cmpint(g_get_monotonic_time() - before, <, 500000);
    gst_object_unref(sink);
    video_renderer_destroy();
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
    test_mirror_startup_failure_is_recoverable();
    test_mirror_sync_with_coverart(false);
    test_mirror_sync_with_coverart(true);
    test_coverart_cycle_has_deadline();
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
