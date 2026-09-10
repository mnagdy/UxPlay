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

typedef struct {
    GMutex lock;
    guint64 buffers;
    GstClockTime first, last, largest_gap, previous_end;
    gint64 request_started, first_wall_ms;
} sink_samples_t;

static void observe_sample(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data) {
    (void)sink;
    (void)pad;
    sink_samples_t *samples = data;
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_GAP)) return;
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    GstClockTime duration = GST_BUFFER_DURATION(buffer);
    g_mutex_lock(&samples->lock);
    if (!samples->buffers) {
        samples->first = pts;
        samples->first_wall_ms = (g_get_monotonic_time() - samples->request_started) / 1000;
    }
    samples->buffers++;
    samples->last = pts;
    if (GST_CLOCK_TIME_IS_VALID(pts)) {
        if (samples->buffers > 1 && pts > samples->previous_end)
            samples->largest_gap = MAX(samples->largest_gap, pts - samples->previous_end);
        samples->previous_end = pts + (GST_CLOCK_TIME_IS_VALID(duration) ? duration : 0);
    }
    g_mutex_unlock(&samples->lock);
}

static void observe_sink(const char *property, sink_samples_t *samples) {
    GstElement *sink = NULL;
    g_object_get(renderer->pipeline, property, &sink, NULL);
    g_assert_nonnull(sink);
    g_object_set(sink, "signal-handoffs", TRUE, NULL);
    g_signal_connect(sink, "handoff", G_CALLBACK(observe_sample), samples);
    gst_object_unref(sink);
}

static gint64 time_ms(GstClockTime time) {
    return GST_CLOCK_TIME_IS_VALID(time) ? (gint64)(time / GST_MSECOND) : -1;
}

