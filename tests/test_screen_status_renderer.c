/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Inspect this production idle renderer to verify it creates no audio owner
 * and handles bus failure/teardown using the same code as the receiver. */
#include "../renderers/screen_status_renderer.c"

static logger_t *test_logger;

static void capture_log(void *cls, int level, const char *message) {
    (void)cls; (void)level;
    g_assert_null(strstr(message, "PRIVATE_"));
}

static bool required_plugins(void) {
    const char *names[] = {"videotestsrc", "capsfilter", "textoverlay", "videoconvert", "fakesink"};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); i++) {
        GstElementFactory *factory = gst_element_factory_find(names[i]);
        if (!factory) { g_test_skip("Required GStreamer text/video plugin is unavailable"); return false; }
        gst_object_unref(factory);
    }
    return true;
}

static void test_off_does_not_acquire(void) {
    screen_status_init(SCREEN_INFO_OFF, "Projector");
    screen_status_renderer_t *r = screen_status_renderer_new(test_logger, "not-a-real-video-sink", "");
    g_assert_nonnull(r);
    g_assert_null(r->pipeline);
    g_assert_true(screen_status_renderer_show(r));
    g_assert_true(screen_status_renderer_tick(r));
    g_assert_false(screen_status_renderer_is_visible(r));
    g_assert_null(r->pipeline);
    g_assert_true(screen_status_renderer_free(r));
    g_assert_null(screen_status_renderer_new(test_logger, "fakesink ! audiotestsrc", ""));
}

static void test_lifecycle_and_no_audio(void) {
    if (!required_plugins()) return;
    screen_status_init(SCREEN_INFO_STATUS, "Projector");
    screen_status_set_readiness(true, true, true, true, true);
    screen_status_renderer_t *r = screen_status_renderer_new(test_logger, "fakesink", "sync=false enable-last-sample=true");
    g_assert_nonnull(r);
    g_assert_true(screen_status_renderer_tick(r));
    g_assert_null(r->pipeline); /* Tick must never steal the active video display. */
    for (int cycle = 0; cycle < 4; cycle++) {
        g_assert_true(screen_status_renderer_show(r));
        g_assert_true(screen_status_renderer_is_visible(r));
        GstElement *sink = gst_bin_get_by_name(GST_BIN(r->pipeline), "status-video-output");
        GstSample *sample = NULL;
        gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        do {
            g_object_get(sink, "last-sample", &sample, NULL);
            if (!sample) g_usleep(10000);
        } while (!sample && g_get_monotonic_time() < deadline);
        g_assert_nonnull(sample); /* Confirm real frames traversed textoverlay. */
        const GstStructure *caps = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
        g_assert_true(gst_structure_has_name(caps, "video/x-raw"));
        gst_sample_unref(sample);
        gst_object_unref(sink);
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(r->pipeline));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            GstElement *element = g_value_get_object(&item);
            GstElementFactory *factory = gst_element_get_factory(element);
            if (factory) g_assert_false(gst_element_factory_list_is_type(factory,
                GST_ELEMENT_FACTORY_TYPE_SINK | GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO));
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(it);
        uint64_t gen = screen_status_begin_session(SCREEN_SESSION_AUDIO_ONLY, SCREEN_ROUTE_RTP, "Phone");
        screen_status_event(gen, SCREEN_EVENT_OUTPUT_PROGRESS);
        g_assert_true(screen_status_renderer_tick(r));
        gchar *text = NULL;
        g_object_get(r->overlay, "text", &text, NULL);
        g_assert_nonnull(strstr(text, "Audio connected"));
        g_free(text);
        g_assert_true(screen_status_renderer_hide(r));
        g_assert_null(r->pipeline);
        g_assert_false(screen_status_renderer_is_visible(r));
        g_assert_true(screen_status_renderer_tick(r));
        g_assert_null(r->pipeline);
    }
    g_assert_true(screen_status_renderer_free(r));
}

static void test_failure_and_mode_change(void) {
    if (!required_plugins()) return;
    screen_status_init(SCREEN_INFO_STATUS, "Projector");
    const char *bad[] = {"does-not-exist=true", "sync=not-a-bool", "sync=false ! audiotestsrc"};
    for (unsigned i = 0; i < G_N_ELEMENTS(bad); i++) {
        screen_status_renderer_t *r = screen_status_renderer_new(test_logger, "fakesink", bad[i]);
        g_assert_false(screen_status_renderer_show(r));
        g_assert_null(r->pipeline);
        g_assert_true(screen_status_renderer_free(r));
    }
    screen_status_renderer_t *r = screen_status_renderer_new(test_logger, "fakesink", "sync=false");
    g_assert_true(screen_status_renderer_show(r));
    GError *private_error = g_error_new_literal(g_quark_from_static_string("test-error"), 1,
        "PRIVATE_TOKEN https://PRIVATE_USER:PRIVATE_PASSWORD@example.invalid/");
    gst_element_post_message(r->pipeline, gst_message_new_error(GST_OBJECT(r->pipeline), private_error, "PRIVATE_DEBUG"));
    g_error_free(private_error);
    g_assert_false(screen_status_renderer_tick(r));
    g_assert_null(r->pipeline);
    g_assert_true(screen_status_renderer_show(r));
    screen_status_set_mode(SCREEN_INFO_OFF);
    g_assert_true(screen_status_renderer_tick(r));
    g_assert_null(r->pipeline);
    screen_status_set_mode(SCREEN_INFO_DEBUG);
    g_assert_true(screen_status_renderer_tick(r));
    g_assert_null(r->pipeline); /* Changing mode alone cannot acquire output. */
    g_assert_true(screen_status_renderer_free(r));
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    g_test_init(&argc, &argv, NULL);
    test_logger = logger_init();
    logger_set_callback(test_logger, capture_log, NULL);
    g_test_add_func("/screen-renderer/off-no-acquire", test_off_does_not_acquire);
    g_test_add_func("/screen-renderer/lifecycle-no-audio", test_lifecycle_and_no_audio);
    g_test_add_func("/screen-renderer/failure-mode", test_failure_and_mode_change);
    int result = g_test_run();
    logger_destroy(test_logger);
    return result;
}
