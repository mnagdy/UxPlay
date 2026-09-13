/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../renderers/screen_status.h"
#include <glib.h>
#include <math.h>
#include <string.h>

static screen_status_snapshot_t snapshot(void) {
    screen_status_snapshot_t value;
    screen_status_get_snapshot(&value);
    return value;
}

static void ready(void) { screen_status_set_readiness(true, true, true, true, true); }

static void test_readiness_and_failure(void) {
    screen_status_init(SCREEN_INFO_STATUS, "Projector");
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_STARTING);
    for (int missing = 0; missing < 5; missing++) {
        bool flags[] = {true, true, true, true, true};
        flags[missing] = false;
        screen_status_set_readiness(flags[0], flags[1], flags[2], flags[3], flags[4]);
        g_assert_cmpint(snapshot().state, !=, SCREEN_STATE_READY);
    }
    ready();
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_READY);
    screen_status_set_readiness(true, true, false, true, true);
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_UNAVAILABLE);
    ready();
    uint64_t generation = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO,
                                                      SCREEN_ROUTE_DIRECT_HTTP, NULL);
    g_assert_true(screen_status_fail(generation, SCREEN_ERROR_DECODER));
    ready();
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_FAILED);
    g_assert_false(screen_status_event(generation, SCREEN_EVENT_OUTPUT_PROGRESS));
    g_assert_true(screen_status_event(generation, SCREEN_EVENT_STOPPED));
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_FAILED);
    screen_status_set_readiness(true, true, true, true, false);
    g_assert_false(screen_status_event(generation, SCREEN_EVENT_RECOVERED));
    ready();
    g_assert_true(screen_status_event(generation, SCREEN_EVENT_RECOVERED));
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_READY);
    g_assert_cmpint(snapshot().last_error, ==, SCREEN_ERROR_DECODER);
}

static void test_intent_and_replacement(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Projector");
    ready();
    uint64_t old = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_PLAYLIST_CACHE, "Phone");
    g_assert_true(screen_status_event(old, SCREEN_EVENT_PAUSED));
    g_assert_true(screen_status_event(old, SCREEN_EVENT_PREPARING));
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_PAUSED);
    g_assert_true(screen_status_event(old, SCREEN_EVENT_OPENING));
    g_assert_true(screen_status_event(old, SCREEN_EVENT_BUFFERING));
    g_assert_true(screen_status_event(old, SCREEN_EVENT_OUTPUT_PROGRESS));
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_PAUSED);
    g_assert_true(screen_status_event(old, SCREEN_EVENT_RESUMED));
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_WAITING_DATA);
    g_assert_true(screen_status_event(old, SCREEN_EVENT_OUTPUT_PROGRESS));
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_PLAYING);
    g_assert_true(screen_status_event(old, SCREEN_EVENT_SEEKING));
    g_assert_true(screen_status_event(old, SCREEN_EVENT_OUTPUT_PROGRESS));
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_SEEKING);
    g_assert_true(screen_status_event(old, SCREEN_EVENT_SEEK_COMPLETE));
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_WAITING_DATA);
    g_assert_true(screen_status_event(old, SCREEN_EVENT_STOPPING));
    g_assert_false(screen_status_event(old, SCREEN_EVENT_OUTPUT_PROGRESS));
    uint64_t active = screen_status_begin_session(SCREEN_SESSION_MIRRORING, SCREEN_ROUTE_RTP, NULL);
    g_assert_cmpuint(active, >, old);
    g_assert_false(screen_status_event(old, SCREEN_EVENT_STOPPED));
    g_assert_false(screen_status_fail(old, SCREEN_ERROR_SOURCE));
    screen_status_video_t video = {0};
    g_assert_false(screen_status_set_video(old, &video));
    g_assert_cmpint(snapshot().state, ==, SCREEN_STATE_INCOMING);
    g_assert_cmpstr(snapshot().sender, ==, "");
    screen_status_init(SCREEN_INFO_OFF, "Reset");
    g_assert_cmpuint(snapshot().generation, >, active);
}

