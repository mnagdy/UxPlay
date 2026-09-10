/* Failure-mode tests for the supervised player. SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../renderers/mpv_backend.h"

static const char *fake;
static const char *fixture_mode;

static uint64_t millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void tick(mpv_backend_t *b)
{
    uint64_t before = millis();
    mpv_backend_poll(b);
    assert(millis() - before < 500); /* callbacks/poll never wait on network */
    struct timespec delay = {0, 3000000};
    nanosleep(&delay, NULL);
}

static mpv_backend_snapshot_t snapshot(mpv_backend_t *b)
{
    mpv_backend_snapshot_t s;
    mpv_backend_snapshot(b, &s);
    return s;
}

static void await_state(mpv_backend_t *b, mpv_backend_state_t state)
{
    uint64_t end = millis() + 4000;
    do {
        tick(b);
        if (snapshot(b).state == state) return;
    } while (millis() < end);
    mpv_backend_snapshot_t s = snapshot(b);
    fprintf(stderr, "fixture=%s wanted state %d, got %d (%s)\n", fixture_mode ? fixture_mode : "real", state, s.state, s.error);
    assert(0);
}

static void cleanup(mpv_backend_t *b)
{
    mpv_backend_shutdown(b);
    uint64_t end = millis() + 3000;
    while (snapshot(b).child_alive && millis() < end) tick(b);
    assert(!snapshot(b).child_alive);
    assert(mpv_backend_destroy(b));
}

static mpv_backend_config_t config(void)
{
    mpv_backend_config_t c = {0};
    c.executable = fake;
    c.startup_timeout_ms = 600;
    c.load_timeout_ms = 500;
    c.stop_timeout_ms = 80;
    c.disable_audio = true;
    c.video_output = "null";
    return c;
}

static mpv_backend_t *create(const char *mode)
{
    fixture_mode = mode;
    setenv("UXPLAY_FAKE_MPV_MODE", mode, 1);
    mpv_backend_config_t c = config();
    /* This fixture intentionally sends every JSON line seven bytes at a time.
     * Its expanded metadata now takes longer than the ordinary timeout test. */
    if (!strcmp(mode, "partial")) c.load_timeout_ms = 2000;
    /* These fixtures deliberately spend 40 ms emitting late diagnostics plus
     * 20 ms in quit. Native Pi evidence showed all diagnostics preserved but
     * SIGTERM during Python shutdown with an 80 ms grace. Give these fixtures
     * room to exit normally; other fixtures retain forced-stop coverage. */
    if (!strcmp(mode, "stop-warnings") || !strcmp(mode, "terminal-warnings"))
        c.stop_timeout_ms = 300;
    if (!strncmp(mode, "packet-", 7)) {
        c.packet_diagnostics = true;
        c.packet_diagnostic_window_ms = 250;
    }
    char error[200];
    mpv_backend_t *b = mpv_backend_create(&c, error, sizeof(error));
    if (!b) fprintf(stderr, "%s\n", error);
    assert(b);
    assert(snapshot(b).dropped_frames == -1);
    assert(snapshot(b).output_dropped_frames == -1);
    return b;
}

static void test_validation(void)
{
    char error[200];
    mpv_backend_config_t c = config();
    c.decode_policy = MPV_DECODE_PI4_HEVC_EXPERIMENTAL;
    assert(!mpv_backend_create(&c, error, sizeof(error)));
    assert(strstr(error, "Pi HEVC requires"));
    c.decode_policy = MPV_DECODE_PI4_SAFE;
    assert(!mpv_backend_create(&c, error, sizeof(error)));
    c.qualified_h264_hwdec = "auto";
    assert(!mpv_backend_create(&c, error, sizeof(error)));
    c.qualified_h264_hwdec = "v4l2m2m-copy";
    mpv_backend_t *b = mpv_backend_create(&c, error, sizeof(error));
    assert(b);
    assert(!mpv_backend_open(b, 1, "file:///etc/passwd", 0));
    assert(!mpv_backend_open(b, 1, "http://", 0));
    assert(!mpv_backend_open(b, 1, "http://fixture/\ninvalid", 0));
    assert(!mpv_backend_open(b, 1, "http://fixture/video", NAN));
    assert(!mpv_backend_open(b, 0, "http://fixture/video", 0));
    assert(!mpv_backend_seek(b, 0, -1));
    assert(!mpv_backend_set_volume(b, 0, 101, false));
    assert(mpv_backend_destroy(b));
    c.executable = "/does/not/exist";
    assert(!mpv_backend_create(&c, error, sizeof(error)));
}

static void test_preflight(void)
{
    char error[200];
    mpv_backend_config_t c = config();
    setenv("UXPLAY_FAKE_MPV_MODE", "out-of-order", 1);
    assert(mpv_backend_preflight(&c, error, sizeof(error)));
    setenv("UXPLAY_FAKE_MPV_MODE", "missing-command", 1);
    assert(!mpv_backend_preflight(&c, error, sizeof(error)));
    assert(strstr(error, "missing"));
    setenv("UXPLAY_FAKE_MPV_MODE", "hang-start", 1);
    assert(!mpv_backend_preflight(&c, error, sizeof(error)));
    assert(strstr(error, "timed out"));
}

