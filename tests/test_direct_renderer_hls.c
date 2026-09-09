/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Integration runner for an HTTP HLS fixture with both audio and video. */
#include "../renderers/video_renderer.c"
#include "../lib/raop.h"

/* Populate the same production cache that /action completes, then export the
 * profiled master. The HTTP server supplies the actual generated media bodies;
 * these valid cache placeholders only establish that each route is available.
 */
static int prepare_pi4_master(const char *input, const char *output, const char *prefix) {
    gchar *master = NULL;
    gsize length = 0;
    GError *error = NULL;
    g_assert_true(g_file_get_contents(input, &master, &length, &error));
    g_assert_no_error(error);
    char **uris = NULL;
    int count = 0;
    g_assert_cmpint(create_media_uri_table(prefix, master, (int)length, &uris, &count), ==, 0);
    g_assert_cmpint(count, >, 0);
    /* Cache-only APIs keep but do not dereference the opaque server owner. */
    raop_t *owner = (raop_t *)g_malloc0(1);
    airplay_video_t *video = airplay_video_init(owner, 7000, NULL);
    g_assert_nonnull(video);
    const char *location = "http://localhost:7000/master.m3u8";
    set_playback_location(video, location, strlen(location));
    set_uri_prefix(video, prefix, strlen(prefix));
    store_master_playlist(video, master);
    create_media_data_store(video, uris, count);
    free(uris);
    for (int i = 0; i < count; i++) {
        char *body = g_strdup("#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1.0,\n"
                              "https://fixture.invalid/segment.ts\n#EXT-X-ENDLIST\n");
        float duration = 0;
        bool endlist = false;
        int chunks = analyze_media_playlist(body, &duration, &endlist);
        g_assert_cmpint(store_media_playlist(video, body, &chunks, &duration, &endlist, i), >=, 0);
    }
    g_assert_true(airplay_video_finalize_cache_profile(video, true));
    g_assert_true(airplay_video_is_ready(video));
    g_assert_true(g_file_set_contents(output, get_master_playlist(video), -1, &error));
    g_assert_no_error(error);
    airplay_video_destroy(video);
    g_free(owner);
    return 0;
}

static void test_log(void *cls, int level, const char *message) {
    (void)cls;
    (void)level;
    g_print("%s\n", message);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc == 5 && !strcmp(argv[1], "--prepare-pi4")) {
        return prepare_pi4_master(argv[2], argv[3], argv[4]);
    }
    if (argc != 3) {
        g_printerr("usage: %s HTTP_HLS_FIXTURE START_SECONDS\n", argv[0]);
        g_printerr("   or: %s --prepare-pi4 INPUT_MASTER OUTPUT_MASTER HTTP_PREFIX\n", argv[0]);
        return 2;
    }
    logger_t *test_logger = logger_init();
    logger_set_callback(test_logger, test_log, NULL);
    logger_set_level(test_logger, LOGGER_INFO);
    logger = test_logger;
    video_renderer_set_start((float)g_ascii_strtod(argv[2], NULL));
    videoflip_t flips[2] = {NONE, NONE};
    video_renderer_init(test_logger, "HTTP HLS regression", flips, "h264parse", "",
                        "avdec_h264", "videoconvert", "fakesink", " sync=true",
                        false, true, false, false, 3, argv[1]);
    /* Preserve real audio preroll while avoiding a dependency on sound hardware. */
    GstElement *audio_sink = gst_element_factory_make("fakesink", "test-audio-sink");
    g_object_set(audio_sink, "sync", TRUE, "async", TRUE, NULL);
    g_object_set(renderer->pipeline, "audio-sink", audio_sink, NULL);
    video_renderer_start();
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    gboolean played = FALSE, failed = FALSE;
    gint64 started = g_get_monotonic_time();
    gint64 deadline = started + 35 * G_USEC_PER_SEC;
    gint64 playing_at = 0;
    gint previous_buffer_bucket = -1;
    while (g_get_monotonic_time() < deadline) {
        GstMessage *message = gst_bus_timed_pop(renderer->bus, 100 * GST_MSECOND);
        if (message) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_BUFFERING) {
                gint percent;
                gst_message_parse_buffering(message, &percent);
                if (percent / 25 != previous_buffer_bucket) {
                    previous_buffer_bucket = percent / 25;
                    g_print("TEST buffering=%d source=%s\n", percent, GST_MESSAGE_SRC_NAME(message));
                }
            }
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) failed = TRUE;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ASYNC_DONE)
                g_print("TEST async-done source=%s\n", GST_MESSAGE_SRC_NAME(message));
            gstreamer_video_pipeline_bus_callback(renderer->bus, message, loop);
            gst_message_unref(message);
        }
        GstState state, pending;
        gst_element_get_state(renderer->pipeline, &state, &pending, 0);
        gint64 position = -1;
        gst_element_query_position(renderer->pipeline, GST_FORMAT_TIME, &position);
        if (state == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING) {
            if (!playing_at) playing_at = g_get_monotonic_time();
            if (position > ((gdouble)g_ascii_strtod(argv[2], NULL) + 2.0) * GST_SECOND) {
                played = TRUE;
                break;
            }
        }
        if (failed) break;
    }
    GstState state, pending;
    gst_element_get_state(renderer->pipeline, &state, &pending, 0);
    g_print("TEST result played=%d failed=%d state=%s pending=%s intent=%d buffering=%d preroll=%d live=%d playing_ms=%" G_GINT64_FORMAT "\n",
            played, failed, gst_element_state_get_name(state), gst_element_state_get_name(pending),
            direct_state.intent, direct_state.buffering, direct_state.preroll_complete,
            direct_state.live, playing_at ? (playing_at - started) / 1000 : -1);
    video_renderer_destroy();
    g_main_loop_unref(loop);
    logger_destroy(test_logger);
    gst_deinit();
    return played && !failed ? 0 : 1;
}