static void test_safe_fields_and_format(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Office\nProjector ${unsafe} <b>");
    ready();
    uint64_t generation = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO,
        SCREEN_ROUTE_DIRECT_HTTP, "https://PRIVATE_USER:PRIVATE_PASSWORD@example.invalid/file?token=PRIVATE_TOKEN");
    screen_status_video_t video = {0};
    g_strlcpy(video.backend, "GStreamer", sizeof(video.backend));
    g_strlcpy(video.decoder, "file:///PRIVATE_PATH", sizeof(video.decoder));
    g_strlcpy(video.profile, "<b>unsafe</b>", sizeof(video.profile));
    memset(video.codec, 'Z', sizeof(video.codec)); /* Not terminated by caller. */
    g_strlcpy(video.memory, "DMABuf", sizeof(video.memory));
    video.width = UINT32_MAX;
    video.fps_num = 30;
    video.fps_den = 0;
    video.overlay_known = true;
    video.overlay_supported = false;
    g_strlcpy(video.overlay_reason, "DMABuf unqualified", sizeof(video.overlay_reason));
    g_assert_true(screen_status_set_video(generation, &video));
    screen_status_progress_t progress = {.position_known = true, .position_seconds = NAN,
        .duration_known = true, .duration_seconds = INFINITY, .buffer_known = true, .buffer_percent = 999};
    g_assert_true(screen_status_set_progress(generation, &progress));
    screen_status_audio_t audio = {.volume_known = true, .volume = NAN};
    g_strlcpy(audio.output, "password PRIVATE_PASSWORD", sizeof(audio.output));
    g_assert_true(screen_status_set_audio(generation, &audio));
    screen_status_snapshot_t s = snapshot();
    g_assert_cmpstr(s.sender, ==, "");
    g_assert_cmpstr(s.video.decoder, ==, "");
    g_assert_cmpstr(s.video.profile, ==, "");
    g_assert_cmpstr(s.audio.output, ==, "");
    g_assert_cmpuint(s.video.width, ==, 0);
    g_assert_cmpuint(strlen(s.video.codec), ==, sizeof(s.video.codec) - 1);
    g_assert_false(s.progress.position_known);
    g_assert_false(s.progress.buffer_known);
    g_assert_false(s.audio.volume_known);
    char text[4096];
    screen_status_format(&s, text, sizeof(text), false);
    g_assert_null(strstr(text, "PRIVATE_"));
    g_assert_null(strstr(text, "${"));
    g_assert_null(strstr(text, "<b>"));
    g_assert_nonnull(strstr(text, "DMABuf unqualified"));
    g_assert_nonnull(strstr(text, "Unknown"));
    uint64_t next = screen_status_begin_session(SCREEN_SESSION_AUDIO_ONLY, SCREEN_ROUTE_RTP,
        "Téléphone 日本語 Ελληνικά\n{\\an7}${command}");
    g_assert_cmpuint(next, >, generation);
    s = snapshot();
    g_assert_true(g_utf8_validate(s.sender, -1, NULL));
    g_assert_null(strstr(s.sender, "\\"));
    screen_status_init(SCREEN_INFO_DEBUG, "日本語 Téléphone");
    s = snapshot();
    for (size_t size = 1; size < 80; size++) {
        memset(text, 0xff, sizeof(text));
        size_t written = screen_status_format(&s, text, size, false);
        g_assert_cmpuint(written, <, size);
        g_assert_true(g_utf8_validate(text, -1, NULL));
        g_assert_cmpint((unsigned char)text[size], ==, 0xff);
    }
}

