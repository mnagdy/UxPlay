/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise RTP delivery racing control teardown and already-queued bus errors.
 * A raw appsrc/fakesink substitutes for codec decoding so no real media, audio
 * device, or decoder is needed to reproduce the renderer lifetime failure. */
#include "../renderers/audio_renderer.c"

static gint delivered;

static void count_sample(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data) {
    (void)sink;
    (void)buffer;
    (void)pad;
    (void)data;
    g_atomic_int_inc(&delivered);
}

static void test_log(void *cls, int level, const char *message) {
    (void)cls;
    (void)level;
    (void)message;
}

static void install_test_renderers(void) {
    aac = alac = TRUE;
    async = vsync = FALSE;
    g_atomic_int_set(&delivered, 0);
    for (int i = 0; i < NFORMATS; i++) {
        audio_renderer_t *test = g_new0(audio_renderer_t, 1);
        GError *error = NULL;
        test->pipeline = gst_parse_launch(
            "appsrc name=audio_source is-live=true format=time ! "
            "fakesink name=test_sink sync=false async=false signal-handoffs=true", &error);
        g_assert_no_error(error);
        g_assert_nonnull(test->pipeline);
        test->appsrc = gst_bin_get_by_name(GST_BIN(test->pipeline), "audio_source");
        test->bus = gst_element_get_bus(test->pipeline);
        test->volume = gst_element_factory_make("volume", NULL);
        g_assert_nonnull(test->volume);
        gst_object_ref_sink(test->volume);
        test->ct = i == 0 ? 8 : 2;
        format[i] = i == 0 ? "test AAC-ELD lifetime" : "test ALAC lifetime";
        GstElement *sink = gst_bin_get_by_name(GST_BIN(test->pipeline), "test_sink");
        g_signal_connect(sink, "handoff", G_CALLBACK(count_sample), NULL);
        gst_object_unref(sink);
        renderer_type[i] = test;
    }
}

static void send_packet(unsigned char ct) {
    unsigned char packet[16] = {ct == 8 ? 0x8c : 0x20};
    int length = sizeof(packet);
    unsigned short sequence = 1;
    uint64_t timestamp = 0;
    audio_renderer_render_buffer(packet, &length, &sequence, &timestamp);
}

static void assert_packet_delivered(unsigned char ct) {
    gint before = g_atomic_int_get(&delivered);
    send_packet(ct);
    gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (g_atomic_int_get(&delivered) == before && g_get_monotonic_time() < deadline)
        g_usleep(1000);
    g_assert_cmpint(g_atomic_int_get(&delivered), >, before);
}

static GstMessage *queued_error(audio_renderer_t *source) {
    GError *error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE,
                                      "Synthetic queued audio decoder failure");
    GstMessage *message = gst_message_new_error(GST_OBJECT(source->appsrc), error, NULL);
    g_error_free(error);
    return message;
}

static void test_packet_and_error_after_stop(void) {
    install_test_renderers();
    unsigned char ct = 8;
    audio_renderer_start(&ct);
    assert_packet_delivered(ct);
    GstMessage *message = queued_error(renderer);
    GstBus *old_bus = gst_object_ref(renderer->bus);
    audio_renderer_stop();
    gint before = g_atomic_int_get(&delivered);
    for (int i = 0; i < 100; i++) send_packet(ct);
    g_assert_cmpint(g_atomic_int_get(&delivered), ==, before);
    g_assert_null(renderer);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    /* Reproduces the prior NULL dereference deterministically: the bus watch
     * had already retrieved an ERROR when a control callback stopped audio. */
    g_assert_true(gstreamer_audio_pipeline_bus_callback(old_bus, message, loop));
    g_assert_false(render_audio);
    gst_message_unref(message);
    gst_object_unref(old_bus);
    g_main_loop_unref(loop);
    audio_renderer_destroy();
}

static void test_old_error_does_not_stop_replacement(void) {
    install_test_renderers();
    unsigned char ct = 8;
    audio_renderer_start(&ct);
    GstMessage *message = queued_error(renderer);
    GstBus *old_bus = gst_object_ref(renderer->bus);
    ct = 2;
    audio_renderer_start(&ct);
    g_assert_cmpint(renderer->ct, ==, ct);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_assert_true(gstreamer_audio_pipeline_bus_callback(old_bus, message, loop));
    GstState state;
    gst_element_get_state(renderer->pipeline, &state, NULL, GST_SECOND);
    g_assert_cmpint(state, ==, GST_STATE_PLAYING);
    assert_packet_delivered(ct);
    gst_message_unref(message);
    gst_object_unref(old_bus);
    g_main_loop_unref(loop);
    audio_renderer_destroy();
}

static gpointer deliver_packets(gpointer data) {
    gint *running = data;
    while (g_atomic_int_get(running)) {
        send_packet(8);
        g_thread_yield();
    }
    return NULL;
}

static void test_packets_during_stop_and_restart(void) {
    install_test_renderers();
    unsigned char ct = 8;
    audio_renderer_start(&ct);
    gint running = TRUE;
    gint before_worker = g_atomic_int_get(&delivered);
    GThread *worker = g_thread_new("audio-packets", deliver_packets, &running);
    gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (g_atomic_int_get(&delivered) == before_worker && g_get_monotonic_time() < deadline)
        g_usleep(1000);
    g_assert_cmpint(g_atomic_int_get(&delivered), >, before_worker);
    for (int i = 0; i < 64; i++) {
        audio_renderer_stop();
        audio_renderer_start(&ct);
        g_thread_yield();
    }
    g_atomic_int_set(&running, FALSE);
    g_thread_join(worker);
    assert_packet_delivered(ct);
    audio_renderer_stop();
    gint before = g_atomic_int_get(&delivered);
    send_packet(ct);
    g_assert_cmpint(g_atomic_int_get(&delivered), ==, before);
    audio_renderer_destroy();
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    g_test_init(&argc, &argv, NULL);
    logger = logger_init();
    logger_set_callback(logger, test_log, NULL);
    g_test_add_func("/audio-renderer/packet-and-error-after-stop", test_packet_and_error_after_stop);
    g_test_add_func("/audio-renderer/stale-error-after-replacement", test_old_error_does_not_stop_replacement);
    g_test_add_func("/audio-renderer/concurrent-stop-restart", test_packets_during_stop_and_restart);
    int result = g_test_run();
    logger_destroy(logger);
    gst_deinit();
    return result;
}