static void test_fast_rendering(void)
{
    char error[200];
    mpv_backend_config_t c = config();
    c.fast_rendering = true;
    setenv("UXPLAY_FAKE_MPV_MODE", "normal", 1);
    mpv_backend_t *b = mpv_backend_create(&c, error, sizeof(error));
    assert(b);
    assert(mpv_backend_open(b, 1, "http://fixture/fast", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    cleanup(b);
}

static void test_pi_hevc_mode(void)
{
    char error[200];
    mpv_backend_config_t c = config();
    c.decode_policy = MPV_DECODE_PI4_HEVC_EXPERIMENTAL;
    c.qualified_h264_hwdec = "v4l2m2m";
    c.video_output = "gpu";
    c.gpu_context = "drm";
    c.gpu_api = "opengl";
    c.fast_rendering = true;
    setenv("UXPLAY_FAKE_MPV_MODE", "atomic-reject", 1);
    mpv_backend_t *b = mpv_backend_create(&c, error, sizeof(error));
    assert(b);
    assert(mpv_backend_open(b, 1, "http://fixture/hevc", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    assert(snapshot(b).video_output_errors == 1);
    assert(!strcmp(snapshot(b).diagnostic_reason, "frame-present-failure"));
    cleanup(b);
    c.gpu_context = "x11";
    assert(!mpv_backend_create(&c, error, sizeof(error)));
}

static void test_initial_position(void)
{
    mpv_backend_t *b = create("normal");
    assert(mpv_backend_open(b, 1, "https://fixture/initial-position", 533));
    assert(mpv_backend_pause(b, 1));
    await_state(b, MPV_BACKEND_PAUSED);
    uint64_t end = millis() + 1000;
    while (snapshot(b).seeking && millis() < end) tick(b);
    assert(!snapshot(b).seeking && snapshot(b).position == 533);
    cleanup(b);
}

static void test_controls_and_replacement(const char *mode)
{
    mpv_backend_t *b = create(mode);
    assert(mpv_backend_set_volume(b, 0, 37, false));
    assert(mpv_backend_open(b, 1, "http://fixture/live?dummy=quoted\"value", 0));
    assert(mpv_backend_pause(b, 1));
    assert(mpv_backend_seek(b, 1, 7200.5));
    assert(mpv_backend_set_osd(b, 1,
        "${path} {\\b1} 1920×1080 Récepteur 東京 🎬"
        "\x01\x1b\x7f\xc2\x85\xe2\x80\xae"
        " bad=" "\xf0\x28\x8c\x28"));
    await_state(b, MPV_BACKEND_PAUSED);
    uint64_t end = millis() + 1500;
    while ((!snapshot(b).position_known || snapshot(b).position != 7200.5) && millis() < end) tick(b);
    mpv_backend_snapshot_t s = snapshot(b);
    assert(s.position_known && s.position == 7200.5);
    assert(!s.duration_known && s.duration != -1);
    assert(s.seekable_known && s.seekable);
    assert(s.width == 1920 && s.height == 1080);
    assert(s.audio_samplerate == 48000 && s.audio_channels == 2);
    assert(s.dropped_frames == 4 && !strcmp(s.video_codec, "h264"));
    assert(s.output_dropped_frames == 2 && !strcmp(s.video_decoder, "h264"));
    assert(!strcmp(s.video_profile, "High") && !strcmp(s.audio_decoder, "aac"));
    assert(s.cache_duration_known && s.cache_duration == 3.5);
    assert(s.cache_bytes_known && s.cache_bytes == 123456);
    assert(s.cache_speed_known && s.cache_speed == 456789);
    assert(s.cache_buffering_known && s.cache_buffering_percent == 65.5);
    assert(s.avsync_known && s.avsync == -0.04);
    assert(s.audio_position_known && s.audio_position == -0.1);
    assert(s.log_messages_active && !s.metadata_observation_errors);
    assert(s.child_pid > 0 && s.decoded_parameters_known && s.decoded_width == 1920);
    assert(s.selected_video_known && s.selected_video_id == 1);
    assert(s.selected_audio_known && s.selected_audio_id == 2);
    assert(s.cache_video.reader_pts_known && s.cache_video.reader_pts == 3.5);
    assert(s.cache_audio.cache_end_known && s.cache_audio.cache_end == 6.9);
    assert(s.video_observation_age_ms >= 0 && s.audio_observation_age_ms >= 0 && s.cache_observation_age_ms >= 0);
    assert(!strcmp(s.hwdec_current, "no"));
    assert(s.requested_paused && s.ready);
    assert(mpv_backend_resume(b, 1));
    await_state(b, MPV_BACKEND_PLAYING);
    assert(mpv_backend_open(b, 2, "https://fixture/new", 0));
    assert(!mpv_backend_pause(b, 1));
    assert(!mpv_backend_stop(b, 1));
    assert(!mpv_backend_seek(b, 1, 10));
    assert(!mpv_backend_set_volume(b, 1, 55, false));
    assert(!mpv_backend_open(b, 1, "https://fixture/stale", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    s = snapshot(b);
    assert(s.generation == 2 && !s.duration_known && s.duration != 9999);
    assert(mpv_backend_stop(b, 2));
    await_state(b, MPV_BACKEND_IDLE);
    assert(!snapshot(b).child_alive);
    cleanup(b);
}

static void test_failure(const char *mode, const char *diagnostic)
{
    mpv_backend_t *b = create(mode);
    assert(mpv_backend_open(b, 1, "https://fixture/error", 0));
    await_state(b, MPV_BACKEND_FAILED);
    mpv_backend_snapshot_t s = snapshot(b);
    assert(strstr(s.error, diagnostic));
    assert(s.error_code != MPV_BACKEND_ERROR_NONE && s.failure_stage[0]);
    assert(!strstr(s.error, "http"));
    assert(!s.ready);
    cleanup(b);
}

static void test_diagnostics(void)
{
    mpv_backend_t *b = create("diagnostic-errors");
    assert(mpv_backend_open(b, 1, "https://fixture/diagnostics", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    uint64_t end = millis() + 1000;
    while (snapshot(b).log_errors < 7 && millis() < end) tick(b);
    mpv_backend_snapshot_t s = snapshot(b);
    assert(s.log_messages_active && s.log_errors == 7);
    assert(s.video_decode_errors == 1 && s.audio_decode_errors == 1);
    assert(s.video_output_errors == 1 && s.audio_output_errors == 1);
    assert(s.demux_errors == 1 && s.network_errors == 1 && s.unclassified_errors == 1);
    assert(!strcmp(s.diagnostic_stage, "unclassified"));
    assert(!s.error[0] && s.error_code == MPV_BACKEND_ERROR_NONE);
    assert(mpv_backend_pause(b, 1));
    await_state(b, MPV_BACKEND_PAUSED); /* diagnostic errors do not change control */
    setenv("UXPLAY_FAKE_MPV_MODE", "normal", 1);
    assert(mpv_backend_open(b, 2, "https://fixture/replacement", 0));
    assert(!snapshot(b).log_errors && !snapshot(b).diagnostic_stage[0]);
    await_state(b, MPV_BACKEND_PLAYING);
    assert(!snapshot(b).log_errors);
    cleanup(b);

    b = create("invalid-metadata");
    assert(mpv_backend_open(b, 1, "https://fixture/metadata", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    s = snapshot(b);
    assert(!s.video_decoder[0] && !s.audio_decoder[0] && !s.video_profile[0]);
    assert(s.output_dropped_frames == -1);
    assert(!s.cache_duration_known && !s.cache_bytes_known && !s.cache_speed_known);
    assert(!s.cache_duration && !s.cache_video.reader_pts_known && !s.cache_video.reader_pts);
    assert(!s.cache_video.cache_end_known && !s.cache_video.cache_end);
    assert(!s.cache_video.cache_duration_known && !s.cache_video.cache_duration);
    assert(!s.cache_buffering_known && !s.avsync_known && !s.audio_position_known);
    assert(s.fps == 0);
    cleanup(b);

    b = create("metadata-unavailable");
    assert(mpv_backend_open(b, 1, "https://fixture/no-metadata", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    assert(snapshot(b).metadata_observation_errors > 0);
    assert(!snapshot(b).video_decoder[0] && snapshot(b).output_dropped_frames == -1);
    cleanup(b);
}

static void test_terminal_diagnostics(const char *mode, mpv_backend_error_t error, int file_code)
{
    mpv_backend_t *b = create(mode);
    assert(mpv_backend_open(b, 1, "https://fixture/terminal", 0));
    uint64_t end = millis() + 2000;
    while (snapshot(b).state == MPV_BACKEND_STARTING && millis() < end) tick(b);
    /* Let an immediately exiting fixture leave an unread terminal event. */
    struct timespec delay = {0, 80000000};
    nanosleep(&delay, NULL);
    await_state(b, MPV_BACKEND_FAILED);
    mpv_backend_snapshot_t s = snapshot(b);
    assert(s.error_code == error && !strcmp(s.end_reason, "error"));
    assert(s.file_error_code_known == (file_code != 0));
    assert(s.file_error_code == file_code && !strstr(s.file_error, "private"));
    if (!file_code) assert(!strcmp(s.file_error, "unknown"));
    end = millis() + 2000;
    while (snapshot(b).child_alive && millis() < end) tick(b);
    s = snapshot(b);
    assert(!s.child_alive && s.child_exit_known && s.error_code == error);
    assert(s.file_error_code == file_code); /* teardown cannot erase cause */
    if (!strcmp(mode, "end-format-exit")) assert(s.child_exit_code == 19 && !s.child_signal);
    cleanup(b);
}

static void test_warning_and_stall_evidence(void)
{
    mpv_backend_t *b = create("warning-reasons");
    assert(mpv_backend_open(b, 1, "https://fixture/warnings", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    uint64_t end = millis() + 1000;
    while (snapshot(b).log_warnings < 13 && millis() < end) tick(b);
    mpv_backend_snapshot_t s = snapshot(b);
    assert(s.log_warnings == 13 && !s.log_errors);
    assert(s.video_decode_warnings == 1 && s.missing_reference_warnings == 1);
    assert(s.invalid_data_warnings == 1 && s.timestamp_warnings == 1 && s.audio_output_warnings == 1);
    assert(s.unknown_warnings == 4 && s.log_text_rejected == 2);
    assert(s.last_http_status_known && s.last_http_status == 503 && s.http_error_count == 1);
    assert(s.hls_init_failures == 1 && s.hls_segment_failures == 1 && s.hls_reload_failures == 1);
    assert(!strcmp(s.diagnostic_reason, "hls-reload-failure"));
    assert(s.diagnostic_reason_at_ms && !s.error_code);
    cleanup(b);

    b = create("stall-paused");
    assert(mpv_backend_open(b, 1, "https://fixture/stalled", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    for (int i = 0; i < 8; ++i) tick(b);
    s = snapshot(b);
    assert(!s.requested_paused && s.actual_paused_known && s.actual_paused);
    assert(s.core_idle_known && s.core_idle);
    assert(s.cache_underrun_known && s.cache_underrun && s.cache_idle_known && !s.cache_idle);
    assert(s.cache_eof_known && !s.cache_eof && s.cache_bytes_known && s.cache_bytes == 0);
    assert(s.cache_video.reader_pts_known && !s.cache_audio.reader_pts_known);
    assert(!s.decoded_parameters_known && s.width == 1920); /* container metadata is not decoded output */
    assert(!s.audio_samplerate && s.audio_observed_at_ms && s.audio_observation_age_ms >= 0);
    cleanup(b);
}

static void test_terminal_warning_drain(void)
{
    mpv_backend_t *b = create("terminal-warnings");
    assert(mpv_backend_open(b, 1, "https://fixture/terminal-warnings", 0));
    await_state(b, MPV_BACKEND_FAILED);
    uint64_t end = millis() + 1000;
    while (snapshot(b).log_warnings < 2 && millis() < end) tick(b);
    mpv_backend_snapshot_t s = snapshot(b);
    assert(s.state == MPV_BACKEND_FAILED && !s.ready && s.child_alive);
    assert(s.terminal_diagnostics_draining);
    assert(s.log_warnings == 2 && s.video_decode_warnings == 1 && !s.log_errors);
    assert(s.last_http_status_known && s.last_http_status == 403);
    assert(s.file_error_code_known && s.file_error_code == -17 && s.error_code == MPV_BACKEND_ERROR_FORMAT);
    assert(s.decoded_width == 1920 && s.position != 9999); /* post-terminal properties/events ignored */
    assert(s.event_overflows == 1 && s.log_overflows == 1);
    assert(!mpv_backend_resume(b, 1));
    while (snapshot(b).child_alive && millis() < end) tick(b);
    s = snapshot(b);
    assert(!s.child_alive && !s.terminal_diagnostics_draining && s.state == MPV_BACKEND_FAILED);
    assert(s.last_http_status == 403 && !strcmp(s.diagnostic_reason, "video-decode-error"));
    cleanup(b);
}

static void test_packet_diagnostics(void)
{
    const char *modes[] = {"packet-normal", "packet-video-only", "packet-audio-unknown", "packet-none", "packet-malicious",
        "packet-restore-error", "packet-restore-timeout", "packet-log-reject", "packet-warning-reasons",
        "packet-late-audio", "packet-cutoff-budget", "packet-cutoff-overflow", "packet-after-marker", "packet-marker-missing"};
    for (size_t i = 0; i < sizeof(modes)/sizeof(modes[0]); ++i) {
        const char *mode = modes[i];
        mpv_backend_t *b = create(mode);
        assert(mpv_backend_open(b, 1, "https://fixture/packet-capture", 0));
        await_state(b, MPV_BACKEND_PLAYING);
        uint64_t end = millis() + 2000;
        while (!snapshot(b).packet_capture_ended_at_ms && millis() < end) tick(b);
        /* Allow restore/subscription replies after stopping, including the
         * deliberate missing-ack fixture. Playback must remain usable. */
        end = millis() + (!strcmp(mode, "packet-restore-timeout") ? 1500 : 100);
        while (millis() < end) tick(b);
        mpv_backend_snapshot_t s = snapshot(b);
        assert(s.packet_diagnostics_enabled && !s.packet_diagnostics_active);
        assert(s.packet_capture_started_at_ms && s.packet_capture_ended_at_ms >= s.packet_capture_started_at_ms);
        assert(s.state == MPV_BACKEND_PLAYING && !s.error_code);
        bool rejected = !strcmp(mode, "packet-log-reject") || !strcmp(mode, "packet-none");
        assert(s.packet_video.packets == (rejected ? 0 : 2));
        if (!rejected) {
            assert(s.packet_video.bytes == 2200 && s.packet_video.pts_packets == 2);
            assert(s.packet_video.first_pts == 0 && s.packet_video.last_pts == .04);
            assert(s.packet_video.first_at_ms && s.packet_video.last_at_ms >= s.packet_video.first_at_ms);
        }
        if (!strcmp(mode, "packet-late-audio") || !strcmp(mode, "packet-cutoff-budget") || !strcmp(mode, "packet-cutoff-overflow")) {
            assert(s.packet_audio.packets == 1 && s.packet_audio.bytes == 256 && s.packet_audio.pts_packets == 1);
            assert(s.packet_audio.first_pts == 0 && s.packet_audio.last_pts == 0);
            if (!strcmp(mode, "packet-cutoff-budget")) assert(s.ipc_read_budget_exhaustions > 0);
        } else if (!strcmp(mode, "packet-audio-unknown")) {
            assert(s.packet_audio.packets == 1 && s.packet_audio.bytes == 120 && !s.packet_audio.pts_packets);
        } else if (!rejected && strcmp(mode, "packet-video-only") && strcmp(mode, "packet-after-marker")) {
            assert(s.packet_audio.packets == 3 && s.packet_audio.bytes == 330);
            assert(s.packet_audio.pts_packets == 2 && s.packet_audio.first_pts == 0 && s.packet_audio.last_pts == .02);
        } else assert(!s.packet_audio.packets && !s.packet_audio.pts_packets);
        assert(s.packet_log_rejected == (!strcmp(mode, "packet-malicious") ? 4 : 0));
        bool complete = !strcmp(mode, "packet-normal") || !strcmp(mode, "packet-video-only") ||
            !strcmp(mode, "packet-audio-unknown") || !strcmp(mode, "packet-warning-reasons") ||
            !strcmp(mode, "packet-late-audio") || !strcmp(mode, "packet-cutoff-budget");
        assert(s.packet_diagnostics_complete == complete);
        if (!strcmp(mode, "packet-after-marker")) assert(s.packet_capture_errors == 1);
        if (!strcmp(mode, "packet-warning-reasons")) {
            assert(s.pes_mismatch_warnings == 1 && s.packet_corrupt_warnings == 1 && s.demux_read_warnings == 1);
            assert(!strcmp(s.diagnostic_reason, "virtual-terminal-unavailable"));
            assert(!strcmp(s.last_warning_stage, "demux") && !strcmp(s.last_warning_reason, "unknown"));
            assert(s.last_warning_at_ms && s.video_output_errors == 1);
        }
        cleanup(b);
    }
}

static void test_replacement_terminal_report(void)
{
    mpv_backend_t *b = create("terminal-warnings");
    assert(mpv_backend_open(b, 1, "https://fixture/old-terminal", 0));
    await_state(b, MPV_BACKEND_FAILED);
    int old_pid = snapshot(b).child_pid;
    assert(old_pid > 0 && snapshot(b).terminal_diagnostics_draining);
    setenv("UXPLAY_FAKE_MPV_MODE", "normal", 1);
    assert(mpv_backend_open(b, 2, "https://fixture/replaced-before-spawn", 0));
    assert(mpv_backend_open(b, 3, "https://fixture/current", 0));
    mpv_backend_snapshot_t s = snapshot(b);
    assert(s.generation == 3 && s.child_generation == 1 && s.child_pid == old_pid);
    await_state(b, MPV_BACKEND_PLAYING);
    s = snapshot(b);
    assert(s.generation == 3 && s.child_generation == 3 && s.child_pid != old_pid);
    assert(!s.log_warnings && !s.error_code && !s.last_http_status_known);
    mpv_backend_snapshot_t old;
    assert(mpv_backend_take_terminal_snapshot(b, &old));
    assert(old.generation == 1 && old.child_generation == 1 && old.child_pid == old_pid && !old.child_alive);
    assert(old.error_code == MPV_BACKEND_ERROR_FORMAT && old.file_error_code == -17);
    assert(old.log_warnings == 2 && old.video_decode_warnings == 1 && old.last_http_status == 403);
    if (!(old.decoded_width == 1920 && old.position != 9999 && old.child_exit_known && old.child_exit_code == 0))
        fprintf(stderr, "Retiring fixture: width=%d position=%.3f exit_known=%d exit_code=%d signal=%d warnings=%llu generation=%llu\n",
            old.decoded_width, old.position, old.child_exit_known, old.child_exit_code, old.child_signal,
            (unsigned long long)old.log_warnings, (unsigned long long)old.generation);
    assert(old.decoded_width == 1920 && old.position != 9999 && old.child_exit_known && old.child_exit_code == 0);
    assert(!strcmp(old.diagnostic_reason, "video-decode-error"));
    assert(!mpv_backend_take_terminal_snapshot(b, &old));
    cleanup(b);

    b = create("normal");
    for (uint64_t generation = 1; generation <= 6; ++generation) {
        assert(mpv_backend_open(b, generation, "https://fixture/report-queue", 0));
        await_state(b, MPV_BACKEND_PLAYING);
        assert(mpv_backend_stop(b, generation));
        await_state(b, MPV_BACKEND_IDLE);
    }
    assert(snapshot(b).terminal_reports_dropped == 2);
    for (uint64_t generation = 3; generation <= 6; ++generation) {
        assert(mpv_backend_take_terminal_snapshot(b, &old));
        assert(old.generation == generation && old.terminal_reports_dropped == 2);
    }
    assert(!mpv_backend_take_terminal_snapshot(b, &old));
    cleanup(b);
}

static void test_stopped_terminal_report(void)
{
    mpv_backend_t *b = create("stop-warnings");
    assert(mpv_backend_open(b, 1, "https://fixture/stopped", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    int pid = snapshot(b).child_pid;
    assert(mpv_backend_stop(b, 1));
    assert(snapshot(b).state == MPV_BACKEND_STOPPING);
    await_state(b, MPV_BACKEND_IDLE);
    mpv_backend_snapshot_t final;
    assert(mpv_backend_take_terminal_snapshot(b, &final));
    assert(final.generation == 1 && final.child_pid == pid && !final.child_alive);
    if (!final.child_exit_known || final.child_exit_code)
        fprintf(stderr, "stop-warnings exit_known=%d exit_code=%d signal=%d warnings=%llu\n",
                final.child_exit_known, final.child_exit_code, final.child_signal,
                (unsigned long long)final.log_warnings);
    assert(final.child_exit_known && !final.child_exit_code);
    assert(final.error_code == MPV_BACKEND_ERROR_FORMAT && final.file_error_code == -17);
    assert(final.log_warnings == 2 && final.video_decode_warnings == 1);
    assert(final.last_http_status_known && final.last_http_status == 403);
    assert(final.event_overflows == 1 && !strcmp(final.end_reason, "error"));
    assert(!final.ready && final.duration != 9999 && final.state == MPV_BACKEND_FAILED);
    cleanup(b);
}

static void test_seek_failure(void)
{
    mpv_backend_t *b = create("seek-error");
    assert(mpv_backend_open(b, 1, "https://fixture/live", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    assert(mpv_backend_seek(b, 1, 42));
    assert(snapshot(b).seeking);
    uint64_t end = millis() + 1000;
    while (!snapshot(b).control_error[0] && millis() < end) tick(b);
    assert(!strcmp(snapshot(b).control_error, "mpv rejected seek"));
    assert(!snapshot(b).seeking);
    assert(snapshot(b).state == MPV_BACKEND_PLAYING);
    cleanup(b);
}

static void test_seek_completion(void)
{
    mpv_backend_t *nonseekable = create("nonseekable");
    assert(mpv_backend_open(nonseekable, 1, "https://fixture/live-no-seek", 0));
    await_state(nonseekable, MPV_BACKEND_PLAYING);
    assert(snapshot(nonseekable).seekable_known && !snapshot(nonseekable).seekable);
    assert(!mpv_backend_seek(nonseekable, 1, 42));
    assert(!snapshot(nonseekable).seeking);
    cleanup(nonseekable);
    mpv_backend_t *b = create("seek-delayed");
    assert(mpv_backend_open(b, 1, "https://fixture/seek", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    for (int i = 0; i < 5; ++i) tick(b);
    assert(mpv_backend_seek(b, 1, 10));
    tick(b);
    assert(snapshot(b).seeking);
    assert(mpv_backend_seek(b, 1, 20));
    assert(mpv_backend_seek(b, 1, 30)); /* coalesces latest pending intent */
    uint64_t end = millis() + 2000;
    bool saw_first = false;
    do {
        tick(b);
        mpv_backend_snapshot_t s = snapshot(b);
        if (s.position == 10) { saw_first = true; assert(s.seeking); }
        if (!s.seeking) break;
    } while (millis() < end);
    assert(saw_first && !snapshot(b).seeking && snapshot(b).position == 30);
    cleanup(b);
    b = create("seek-never-completes");
    assert(mpv_backend_open(b, 1, "https://fixture/seek-timeout", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    for (int i = 0; i < 5; ++i) tick(b);
    assert(mpv_backend_seek(b, 1, 42));
    end = millis() + 2000;
    while (snapshot(b).seeking && millis() < end) tick(b);
    assert(!snapshot(b).seeking);
    assert(!strcmp(snapshot(b).control_error, "mpv seek completion timed out"));
    cleanup(b);
}

static void test_eof_and_forced_shutdown(void)
{
    mpv_backend_t *b = create("eof");
    assert(mpv_backend_open(b, 1, "https://fixture/end", 0));
    await_state(b, MPV_BACKEND_ENDED);
    assert(!snapshot(b).duration_known);
    cleanup(b);
    b = create("ignore-stop");
    assert(mpv_backend_open(b, 1, "https://fixture/ignore-stop", 0));
    await_state(b, MPV_BACKEND_PLAYING);
    assert(!mpv_backend_destroy(b));
    cleanup(b); /* Requires TERM then KILL, still bounded and reaped. */
    b = create("normal");
    assert(mpv_backend_open(b, 1, "http://fixture/cancel-before-poll", 0));
    assert(mpv_backend_stop(b, 1));
    tick(b);
    assert(!snapshot(b).child_alive);
    cleanup(b);
}

/* Optional real-player path: caller supplies a local HTTP fixture URL.
 * It deliberately uses no screen/audio device; physical output needs Pi QA. */
static void test_real(const char *url)
{
    char error[200];
    mpv_backend_config_t c = {0};
    c.video_output = "null";
    c.audio_device = "null";
    c.packet_diagnostics = true;
    c.packet_diagnostic_window_ms = 1000;
    assert(mpv_backend_preflight(&c, error, sizeof(error)));
    mpv_backend_t *b = mpv_backend_create(&c, error, sizeof(error));
    assert(b && mpv_backend_open(b, 1, url, 0.5));
    assert(mpv_backend_pause(b, 1));
    await_state(b, MPV_BACKEND_PAUSED);
    uint64_t end = millis() + 1500;
    while ((!snapshot(b).duration_known || !snapshot(b).seekable ||
            !snapshot(b).width || !snapshot(b).video_decoder[0] ||
            !snapshot(b).audio_decoder[0]) && millis() < end) tick(b);
    mpv_backend_snapshot_t s = snapshot(b);
    assert(s.width > 0 && s.height > 0 && s.duration_known);
    assert(s.video_decoder[0] && s.audio_decoder[0] && s.video_codec[0]);
    assert(s.log_messages_active);
    assert(s.duration > 1 && s.seekable_known && s.seekable);
    end = millis() + 2000;
    while ((snapshot(b).seeking || snapshot(b).position < .45) && millis() < end) tick(b);
    assert(!snapshot(b).seeking && snapshot(b).position >= .45);
    assert(mpv_backend_seek(b, 1, 1));
    assert(snapshot(b).seeking);
    assert(mpv_backend_set_osd(b, 1, "${path} {\\b1} literal diagnostic"));
    assert(mpv_backend_resume(b, 1));
    await_state(b, MPV_BACKEND_PLAYING);
    end = millis() + 2000;
    while ((!snapshot(b).position_known || snapshot(b).position < 1) && millis() < end) tick(b);
    assert(snapshot(b).position_known && snapshot(b).position >= 1);
    end = millis() + 2000;
    while (snapshot(b).seeking && millis() < end) tick(b);
    assert(!snapshot(b).seeking);
    end = millis() + 1500;
    while (strcmp(snapshot(b).audio_output, "null") && millis() < end) tick(b);
    assert(!strcmp(snapshot(b).audio_output, "null"));
    assert(snapshot(b).pixel_format[0]);
    assert(!strcmp(snapshot(b).hwdec_current, "no"));
    assert(!snapshot(b).control_error[0]);
    end = millis() + 2500;
    while (!snapshot(b).packet_capture_ended_at_ms && millis() < end) tick(b);
    for (int i = 0; i < 20; ++i) tick(b);
    s = snapshot(b);
    fprintf(stderr, "Packet capture: video=%llu audio=%llu rejected=%llu loss=%llu/%llu budgets=%llu errors=%llu complete=%d\n",
        (unsigned long long)s.packet_video.packets, (unsigned long long)s.packet_audio.packets,
        (unsigned long long)s.packet_log_rejected, (unsigned long long)s.log_overflows,
        (unsigned long long)s.event_overflows, (unsigned long long)s.ipc_read_budget_exhaustions,
        (unsigned long long)s.packet_capture_errors, s.packet_diagnostics_complete);
    assert(s.packet_video.packets > 0 && s.packet_audio.packets > 0);
    assert(s.packet_video.pts_packets > 0 && s.packet_audio.pts_packets > 0);
    assert(!s.packet_log_rejected && !s.packet_capture_errors && !s.log_overflows && !s.event_overflows);
    assert(s.packet_diagnostics_complete && !s.packet_diagnostics_active);
    cleanup(b);
    puts("Real mpv HTTP playback/control test passed (null video/audio outputs).");
}

static void test_real_rejected(const char *url)
{
    char error[200];
    mpv_backend_config_t c = {0};
    c.video_output = "null";
    c.audio_device = "null";
    mpv_backend_t *b = mpv_backend_create(&c, error, sizeof(error));
    assert(b && mpv_backend_open(b, 1, url, 0));
    await_state(b, MPV_BACKEND_FAILED);
    assert(!strstr(snapshot(b).error, "://"));
    assert(snapshot(b).log_messages_active);
    /* Real FFmpeg protocol rejection must reach the error-module observer
     * even though ordinary console logging stays disabled. */
    if (strstr(url, "local-reference.m3u8")) assert(snapshot(b).log_errors > 0);
    cleanup(b);
    puts("Real mpv rejected a disallowed local-file playlist reference.");
}

static void test_real_http_error(const char *url)
{
    mpv_backend_config_t c = {0};
    c.video_output = "null";
    c.audio_device = "null";
    char error[200];
    mpv_backend_t *b = mpv_backend_create(&c, error, sizeof(error));
    assert(b && mpv_backend_open(b, 1, url, 0));
    await_state(b, MPV_BACKEND_FAILED);
    uint64_t end = millis() + 2000;
    while (snapshot(b).child_alive && millis() < end) tick(b);
    mpv_backend_snapshot_t s = snapshot(b);
    assert(s.last_http_status_known && s.last_http_status == 403 && s.http_error_count > 0);
    assert(s.log_warnings > 0 && !strstr(s.file_error, "http") && !strstr(s.error, "://"));
    cleanup(b);
    puts("Real mpv HTTP403 warning classified without retaining source text.");
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--real")) { test_real(argv[2]); return 0; }
    if (argc == 3 && !strcmp(argv[1], "--real-rejected")) { test_real_rejected(argv[2]); return 0; }
    if (argc == 3 && !strcmp(argv[1], "--real-http-error")) { test_real_http_error(argv[2]); return 0; }
    assert(argc == 2);
    fake = argv[1];
    int extra_pipe[2];
    assert(pipe(extra_pipe) == 0);
    char extra_fd[32];
    snprintf(extra_fd, sizeof(extra_fd), "%d", extra_pipe[1]);
    setenv("UXPLAY_FAKE_MPV_EXTRA_FD", extra_fd, 1);
    char log_path[128], lock_path[128];
    snprintf(log_path, sizeof(log_path), "/tmp/uxplay-mpv-test-%ld.log", (long)getpid());
    snprintf(lock_path, sizeof(lock_path), "/tmp/uxplay-mpv-test-%ld.lock", (long)getpid());
    setenv("UXPLAY_FAKE_MPV_LOG", log_path, 1);
    setenv("UXPLAY_FAKE_MPV_LOCK", lock_path, 1);
    test_validation();
    test_preflight();
    test_fast_rendering();
    test_pi_hevc_mode();
    test_initial_position();
    test_controls_and_replacement("normal");
    test_controls_and_replacement("partial");
    test_controls_and_replacement("out-of-order");
    test_failure("load-error", "rejected");
    test_failure("load-hang", "timed out");
    test_failure("malformed", "malformed");
    test_failure("oversized", "size limit");
    test_failure("eof-ipc", "mpv");
    test_failure("crash", "mpv");
    test_failure("end-error", "ended");
    test_diagnostics();
    test_terminal_diagnostics("end-error", MPV_BACKEND_ERROR_PLAYBACK, 0);
    test_terminal_diagnostics("end-audio-error", MPV_BACKEND_ERROR_AUDIO_OUTPUT, -14);
    test_terminal_diagnostics("end-format-exit", MPV_BACKEND_ERROR_FORMAT, -17);
    test_warning_and_stall_evidence();
    test_packet_diagnostics();
    test_terminal_warning_drain();
    test_replacement_terminal_report();
    test_stopped_terminal_report();
    test_seek_failure();
    test_seek_completion();
    test_eof_and_forced_shutdown();
    FILE *f = fopen(log_path, "r");
    assert(f);
    char line[18000];
    bool raw_osd = false, utf8_osd = false, software = false, disabled_audio = false;
    bool fast_rendering = false, default_rendering = false, hevc_mode = false, initial_start = false;
    while (fgets(line, sizeof(line), f)) {
        assert(!strstr(line, "overlapping_children"));
        assert(!strstr(line, "inherited_extra_descriptor"));
        assert(!strstr(line, "\"seek\", 533"));
        if (strstr(line, "\"argv\"")) {
            initial_start |= strstr(line, "--start=533") != NULL;
            if (strstr(line, "--hwdec=drm,v4l2m2m")) {
                hevc_mode = true;
                assert(strstr(line, "--hwdec-codecs=h264,hevc"));
                assert(strstr(line, "--hwdec-software-fallback=no"));
                assert(strstr(line, "--gpu-hwdec-interop=drmprime-overlay"));
                assert(strstr(line, "--drm-drmprime-video-plane=primary"));
                assert(strstr(line, "--drm-draw-plane=overlay"));
                assert(strstr(line, "--drm-draw-surface-size=1280x720"));
            } else {
                assert(!strstr(line, "--gpu-hwdec-interop="));
            }
            assert(!strstr(line, "http://") && !strstr(line, "https://"));
            software |= strstr(line, "--hwdec=no") != NULL;
            disabled_audio |= strstr(line, "--aid=no") != NULL;
            fast_rendering |= strstr(line, "--profile=fast") != NULL;
            default_rendering |= strstr(line, "--profile=") == NULL;
        }
        raw_osd |= strstr(line, "\"raw\", \"show-text\", \"${path}") != NULL;
        if (strstr(line, "\"raw\", \"show-text\"")) {
            utf8_osd |= strstr(line, "1920\\u00d71080 R\\u00e9cepteur \\u6771\\u4eac \\ud83c\\udfac bad=?(?(") != NULL;
            assert(!strstr(line, "\\u0001") && !strstr(line, "\\u001b") &&
                   !strstr(line, "\\u007f") && !strstr(line, "\\u0085") &&
                   !strstr(line, "\\u202e"));
        }
    }
    fclose(f);
    assert(raw_osd && utf8_osd && software && disabled_audio);
    assert(fast_rendering && default_rendering && hevc_mode && initial_start);
    unlink(log_path);
    unlink(lock_path);
    close(extra_pipe[0]);
    close(extra_pipe[1]);
    puts("mpv backend IPC, controls, policy and child-lifetime tests passed.");
    return 0;
}