static void test_modes_and_history(void) {
    screen_info_mode_t mode = SCREEN_INFO_OFF;
    g_assert_true(screen_status_parse_mode("status", &mode));
    g_assert_cmpint(mode, ==, SCREEN_INFO_STATUS);
    g_assert_false(screen_status_parse_mode("DEBUG", &mode));
    g_assert_cmpint(mode, ==, SCREEN_INFO_STATUS);
    screen_status_init(mode, "Projector");
    ready();
    char text[4096];
    screen_status_snapshot_t s = snapshot();
    g_assert_cmpuint(screen_status_format(&s, text, sizeof(text), true), ==, 0);
    screen_status_format(&s, text, sizeof(text), false);
    g_assert_nonnull(strstr(text, "Choose Projector"));
    uint64_t generation = screen_status_begin_session(SCREEN_SESSION_AUDIO_ONLY, SCREEN_ROUTE_RTP, NULL);
    for (int i = 0; i < 25; i++) {
        screen_status_event(generation, SCREEN_EVENT_BUFFERING);
        screen_status_event(generation, SCREEN_EVENT_OUTPUT_PROGRESS);
    }
    s = snapshot();
    g_assert_cmpuint(s.history_count, ==, SCREEN_STATUS_HISTORY);
    g_assert_cmpuint(screen_status_format(&s, text, sizeof(text), true), ==, 0);
    screen_status_set_mode(SCREEN_INFO_DEBUG);
    s = snapshot();
    g_assert_cmpuint(screen_status_format(&s, text, sizeof(text), true), >, 0);
    g_assert_nonnull(strstr(text, "Audio connected"));
    screen_status_set_mode(SCREEN_INFO_OFF);
    s = snapshot();
    g_assert_cmpuint(screen_status_format(&s, text, sizeof(text), false), ==, 0);
}

static void test_unreported_counters_are_unknown(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Projector");
    uint64_t gen = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    screen_status_video_t video = {.output_buffers = 500};
    g_strlcpy(video.backend, "GStreamer", sizeof(video.backend));
    screen_status_set_video(gen, &video);
    screen_status_snapshot_t s = snapshot();
    g_assert_cmpint(s.last_progress_at_us, ==, 0);
    char text[4096];
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "video sink buffers Unknown"));
    g_assert_nonnull(strstr(text, "sink buffers Unknown"));
    g_assert_null(strstr(text, "500 video sink buffers"));
    video.output_buffers = 0;
    video.output_buffers_known = true;
    screen_status_set_video(gen, &video);
    s = snapshot();
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "0 video sink buffers"));
}

static void test_mpv_diagnostic_numbers(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Projector");
    uint64_t gen = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    screen_status_progress_t progress = {.cache_seconds_known = true, .cache_seconds = NAN,
        .avsync_known = true, .avsync = INFINITY, .audio_position_known = true, .audio_position = -INFINITY};
    g_assert_true(screen_status_set_progress(gen, &progress));
    screen_status_snapshot_t s = snapshot();
    g_assert_false(s.progress.cache_seconds_known);
    g_assert_false(s.progress.avsync_known);
    g_assert_false(s.progress.audio_position_known);
    g_assert_cmpfloat(s.progress.cache_seconds, ==, 0);
    g_assert_cmpfloat(s.progress.avsync, ==, 0);
    g_assert_cmpfloat(s.progress.audio_position, ==, 0);
    progress.cache_seconds = -0.1;
    progress.avsync = 1e13;
    progress.audio_position = -1e13;
    screen_status_set_progress(gen, &progress);
    s = snapshot();
    g_assert_false(s.progress.cache_seconds_known);
    g_assert_false(s.progress.avsync_known);
    g_assert_false(s.progress.audio_position_known);
    progress.cache_seconds = 0;
    progress.avsync = -0.125;
    progress.audio_position = -0.25; /* Preroll timestamps and sync offsets can be negative. */
    screen_status_set_progress(gen, &progress);
    s = snapshot();
    g_assert_true(s.progress.cache_seconds_known);
    g_assert_true(s.progress.avsync_known);
    g_assert_true(s.progress.audio_position_known);
    g_assert_cmpfloat(s.progress.cache_seconds, ==, 0);
    g_assert_cmpfloat(s.progress.avsync, ==, -0.125);
    g_assert_cmpfloat(s.progress.audio_position, ==, -0.25);
    progress.cache_seconds_known = progress.audio_position_known = progress.avsync_known = false;
    progress.cache_seconds = progress.audio_position = progress.avsync = 500;
    progress.video_decode_errors = progress.network_errors = 999;
    g_strlcpy(progress.diagnostic_stage, "video-decode", sizeof(progress.diagnostic_stage));
    screen_status_set_progress(gen, &progress);
    s = snapshot();
    g_assert_cmpfloat(s.progress.cache_seconds, ==, 0);
    g_assert_cmpfloat(s.progress.audio_position, ==, 0);
    g_assert_cmpfloat(s.progress.avsync, ==, 0);
    g_assert_cmpuint(s.progress.video_decode_errors, ==, 0);
    g_assert_cmpuint(s.progress.network_errors, ==, 0);
    g_assert_cmpstr(s.progress.diagnostic_stage, ==, "");
    screen_status_video_t video = {.output_dropped_frames = 999};
    screen_status_set_video(gen, &video);
    g_assert_cmpuint(snapshot().video.output_dropped_frames, ==, 0);
}