static void assert_terminal_failure(void) {
    GstState state, pending;
    gst_element_get_state(renderer->pipeline, &state, &pending, 0);
    g_assert_true(direct_state.failed);
    g_assert_cmpint(state, ==, GST_STATE_NULL);
    g_assert_cmpint(pending, ==, GST_STATE_VOID_PENDING);
    double duration = -1, position = -1, seek_start = -1, seek_duration = -1;
    float rate = -1;
    bool empty = false, full = true, ready = true, likely = true;
    /* This is the atomic snapshot used by the phone's playback-info callback.
     * Returning false would request the application's broad HLS shutdown. */
    g_assert_true(video_get_playback_info_with_readiness(&duration, &position, &seek_start,
                    &seek_duration, &rate, &empty, &full, &ready, &likely));
    g_assert_cmpfloat(duration, ==, 0);
    g_assert_cmpfloat(position, ==, 0);
    g_assert_cmpfloat(seek_start, ==, 0);
    g_assert_cmpfloat(seek_duration, ==, 0);
    g_assert_cmpfloat(rate, ==, 0);
    g_assert_true(empty);
    g_assert_false(full);
    g_assert_false(ready);
    g_assert_false(likely);
    g_print("TEST terminal_failure state=%s pending=%s failed=%d ready=%d likely=%d"
            " empty=%d full=%d rate=%.0f duration=%.0f position=%.0f seek_start=%.0f seek_duration=%.0f\n",
            gst_element_state_get_name(state), gst_element_state_get_name(pending),
            direct_state.failed, ready, likely, empty, full, rate, duration, position,
            seek_start, seek_duration);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    gboolean pi4 = FALSE, direct_http = FALSE;
    while (argc > 1 && (!strcmp(argv[1], "--pi4") || !strcmp(argv[1], "--pi4-direct"))) {
        pi4 = TRUE;
        if (!strcmp(argv[1], "--pi4-direct")) direct_http = TRUE;
        for (int i = 1; i < argc; i++) argv[i] = argv[i + 1];
        argc--;
    }
    if (argc == 5 && !strcmp(argv[1], "--prepare-pi4")) {
        return prepare_pi4_master(argv[2], argv[3], argv[4]);
    }
    gboolean observe = argc == 4 && !strcmp(argv[1], "--observe");
    gboolean replace = argc == 4 && !strcmp(argv[1], "--replace");
    gboolean replace_after_error = argc == 4 && !strcmp(argv[1], "--replace-after-error");
    if (argc != 3 && !observe && !replace && !replace_after_error) {
        g_printerr("usage: %s HTTP_HLS_FIXTURE START_SECONDS\n", argv[0]);
        g_printerr("   or: %s --replace-after-error FAILING_HTTP_FIXTURE HEALTHY_HTTP_FIXTURE\n", argv[0]);
        g_printerr("   or: %s --prepare-pi4 INPUT_MASTER OUTPUT_MASTER HTTP_PREFIX\n", argv[0]);
        return 2;
    }
    const char *uri = observe || replace || replace_after_error ? argv[2] : argv[1];
    gdouble start_seconds = observe || replace || replace_after_error ? 0 : g_ascii_strtod(argv[2], NULL);
    gdouble observe_seconds = observe ? g_ascii_strtod(argv[3], NULL) : 2;
    g_assert_true(observe_seconds >= 0 && observe_seconds <= 120);
    sink_samples_t audio = {.first = GST_CLOCK_TIME_NONE, .last = GST_CLOCK_TIME_NONE};
    sink_samples_t video = {.first = GST_CLOCK_TIME_NONE, .last = GST_CLOCK_TIME_NONE};
    g_mutex_init(&audio.lock);
    g_mutex_init(&video.lock);
    logger_t *test_logger = logger_init();
    logger_set_callback(test_logger, test_log, NULL);
    logger_set_level(test_logger, LOGGER_INFO);
    if (pi4) video_renderer_configure_pi4(test_logger);
    logger = test_logger;
    video_renderer_set_start_with_source((float)start_seconds, direct_http);
    audio.request_started = video.request_started = direct_requested_at;
    audio.first_wall_ms = video.first_wall_ms = -1;
    videoflip_t flips[2] = {NONE, NONE};
    video_renderer_init(test_logger, "HTTP HLS regression", flips, "h264parse", "",
                        "avdec_h264", "videoconvert", "fakesink", " sync=true",
                        false, true, false, false, 3, uri);
    /* Preserve real audio preroll while avoiding a dependency on sound hardware. */
    GstElement *audio_sink = gst_element_factory_make("fakesink", "test-audio-sink");
    g_object_set(audio_sink, "sync", TRUE, "async", TRUE, NULL);
    g_object_set(renderer->pipeline, "audio-sink", audio_sink, NULL);
    observe_sink("audio-sink", &audio);
    observe_sink("video-sink", &video);
    gint64 start_call = g_get_monotonic_time();
    video_renderer_start();
    g_print("TEST start_call_ms=%" G_GINT64_FORMAT "\n", (g_get_monotonic_time() - start_call) / 1000);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    gboolean played = FALSE, failed = FALSE;
    gint64 started = g_get_monotonic_time();
    gint64 deadline = started + (gint64)(observe_seconds + 30) * G_USEC_PER_SEC;
    gint64 playing_at = 0;
    gint previous_buffer_bucket = -1;
    gboolean replaced = FALSE, terminal_failure_observed = FALSE;
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
        video_renderer_eos_watch();
        if (replace_after_error && !replaced && failed) {
            /* A real bus error from the HTTP pipeline, not a timer or injected
             * message, must finish stopping this request before replacement. */
            assert_terminal_failure();
            terminal_failure_observed = TRUE;
            g_print("TEST terminal_failure_ms=%" G_GINT64_FORMAT "\n",
                    (g_get_monotonic_time() - started) / 1000);
        }
        if (!replaced && ((replace && g_get_monotonic_time() - started >= 500000) ||
                          (replace_after_error && terminal_failure_observed))) {
            gint64 replacement_at = g_get_monotonic_time();
            guint64 old_generation = renderer->direct_generation;
            video_renderer_set_start_with_source(0, direct_http);
            video_renderer_destroy();
            playing_at = 0;
            /* All old callbacks have stopped; only replacement samples count. */
            audio.buffers = video.buffers = 0;
            audio.first = audio.last = video.first = video.last = GST_CLOCK_TIME_NONE;
            audio.largest_gap = audio.previous_end = video.largest_gap = video.previous_end = 0;
            audio.request_started = video.request_started = direct_requested_at;
            audio.first_wall_ms = video.first_wall_ms = -1;
            video_renderer_init(test_logger, "HTTP HLS regression", flips, "h264parse", "",
                                "avdec_h264", "videoconvert", "fakesink", " sync=true",
                                false, true, false, false, 3, argv[3]);
            audio_sink = gst_element_factory_make("fakesink", "replacement-audio-sink");
            g_object_set(audio_sink, "sync", TRUE, "async", TRUE, NULL);
            g_object_set(renderer->pipeline, "audio-sink", audio_sink, NULL);
            observe_sink("audio-sink", &audio);
            observe_sink("video-sink", &video);
            video_renderer_start();
            if (replace_after_error) {
                g_assert_cmpuint(renderer->direct_generation, >, old_generation);
                g_assert_false(direct_state.failed);
            }
            replaced = TRUE;
            failed = FALSE;
            g_print("TEST replacement_call_ms=%" G_GINT64_FORMAT "\n",
                    (g_get_monotonic_time() - replacement_at) / 1000);
        }
        if (replace_after_error && !replaced && g_get_monotonic_time() - started >= 10 * G_USEC_PER_SEC) {
            g_printerr("Expected a terminal HTTP error within ten seconds\n");
            failed = TRUE;
            break;
        }
        GstState state, pending;
        gst_element_get_state(renderer->pipeline, &state, &pending, 0);
        gint64 position = -1;
        gst_element_query_position(renderer->pipeline, GST_FORMAT_TIME, &position);
        if (state == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING) {
            if (!playing_at) playing_at = g_get_monotonic_time();
            if ((!replace_after_error || replaced) &&
                position > (start_seconds + observe_seconds) * GST_SECOND) {
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
    if (replace_after_error && played && !failed) {
        double duration, position, seek_start, seek_duration;
        float rate;
        bool empty, full, ready, likely;
        g_assert_true(video_get_playback_info_with_readiness(&duration, &position, &seek_start,
                        &seek_duration, &rate, &empty, &full, &ready, &likely));
        g_assert_true(ready);
        g_assert_true(likely);
        g_assert_cmpfloat(rate, ==, 1);
        g_assert_cmpfloat(position, >, 1.5);
        g_assert_false(direct_state.failed);
        g_print("TEST replacement_readiness ready=%d likely=%d failed=%d rate=%.0f\n",
                ready, likely, direct_state.failed, rate);
    }
    video_renderer_destroy();
    g_print("TEST observation video_buffers=%" G_GUINT64_FORMAT " audio_buffers=%" G_GUINT64_FORMAT
            " first_audio_ms=%" G_GINT64_FORMAT " last_audio_ms=%" G_GINT64_FORMAT
            " max_audio_gap_ms=%" G_GINT64_FORMAT " last_video_ms=%" G_GINT64_FORMAT
            " first_video_wall_ms=%" G_GINT64_FORMAT "\n",
            video.buffers, audio.buffers, time_ms(audio.first), time_ms(audio.last),
            time_ms(audio.largest_gap), time_ms(video.last), video.first_wall_ms);
    g_mutex_clear(&audio.lock);
    g_mutex_clear(&video.lock);
    g_main_loop_unref(loop);
    logger_destroy(test_logger);
    gst_deinit();
    return played && !failed && (!replace_after_error || (terminal_failure_observed && replaced)) ? 0 : 1;
}