static void test_mpv_diagnostic_format(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Projector");
    uint64_t gen = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    screen_status_video_t video = {.width = 1280, .height = 720, .fps_num = 25, .fps_den = 1,
        .dropped_known = true, .dropped_frames = 2, .output_dropped_known = true, .output_dropped_frames = 7};
    g_strlcpy(video.backend, "mpv", sizeof(video.backend));
    g_strlcpy(video.codec, "hevc", sizeof(video.codec));
    g_strlcpy(video.pixel_format, "yuv420p10le", sizeof(video.pixel_format));
    g_strlcpy(video.memory, "UNUSED_MEMORY", sizeof(video.memory));
    screen_status_set_video(gen, &video);
    screen_status_progress_t progress = {.position_known = true, .position_seconds = 0,
        .cache_seconds_known = true, .cache_seconds = 3.5, .audio_position_known = true, .audio_position = 1.2,
        .avsync_known = true, .avsync = -0.125, .diagnostics_known = true,
        .video_decode_errors = 1, .audio_decode_errors = 2, .demux_errors = 3, .network_errors = 4,
        .video_output_errors = 5, .audio_output_errors = 6, .unclassified_errors = 7};
    g_strlcpy(progress.diagnostic_stage, "video-decode", sizeof(progress.diagnostic_stage));
    screen_status_set_progress(gen, &progress);
    screen_status_event(gen, SCREEN_EVENT_WAITING_DATA);
    screen_status_snapshot_t s = snapshot();
    g_assert_cmpint(s.state, ==, SCREEN_STATE_WAITING_DATA); /* Error reports need not be fatal. */
    char text[4096];
    for (int compact = 0; compact <= 1; compact++) {
        screen_status_format(&s, text, sizeof(text), compact);
        g_assert_nonnull(strstr(text, "yuv420p10le"));
        g_assert_nonnull(strstr(text, "position 0.0s | cache 3.5s"));
        g_assert_nonnull(strstr(text, "PTS 1.2s | AV sync -0.125s"));
        g_assert_nonnull(strstr(text, "Decoder drops: 2 | VO drops: 7"));
        g_assert_nonnull(strstr(text, "Error reports: Vdec 1 Adec 2 Demux 3 Net 4 Vout 5 Aout 6 Other 7 | last video-decode"));
        g_assert_null(strstr(text, "sink buffers"));
        g_assert_null(strstr(text, "UNUSED_MEMORY"));
    }
    progress.diagnostics_known = false;
    progress.cache_seconds_known = progress.audio_position_known = progress.avsync_known = false;
    screen_status_set_progress(gen, &progress);
    s = snapshot();
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "cache Unknown"));
    g_assert_nonnull(strstr(text, "PTS Unknown | AV sync Unknown"));
    g_assert_nonnull(strstr(text, "Error reports unavailable"));
    g_assert_null(strstr(text, "Error reports: Vdec 0"));
    progress.diagnostics_known = true;
    progress.video_decode_errors = progress.audio_decode_errors = progress.demux_errors = progress.network_errors = 0;
    progress.video_output_errors = progress.audio_output_errors = progress.unclassified_errors = 0;
    screen_status_set_progress(gen, &progress);
    s = snapshot();
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "Error reports: Vdec 0 Adec 0 Demux 0 Net 0 Vout 0 Aout 0 Other 0"));
}

static void test_mpv_diagnostic_privacy_and_generation(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Projector");
    uint64_t old = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    screen_status_video_t video = {.output_dropped_known = true, .output_dropped_frames = 23};
    g_strlcpy(video.backend, "mpv", sizeof(video.backend));
    g_strlcpy(video.pixel_format, "http://PRIVATE_TOKEN.invalid", sizeof(video.pixel_format));
    screen_status_progress_t progress = {.diagnostics_known = true, .video_decode_errors = 17,
        .cache_seconds_known = true, .cache_seconds = 12};
    g_strlcpy(progress.diagnostic_stage, "https://PRIVATE_TOKEN.invalid", sizeof(progress.diagnostic_stage));
    screen_status_set_video(old, &video);
    screen_status_set_progress(old, &progress);
    screen_status_snapshot_t s = snapshot();
    g_assert_cmpstr(s.video.pixel_format, ==, "");
    g_assert_cmpstr(s.progress.diagnostic_stage, ==, "");
    g_assert_cmpuint(s.progress.video_decode_errors, ==, 17);
    g_strlcpy(progress.diagnostic_stage, "raw arbitrary log message", sizeof(progress.diagnostic_stage));
    screen_status_set_progress(old, &progress);
    g_assert_cmpstr(snapshot().progress.diagnostic_stage, ==, "");
    memset(progress.diagnostic_stage, 'X', sizeof(progress.diagnostic_stage));
    screen_status_set_progress(old, &progress);
    g_assert_cmpstr(snapshot().progress.diagnostic_stage, ==, "");
    const char *stages[] = {"video-decode", "audio-decode", "video-output", "audio-output", "demux", "source", "unclassified"};
    for (size_t i = 0; i < G_N_ELEMENTS(stages); i++) {
        g_strlcpy(progress.diagnostic_stage, stages[i], sizeof(progress.diagnostic_stage));
        screen_status_set_progress(old, &progress);
        g_assert_cmpstr(snapshot().progress.diagnostic_stage, ==, stages[i]);
    }
    uint64_t current = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    g_assert_false(screen_status_set_progress(old, &progress));
    g_assert_false(screen_status_set_video(old, &video));
    s = snapshot();
    g_assert_cmpuint(s.generation, ==, current);
    g_assert_false(s.progress.diagnostics_known);
    g_assert_false(s.progress.cache_seconds_known);
    g_assert_false(s.video.output_dropped_known);
    g_assert_cmpuint(s.progress.video_decode_errors, ==, 0);
    g_assert_cmpstr(s.progress.diagnostic_stage, ==, "");
    g_assert_cmpstr(s.video.pixel_format, ==, "");
    screen_status_fail(current, SCREEN_ERROR_BACKEND);
    s = snapshot();
    char text[4096];
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "Playback backend failed"));
    g_assert_null(strstr(text, "PRIVATE_"));
}

static void test_mpv_observed_state_and_warnings(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Projector");
    uint64_t gen = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    screen_status_event(gen, SCREEN_EVENT_WAITING_DATA);
    screen_status_video_t video = {.width = 1280, .height = 720};
    g_strlcpy(video.backend, "mpv", sizeof(video.backend));
    screen_status_set_video(gen, &video);
    screen_status_progress_t progress = {.actual_paused_known = true, .actual_paused = true,
        .core_idle_known = true, .core_idle = false, .cache_eof_known = true, .cache_eof = true,
        .cache_underrun_known = true, .cache_underrun = false, .cache_idle_known = true, .cache_idle = true,
        .diagnostics_known = true, .log_warnings = 13, .missing_reference_warnings = 7,
        .invalid_data_warnings = 2, .timestamp_warnings = 1, .video_decode_warnings = 1,
        .audio_output_warnings = 1, .unknown_warnings = 1, .last_http_status_known = true, .last_http_status = 503};
    g_strlcpy(progress.diagnostic_reason, "missing-reference", sizeof(progress.diagnostic_reason));
    screen_status_set_progress(gen, &progress);
    screen_status_snapshot_t s = snapshot();
    g_assert_false(s.pause_requested); /* Actual state is independent of accepted user intent. */
    g_assert_true(s.progress.actual_paused);
    g_assert_cmpint(s.state, ==, SCREEN_STATE_WAITING_DATA); /* Warnings are not fatal. */
    char text[4096];
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "Waiting for playback progress"));
    g_assert_null(strstr(text, "Waiting for stream data"));
    g_assert_nonnull(strstr(text, "Video info: Unknown Unknown | 1280x720"));
    g_assert_nonnull(strstr(text, "decoded params Unknown"));
    g_assert_nonnull(strstr(text, "Player: pause yes | core idle no | cache EOF yes | underrun no | cache idle yes"));
    g_assert_nonnull(strstr(text, "Warning reports: 13 | refs 7 invalid 2 timestamps 1"));
    g_assert_nonnull(strstr(text, "Last reason: missing-reference | HTTP 503"));
    video.decoded_parameters_known = true;
    screen_status_set_video(gen, &video);
    s = snapshot();
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "decoded params available"));
    g_assert_null(strstr(text, "decoded frames"));

    const int invalid_status[] = {-1, 0, 99, 600, 999};
    for (size_t i = 0; i < G_N_ELEMENTS(invalid_status); i++) {
        progress.last_http_status = invalid_status[i];
        screen_status_set_progress(gen, &progress);
        s = snapshot();
        g_assert_false(s.progress.last_http_status_known);
        g_assert_cmpint(s.progress.last_http_status, ==, 0);
    }
    const char *private_reasons[] = {"https://PRIVATE_TOKEN.invalid", "PRIVATE_PLAIN_TEXT",
        "missing-reference <script>", "file:///PRIVATE_PATH"};
    for (size_t i = 0; i < G_N_ELEMENTS(private_reasons); i++) {
        g_strlcpy(progress.diagnostic_reason, private_reasons[i], sizeof(progress.diagnostic_reason));
        screen_status_set_progress(gen, &progress);
        g_assert_cmpstr(snapshot().progress.diagnostic_reason, ==, "");
    }
    const char *reasons[] = {"http-client-error", "http-server-error", "hls-init-failure", "hls-segment-failure",
        "hls-reload-failure", "missing-reference", "video-decode-error", "invalid-data", "timestamp-discontinuity",
        "audio-output-underrun", "audio-output-init-error", "audio-decode-error"};
    for (size_t i = 0; i < G_N_ELEMENTS(reasons); i++) {
        g_strlcpy(progress.diagnostic_reason, reasons[i], sizeof(progress.diagnostic_reason));
        screen_status_set_progress(gen, &progress);
        g_assert_cmpstr(snapshot().progress.diagnostic_reason, ==, reasons[i]);
    }

    progress.actual_paused_known = progress.core_idle_known = false;
    progress.cache_eof_known = progress.cache_underrun_known = progress.cache_idle_known = false;
    progress.actual_paused = progress.core_idle = progress.cache_eof = progress.cache_underrun = progress.cache_idle = true;
    progress.diagnostics_known = false;
    progress.last_http_status = 503;
    screen_status_set_progress(gen, &progress);
    s = snapshot();
    g_assert_false(s.progress.actual_paused);
    g_assert_false(s.progress.core_idle);
    g_assert_false(s.progress.cache_eof);
    g_assert_false(s.progress.cache_underrun);
    g_assert_false(s.progress.cache_idle);
    g_assert_false(s.progress.last_http_status_known);
    g_assert_cmpint(s.progress.last_http_status, ==, 0);
    g_assert_cmpstr(s.progress.diagnostic_reason, ==, "");
    g_assert_cmpuint(s.progress.log_warnings, ==, 0);
    g_assert_cmpuint(s.progress.video_decode_warnings, ==, 0);
    g_assert_cmpuint(s.progress.missing_reference_warnings, ==, 0);
    g_assert_cmpuint(s.progress.invalid_data_warnings, ==, 0);
    g_assert_cmpuint(s.progress.timestamp_warnings, ==, 0);
    g_assert_cmpuint(s.progress.audio_output_warnings, ==, 0);
    g_assert_cmpuint(s.progress.unknown_warnings, ==, 0);
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "Player: pause Unknown | core idle Unknown | cache EOF Unknown | underrun Unknown | cache idle Unknown"));
    g_assert_nonnull(strstr(text, "Warning reports unavailable"));
    g_assert_null(strstr(text, "Warning reports: 0"));
    g_assert_null(strstr(text, "PRIVATE_"));

    uint64_t next = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    g_assert_false(screen_status_set_progress(gen, &progress));
    g_assert_false(screen_status_set_video(gen, &video));
    s = snapshot();
    g_assert_cmpuint(s.generation, ==, next);
    g_assert_false(s.video.decoded_parameters_known);
    g_assert_false(s.progress.actual_paused_known);
    g_assert_false(s.progress.core_idle_known);
    g_assert_false(s.progress.cache_eof_known);
    g_assert_false(s.progress.cache_underrun_known);
    g_assert_false(s.progress.cache_idle_known);
    g_assert_false(s.progress.diagnostics_known);
    g_assert_cmpstr(s.progress.diagnostic_reason, ==, "");
    g_assert_cmpuint(s.progress.log_warnings, ==, 0);
}

static void test_startup_packet_capture(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Projector");
    uint64_t gen = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    screen_status_video_t video = {0};
    g_strlcpy(video.backend, "mpv", sizeof(video.backend));
    screen_status_set_video(gen, &video);
    screen_status_progress_t progress = {.diagnostics_known = true,
        .packet_capture_enabled = true, .packet_capture_active = true,
        .video_packets = 501, .audio_packets = 0, .log_warnings = 1};
    g_strlcpy(progress.last_warning_stage, "demux", sizeof(progress.last_warning_stage));
    g_strlcpy(progress.last_warning_reason, "PRIVATE_STREAM_URL", sizeof(progress.last_warning_reason));
    screen_status_set_progress(gen, &progress);
    screen_status_snapshot_t s = snapshot();
    char text[4096];
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "Startup packets: video 501 | audio 0 | capture collecting"));
    g_assert_nonnull(strstr(text, "Last warning: demux | Unknown"));
    g_assert_null(strstr(text, "PRIVATE_"));
    progress.packet_capture_active = false;
    progress.packet_capture_complete = true;
    screen_status_set_progress(gen, &progress);
    s = snapshot();
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "audio 0 | capture complete"));
    progress.packet_capture_complete = false;
    screen_status_set_progress(gen, &progress);
    s = snapshot();
    screen_status_format(&s, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "audio 0 | capture incomplete"));
    screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    g_assert_false(screen_status_set_progress(gen, &progress));
    s = snapshot();
    g_assert_false(s.progress.packet_capture_enabled);
    g_assert_cmpuint(s.progress.video_packets, ==, 0);
    g_assert_cmpstr(s.progress.last_warning_stage, ==, "");
}

static void test_failure_detail_lifetime(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Projector");
    ready();
    uint64_t gen = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    g_assert_true(screen_status_fail_detail(gen, SCREEN_ERROR_SOURCE, "mpv could not load the stream"));
    screen_status_event(gen, SCREEN_EVENT_STOPPED);
    ready();
    screen_status_snapshot_t s = snapshot();
    char text[4096];
    screen_status_format(&s, text, sizeof(text), false);
    g_assert_cmpint(s.state, ==, SCREEN_STATE_FAILED);
    g_assert_nonnull(strstr(text, "mpv could not load the stream"));
    g_assert_true(screen_status_fail_detail(gen, SCREEN_ERROR_SOURCE,
        "https://PRIVATE_USER:PRIVATE_PASSWORD@example.invalid/?token=PRIVATE_TOKEN"));
    s = snapshot();
    screen_status_format(&s, text, sizeof(text), false);
    g_assert_null(strstr(text, "PRIVATE_"));
    uint64_t next = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    g_assert_false(screen_status_fail_detail(gen, SCREEN_ERROR_BACKEND, "stale failure"));
    s = snapshot();
    g_assert_cmpuint(s.generation, ==, next);
    g_assert_cmpstr(s.error_detail, ==, "");
    g_assert_cmpint(s.error, ==, SCREEN_ERROR_NONE);
}

static gpointer stale_writer(gpointer data) {
    uint64_t old = *(uint64_t *)data;
    screen_status_video_t video = {.width = 999, .height = 999};
    for (int i = 0; i < 10000; i++) {
        g_assert_false(screen_status_set_video(old, &video));
        g_assert_false(screen_status_event(old, SCREEN_EVENT_OUTPUT_PROGRESS));
    }
    return NULL;
}

static void test_concurrent_stale_events(void) {
    screen_status_init(SCREEN_INFO_DEBUG, "Projector");
    uint64_t old = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP, NULL);
    uint64_t current = screen_status_begin_session(SCREEN_SESSION_MIRRORING, SCREEN_ROUTE_RTP, NULL);
    GThread *writer = g_thread_new("stale-status", stale_writer, &old);
    screen_status_video_t video = {.width = 1920, .height = 1080};
    for (int i = 0; i < 1000; i++) {
        g_assert_true(screen_status_set_video(current, &video));
        screen_status_snapshot_t s = snapshot();
        g_assert_cmpuint(s.generation, ==, current);
        g_assert_cmpuint(s.video.width, ==, 1920);
    }
    g_thread_join(writer);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/screen/readiness-failure", test_readiness_and_failure);
    g_test_add_func("/screen/intent-replacement", test_intent_and_replacement);
    g_test_add_func("/screen/safe-fields-format", test_safe_fields_and_format);
    g_test_add_func("/screen/modes-history", test_modes_and_history);
    g_test_add_func("/screen/unreported-counters", test_unreported_counters_are_unknown);
    g_test_add_func("/screen/mpv-diagnostic-numbers", test_mpv_diagnostic_numbers);
    g_test_add_func("/screen/mpv-diagnostic-format", test_mpv_diagnostic_format);
    g_test_add_func("/screen/mpv-diagnostic-privacy-generation", test_mpv_diagnostic_privacy_and_generation);
    g_test_add_func("/screen/mpv-observed-state-warnings", test_mpv_observed_state_and_warnings);
    g_test_add_func("/screen/startup-packet-capture", test_startup_packet_capture);
    g_test_add_func("/screen/failure-detail-lifetime", test_failure_detail_lifetime);
    g_test_add_func("/screen/concurrent-stale-events", test_concurrent_stale_events);
    return g_test_run();
}
