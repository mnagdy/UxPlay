/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Receiver-level integration: use the real callbacks, output handover helper,
 * model and player supervisor. Media/player IPC is synthetic; display/audio
 * sinks are headless. No Pi, discovery advertisement or real media is used. */
#include <gst/gst.h>
#define UXPLAY_RECEIVER_TEST 1 /* Synthetic traces must not enter a real user's history. */
#define main uxplay_program_main
#include "../uxplay.cpp"
#undef main
#include <glib/gstdio.h>
#include <thread>

#ifndef UXPLAY_HAVE_MPV
#error This integration test requires UXPLAY_ENABLE_MPV
#endif

static const char *fake_player;
static unsigned short listener_port;

/* Run the real program entry point in a fresh process, before this harness
 * initializes GStreamer. Every exit path must leave receiver resources alone. */
static void assert_startup_isolation() {
    g_assert_false(gst_is_initialized());
    g_assert_cmpint(debug_fd, ==, -1);
    g_assert_cmpint(debug_lock, ==, -1);
    g_assert_null(debug_dir);
    g_assert_null(render_logger);
    g_assert_null(status_display);
    g_assert_null(raop);
    g_assert_null(mpv_player);
    g_assert_false(receiver_registered);
    int status = 0;
    g_assert_cmpint(waitpid(-1, &status, WNOHANG), ==, -1);
    g_assert_cmpint(errno, ==, ECHILD);
}

static void test_startup_cli(const char *executable, const char *home) {
    gchar *config = g_build_filename(home, "startup.conf", NULL);
    gchar *player_log = g_build_filename(home, "startup-player.jsonl", NULL);
    const char *contents =
        "airplay-video-backend mpv\n"
        "display-owner wayland\n"
        "vs waylandsink\n"
        "mpv-decode pi4-wayland-experimental\n"
        "mpv-vo dmabuf-wayland\n"
        "mpv-h264-hwdec v4l2m2m\n"
        "mpv-audio-device alsa/fixture\n"
        "mpv-executable /missing/configured-player\n"
        "mpv-render-profile fast\n"
        "screen-info debug\n"
        "as 0\n";
    g_assert_true(g_file_set_contents(config, contents, -1, NULL));
    auto run = [&](const char *mode, int expected,
                   std::initializer_list<const char *> overrides) {
        std::vector<const char *> args = {executable, "--run-startup-cli", "-rc", config,
            "-check-startup", "-mpv-executable", fake_player};
        args.insert(args.end(), overrides.begin(), overrides.end());
        args.push_back(NULL);
        gchar **env = g_get_environ();
        env = g_environ_unsetenv(env, "WAYLAND_DISPLAY");
        env = g_environ_unsetenv(env, "XDG_RUNTIME_DIR");
        env = g_environ_setenv(env, "UXPLAY_FAKE_MPV_MODE", mode, TRUE);
        env = g_environ_setenv(env, "UXPLAY_FAKE_MPV_LOG", player_log, TRUE);
        gchar *out = NULL, *err = NULL;
        gint status = 0;
        g_assert_true(g_spawn_sync(NULL, const_cast<gchar **>(args.data()), env,
            G_SPAWN_DEFAULT, NULL, NULL, &out, &err, &status, NULL));
        g_assert_true(WIFEXITED(status));
        g_assert_cmpint(WEXITSTATUS(status), ==, expected);
        std::string message = std::string(out) + err;
        g_free(out); g_free(err); g_strfreev(env);
        return message;
    };

    g_assert_nonnull(strstr(run("normal", 0, {}).c_str(), "Startup check passed"));
    gchar *trace = NULL;
    g_assert_true(g_file_get_contents(player_log, &trace, NULL, NULL));
    /* Configuration plus command-line override reaches the actual child. */
    for (const char *arg : {"--vo=dmabuf-wayland", "--hwdec=drm,v4l2m2m",
            "--audio-device=alsa/fixture", "--aid=no", "--profile=fast",
            "--force-window=no", "--load-scripts=no", "--osc=no"})
        g_assert_nonnull(strstr(trace, arg));
    g_assert_nonnull(strstr(trace, "\"quit\""));
    g_assert_null(strstr(trace, "\"loadfile\""));
    g_free(trace);

    g_assert_nonnull(strstr(run("missing-command", 1, {}).c_str(), "mpv startup check failed"));
    g_assert_nonnull(strstr(run("normal", 1, {"-mpv-executable", "/bin/false"}).c_str(),
                           "mpv startup check failed"));
    g_assert_nonnull(strstr(run("normal", 1, {"-mpv-drm-device", "/dev/dri/card1"}).c_str(),
                           "Display configuration"));
    g_assert_nonnull(strstr(run("normal", 1, {"-vs", "0"}).c_str(), "Display configuration"));
    g_assert_nonnull(strstr(run("normal", 1, {"-airplay-video-backend", "gstreamer"}).c_str(),
                           "requires -airplay-video-backend mpv"));
    g_assert_nonnull(strstr(run("normal", 1, {"-h"}).c_str(), "cannot be combined"));
    g_assert_nonnull(strstr(run("normal", 1, {"-v"}).c_str(), "cannot be combined"));
    g_assert_cmpint(g_unlink(config), ==, 0);
    g_assert_nonnull(strstr(run("normal", 1, {}).c_str(), "was not found"));
    g_unlink(player_log);
    g_free(config); g_free(player_log);
}

static uint64_t mirror_test_base_time;
static std::vector<uint64_t> mirror_test_submissions, mirror_test_accepted_pts;

static uint64_t timed_mirror_renderer(unsigned char *data, int *size, int *nals,
                                     uint64_t *local_timestamp) {
    g_assert_cmpint(*size, ==, 5);
    g_assert_cmpint(*nals, ==, 1);
    g_assert_cmpint(data[0], ==, 0);
    mirror_test_submissions.push_back(*local_timestamp);
    /* This is the renderer's clock boundary, not a second implementation of
     * the callback's epoch translation or retry. No decoding is requested. */
    if (*local_timestamp < mirror_test_base_time)
        return mirror_test_base_time - *local_timestamp;
    mirror_test_accepted_pts.push_back(*local_timestamp - mirror_test_base_time);
    return 0;
}

static void test_mirror_timestamp_retry() {
    auto saved_renderer = mirror_render_buffer;
    const uint64_t saved_offset = remote_clock_offset;
    mirror_render_buffer = timed_mirror_renderer;
    remote_clock_offset = 0;
    mirror_test_submissions.clear();
    mirror_test_accepted_pts.clear();
    /* Phone uptime and Pi Unix time deliberately have distinct epochs. The
     * first packet precedes the newly started pipeline by the observed 22ms. */
    const uint64_t sender_time = UINT64_C(86400) * GST_SECOND;
    const uint64_t local_time = UINT64_C(1800000000) * GST_SECOND;
    const uint64_t startup_delay = UINT64_C(22916) * GST_USECOND;
    const uint64_t frame_step = GST_SECOND / 60;
    mirror_test_base_time = local_time + startup_delay;
    unsigned char payload[] = {0, 0, 0, 1, 1};
    video_decode_struct packet = {};
    packet.data = payload;
    packet.data_len = sizeof(payload);
    packet.nal_count = 1;
    packet.ntp_time_remote = sender_time;
    packet.ntp_time_local = local_time;
    video_process(NULL, NULL, &packet);
    g_assert_cmpuint(mirror_test_submissions.size(), ==, 2);
    g_assert_cmpuint(mirror_test_submissions[0], ==, local_time);
    g_assert_cmpuint(mirror_test_submissions[1], ==, mirror_test_base_time);
    g_assert_cmpuint(mirror_test_accepted_pts.size(), ==, 1);
    g_assert_cmpuint(mirror_test_accepted_pts[0], ==, 0);
    g_assert_cmpuint(remote_clock_offset, ==, local_time - sender_time + startup_delay);

    packet.ntp_time_remote = sender_time + frame_step;
    packet.ntp_time_local = local_time + frame_step;
    video_process(NULL, NULL, &packet);
    g_assert_cmpuint(mirror_test_submissions.size(), ==, 3);
    g_assert_cmpuint(mirror_test_accepted_pts.size(), ==, 2);
    g_assert_cmpuint(mirror_test_accepted_pts[1], ==, frame_step);

    /* The ordinary no-correction path still translates exactly once. */
    remote_clock_offset = 0;
    mirror_test_submissions.clear();
    mirror_test_accepted_pts.clear();
    packet.ntp_time_remote = sender_time;
    packet.ntp_time_local = mirror_test_base_time + GST_MSECOND;
    video_process(NULL, NULL, &packet);
    g_assert_cmpuint(mirror_test_submissions.size(), ==, 1);
    g_assert_cmpuint(mirror_test_accepted_pts[0], ==, GST_MSECOND);
    mirror_render_buffer = saved_renderer;
    remote_clock_offset = saved_offset;
}

static void discard_log(void *, int, const char *text) {
    g_assert_null(strstr(text, "PRIVATE_RECEIVER_FIXTURE"));
}

static void test_audio_only_liveness() {
    const bool saved_audio = use_audio, saved_owner = mpv_owns_output;
    const bool saved_reset = reset_httpd, saved_relaunch = relaunch_video;
    const bool saved_full_reset = full_video_reset, saved_close = close_window;
    const guint saved_connections = open_connections, saved_limit = missed_feedback_limit;
    const guint saved_missed = missed_feedback.load();
    use_audio = false; /* Exercise the real callback without opening audio. */
    mpv_owns_output = false;
    open_connections = 1;
    missed_feedback_limit = 15;
    missed_feedback = 0;
    reset_httpd = relaunch_video = full_video_reset = false;
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    unsigned char payload[] = {0x20, 0};
    audio_decode_struct packet = {};
    packet.ct = 2;
    packet.data = payload;
    packet.data_len = sizeof(payload);
    /* A legacy audio sender keeps delivering ALAC without HTTP /feedback. */
    for (int second = 0; second < 40; ++second) {
        audio_process(NULL, NULL, &packet);
        feedback_callback(loop);
        g_assert_false(relaunch_video);
        g_assert_cmpuint(missed_feedback.load(), ==, 1);
    }
    missed_feedback = 8;
    packet.data_len = 0;
    audio_process(NULL, NULL, &packet);
    packet.data_len = sizeof(payload);
    payload[0] = 0;
    audio_process(NULL, NULL, &packet);
    payload[0] = 0x20;
    packet.ct = 8;
    audio_process(NULL, NULL, &packet);
    packet.ct = 2;
    mpv_owns_output = true;
    audio_process(NULL, NULL, &packet);
    g_assert_cmpuint(missed_feedback.load(), ==, 8);
    mpv_owns_output = false;
    /* The timeout remains effective after real packet delivery stops. */
    audio_process(NULL, NULL, &packet);
    for (int second = 0; second < 17; ++second) feedback_callback(loop);
    g_assert_true(reset_httpd);
    g_assert_true(relaunch_video);
    g_assert_true(full_video_reset);
    g_main_loop_unref(loop);
    use_audio = saved_audio; mpv_owns_output = saved_owner;
    reset_httpd = saved_reset; relaunch_video = saved_relaunch;
    full_video_reset = saved_full_reset; close_window = saved_close;
    open_connections = saved_connections; missed_feedback_limit = saved_limit;
    missed_feedback = saved_missed;
}

static screen_status_snapshot_t screen_snapshot() {
    screen_status_snapshot_t snapshot;
    screen_status_get_snapshot(&snapshot);
    return snapshot;
}

static mpv_backend_snapshot_t player_snapshot() {
    mpv_backend_snapshot_t snapshot;
    mpv_backend_snapshot(mpv_player, &snapshot);
    return snapshot;
}

static void tick_receiver() {
    receiver_screen_tick(NULL);
    if (compositor_owns_display)
        g_assert_true(screen_status_renderer_is_visible(status_display));
    g_usleep(3000);
}

static void await_player(mpv_backend_state_t state) {
    gint64 deadline = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
    do {
        tick_receiver();
        if (player_snapshot().state == state) return;
    } while (g_get_monotonic_time() < deadline);
    g_error("Receiver player state: wanted %d, got %d", state, player_snapshot().state);
}

static void await_rebuild() {
    gint64 deadline = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
    while (!mpv_rebuild_pending && g_get_monotonic_time() < deadline) tick_receiver();
    g_assert_true(mpv_rebuild_pending);
    g_assert_false(player_snapshot().child_alive);
    g_assert_true(mpv_owns_output); /* ownership retained until actual rebuild */
}

static void create_player(const char *mode) {
    g_setenv("UXPLAY_FAKE_MPV_MODE", mode, TRUE);
    mpv_backend_config_t config = {};
    config.executable = fake_player;
    config.video_output = "null";
    config.disable_audio = true;
    config.startup_timeout_ms = 600;
    config.load_timeout_ms = 600;
    config.stop_timeout_ms = 80;
    char error[160];
    mpv_player = mpv_backend_create(&config, error, sizeof(error));
    g_assert_nonnull(mpv_player);
    airplay_video_mpv = true;
}

static void destroy_player() {
    mpv_backend_shutdown(mpv_player);
    gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (player_snapshot().child_alive && g_get_monotonic_time() < deadline) {
        mpv_backend_poll(mpv_player);
        g_usleep(3000);
    }
    g_assert_false(player_snapshot().child_alive);
    g_assert_true(mpv_backend_destroy(mpv_player));
    mpv_player = NULL;
    airplay_video_mpv = false;
}

static void open_video(const char *suffix) {
    std::string source = "https://receiver-fixture.invalid/";
    source += suffix;
    on_video_request(NULL, true);
    on_video_play(NULL, source.c_str(), 0.0f, true);
}

static void control_request(const char *request, const char *expected) {
    int client = socket(AF_UNIX, SOCK_DGRAM, 0);
    g_assert_cmpint(client, >=, 0);
    struct sockaddr_un local = {}, server = {};
    local.sun_family = server.sun_family = AF_UNIX;
    gchar *path = g_build_filename(debug_dir, "test-client", NULL);
    g_strlcpy(local.sun_path, path, sizeof(local.sun_path));
    g_strlcpy(server.sun_path, debug_path, sizeof(server.sun_path));
    g_assert_cmpint(bind(client, (struct sockaddr *)&local, sizeof(local)), ==, 0);
    g_assert_cmpint(sendto(client, request, strlen(request), 0,
        (struct sockaddr *)&server, sizeof(server)), ==, (int)strlen(request));
    debug_control_tick(NULL);
    char response[256] = {};
    g_assert_cmpint(recv(client, response, sizeof(response)-1, MSG_DONTWAIT), >, 0);
    g_assert_cmpstr(response, ==, expected);
    close(client); unlink(path); g_free(path);
}

static void test_live_control() {
    int pid = player_snapshot().child_pid;
    uint64_t generation = screen_generation();
    bool capture = player_snapshot().packet_diagnostics_enabled;
    control_request("off", "Debug overlay off");
    g_assert_cmpint(screen_status_get_mode(), ==, SCREEN_INFO_STATUS);
    control_request("off", "Debug overlay off");
    control_request("status", "Debug overlay off");
    guint prior_feedback = missed_feedback;
    for (int i = 0; i < 30; ++i) control_request("status", "Debug overlay off");
    g_assert_cmpuint(missed_feedback, ==, prior_feedback);
    g_assert_false(reset_httpd);
    g_assert_cmpint(player_snapshot().child_pid, ==, pid);
    g_assert_cmpuint(screen_generation(), ==, generation);
    control_request("restart", "ERROR Invalid command");
    control_request("on", "Debug overlay on");
    tick_receiver();
    g_assert_cmpint(screen_status_get_mode(), ==, SCREEN_INFO_DEBUG);
    g_assert_cmpint(player_snapshot().child_pid, ==, pid);
    g_assert_cmpuint(screen_generation(), ==, generation);
    g_assert_cmpint(player_snapshot().packet_diagnostics_enabled, ==, capture);
    g_assert_true(player_snapshot().child_alive);
    /* Persist across control endpoint recreation, without touching the player. */
    debug_control_close();
    screen_info_mode_t restored = SCREEN_INFO_OFF;
    debug_control_init(&restored);
    g_assert_cmpint(restored, ==, SCREEN_INFO_DEBUG);
    gchar *path = g_build_filename(debug_dir, "mode", NULL);
    g_unlink(path); g_mkdir(path, 0700);
    control_request("off", "ERROR Could not save setting; overlay unchanged");
    g_assert_cmpint(screen_status_get_mode(), ==, SCREEN_INFO_DEBUG);
    g_rmdir(path); g_free(path);
}

static void test_mpv_callbacks_and_handover() {
    create_player("normal");
    g_assert_true(screen_status_renderer_is_visible(status_display));
    on_video_request(NULL, true);
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_PREPARING);
    on_video_stop(NULL);
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_READY);
    g_assert_false(player_snapshot().child_alive);
    open_video("first.m3u8");
    const uint64_t first = screen_generation();
    g_assert_true(mpv_owns_output);
    g_assert_false(player_snapshot().child_alive); /* callbacks merely enqueue */
    playback_info_t info;
    on_video_acquire_playback_info(NULL, &info);
    g_assert_cmpfloat(info.duration, >=, 0.0);
    g_assert_false(info.ready_to_play);
    await_player(MPV_BACKEND_PLAYING);
    g_assert_cmpint(screen_status_renderer_is_visible(status_display), ==, compositor_owns_display);
    g_assert_true(mpv_output_prepared);
    g_assert_false(playback_output_released);
    /* This fake emits one timestamp. File-loaded plus that baseline must not
     * be represented as measured timeline progress. */
    g_assert_false(screen_snapshot().output_observed);
    g_assert_cmpint(screen_snapshot().state, !=, SCREEN_STATE_PLAYING);
    on_video_acquire_playback_info(NULL, &info);
    g_assert_cmpfloat(info.duration, ==, 0.0); /* unknown live duration is not EOF */
    g_assert_true(info.ready_to_play);

    test_live_control();
    on_video_rate(NULL, 0.0f);
    await_player(MPV_BACKEND_PAUSED);
    g_assert_true(screen_snapshot().pause_requested);
    test_live_control();
    on_video_rate(NULL, 1.0f);
    await_player(MPV_BACKEND_PLAYING);
    /* Intent can be accepted before the observed pause property updates. */
    const gint64 unpause_deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while ((!player_snapshot().actual_paused_known || player_snapshot().actual_paused) &&
           g_get_monotonic_time() < unpause_deadline) tick_receiver();
    g_assert_true(player_snapshot().actual_paused_known);
    g_assert_false(player_snapshot().actual_paused);
    g_assert_false(screen_snapshot().pause_requested);
    /* This fixture emits playback-restart without advancing its position.
     * The receiver must clear pause intent but may correctly await new output. */
    g_assert_cmpint(screen_snapshot().state, !=, SCREEN_STATE_PAUSED);

    on_video_scrub(NULL, 5.0f);
    gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while ((player_snapshot().seeking || screen_snapshot().state == SCREEN_STATE_SEEKING) &&
           g_get_monotonic_time() < deadline) tick_receiver();
    g_assert_false(player_snapshot().seeking);
    g_assert_cmpint(screen_snapshot().state, !=, SCREEN_STATE_SEEKING);
    g_assert_cmpfloat(player_snapshot().position, ==, 5.0);

    /* YouTube removes the old item, accepts a new request, then sends rate=1
     * while FCUP is still fetching the replacement. The old item must stay
     * paused; preserve the new request's final pause intent independently. */
    on_video_playlist_remove(NULL);
    await_player(MPV_BACKEND_PAUSED);
    on_video_rate(NULL, 1.0f);
    for (int i = 0; i < 10; ++i) tick_receiver();
    g_assert_true(player_snapshot().requested_paused);
    on_video_request(NULL, false);
    on_video_rate(NULL, 1.0f);
    for (int i = 0; i < 10; ++i) tick_receiver();
    g_assert_true(player_snapshot().requested_paused);
    on_video_rate(NULL, 0.0f);
    on_video_play(NULL, "https://receiver-fixture.invalid/replacement.m3u8", 0, false);
    g_assert_cmpuint(screen_generation(), >, first);
    /* A queued replacement must not expose the previous player's timeline. */
    on_video_acquire_playback_info(NULL, &info);
    g_assert_cmpfloat(info.position, ==, 0.0);
    g_assert_false(info.ready_to_play);
    await_player(MPV_BACKEND_PAUSED);
    on_video_rate(NULL, 1.0f);
    await_player(MPV_BACKEND_PLAYING);
    g_assert_cmpuint(player_snapshot().generation, ==, screen_generation());

    /* Also cancel a newer accepted request while the old child is active. */
    on_video_request(NULL, true);
    g_assert_cmpuint(screen_generation(), >, player_snapshot().generation);
    on_video_stop(NULL);
    /* Deliberately ask for rebuild too early. Production helper refuses while
     * the old child could still own the device. */
    g_assert_true(player_snapshot().child_alive);
    mpv_rebuild_pending = true;
    g_assert_false(rebuild_video_outputs());
    g_assert_true(mpv_owns_output);
    mpv_rebuild_pending = false;
    await_rebuild();
    g_assert_true(rebuild_video_outputs());
    g_assert_false(mpv_owns_output);
    g_assert_true(playback_output_released);
    g_assert_true(screen_status_renderer_is_visible(status_display));
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_READY);
    on_video_acquire_playback_info(NULL, &info);
    g_assert_cmpfloat(info.duration, ==, -1.0);
    g_assert_false(info.playback_buffer_empty);

    g_assert_cmpint(video_set_codec(NULL, VIDEO_CODEC_H264), ==, 0);
    g_assert_cmpint(screen_status_renderer_is_visible(status_display), ==, compositor_owns_display);
    g_assert_cmpint(screen_snapshot().kind, ==, SCREEN_SESSION_MIRRORING);
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_OPENING);
    g_assert_false(playback_output_released);
    /* Re-entering after an obsolete reset must tolerate retired codec shells. */
    g_assert_cmpuint(video_renderer_listen(NULL, 1), ==, 0);
    preserve_connections = true;
    url.clear();
    g_assert_true(rebuild_video_outputs());
    destroy_player();
}

static void test_preparation_failure_reports_terminal() {
    create_player("normal");
    /* Stop before any player exists must also be terminal to the phone. */
    on_video_request(NULL, false);
    on_video_stop(NULL);
    playback_info_t info = {};
    on_video_acquire_playback_info(NULL, &info);
    g_assert_cmpfloat(info.duration, ==, -1.0);
    g_assert_false(info.playback_buffer_empty);
    on_video_request(NULL, false);
    on_video_request_error(NULL);
    on_video_acquire_playback_info(NULL, &info);
    g_assert_cmpfloat(info.duration, ==, -1.0);
    g_assert_cmpfloat(info.position, ==, -1.0);
    g_assert_false(info.ready_to_play);
    /* The next request can use this receiver without a network reset. */
    open_video("after-preparation-failure.m3u8");
    await_player(MPV_BACKEND_PLAYING);
    on_video_stop(NULL);
    await_rebuild();
    g_assert_true(rebuild_video_outputs());
    destroy_player();
}

static void test_failure_survives_release() {
    create_player("load-error");
    open_video("failed.m3u8");
    await_rebuild();
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_FAILED);
    g_assert_cmpint(screen_snapshot().error, ==, SCREEN_ERROR_SOURCE);
    const screen_status_snapshot_t failed = screen_snapshot();
    g_assert_cmpstr(failed.error_detail, ==, "mpv rejected the media load request");
    g_assert_true(rebuild_video_outputs());
    g_assert_false(mpv_owns_output);
    g_assert_true(playback_output_released);
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_FAILED);
    g_assert_true(screen_status_renderer_is_visible(status_display));
    char text[4096];
    screen_status_snapshot_t s = screen_snapshot();
    screen_status_format(&s, text, sizeof(text), false);
    g_assert_nonnull(strstr(text, "mpv rejected the media load request"));
    g_assert_null(strstr(text, "private.invalid"));
    destroy_player();
}

static void test_mpv_trace_privacy_and_measurements() {
    mpv_backend_snapshot_t player = {};
    player.generation = 23;
    player.state = MPV_BACKEND_BUFFERING;
    player.ready = true;
    player.video_decode_errors = 9;
    player.audio_decode_errors = 4;
    player.cache_duration_known = true;
    player.cache_duration = 1.5;
    player.width = 1280;
    player.height = 720;
    player.dropped_frames = -1;
    player.output_dropped_frames = 0;
    player.error_code = MPV_BACKEND_ERROR_FORMAT;
    player.file_error_code_known = true;
    player.file_error_code = -17;
    g_strlcpy(player.video_codec, "https://PRIVATE_RECEIVER_FIXTURE", sizeof(player.video_codec));
    g_strlcpy(player.error, "https://PRIVATE_RECEIVER_FIXTURE", sizeof(player.error));
    g_strlcpy(player.file_error, "PRIVATE_RECEIVER_FIXTURE", sizeof(player.file_error));
    g_strlcpy(player.failure_stage, "PRIVATE_RECEIVER_FIXTURE", sizeof(player.failure_stage));
    char text[6144];
    format_mpv_trace(player, text, sizeof(text));
    g_assert_null(strstr(text, "PRIVATE_RECEIVER_FIXTURE"));
    g_assert_null(strstr(text, "https"));
    g_assert_nonnull(strstr(text, "codec=other"));
    g_assert_nonnull(strstr(text, "cache_known=1 cache_seconds=1.500"));
    g_assert_nonnull(strstr(text, "decoder_drops=-1 output_drops=0"));
    g_assert_nonnull(strstr(text, "video_decode_errors=9 audio_decode_errors=4"));
    g_assert_nonnull(strstr(text, "audio_cache_duration_known=0 audio_cache_duration=0.000"));
    player.actual_paused_known = true;
    player.actual_paused = true;
    player.log_warnings = 7;
    player.last_http_status_known = true;
    player.last_http_status = 403;
    g_strlcpy(player.diagnostic_reason, "http-client-error", sizeof(player.diagnostic_reason));
    g_strlcpy(player.audio_decoder, "PRIVATE_RECEIVER_FIXTURE", sizeof(player.audio_decoder));
    g_strlcpy(player.pixel_format, "PRIVATE_RECEIVER_FIXTURE", sizeof(player.pixel_format));
    g_strlcpy(player.hwdec_current, "PRIVATE_RECEIVER_FIXTURE", sizeof(player.hwdec_current));
    format_mpv_trace(player, text, sizeof(text));
    g_assert_null(strstr(text, "PRIVATE_RECEIVER_FIXTURE"));
    g_assert_nonnull(strstr(text, "actual_paused_known=1 actual_paused=1"));
    g_assert_nonnull(strstr(text, "warning_reports=7"));
    g_assert_nonnull(strstr(text, "http_status_known=1 http_status=403"));
    g_assert_nonnull(strstr(text, "diagnostic_reason=http-client-error"));
    player.packet_diagnostics_enabled = player.packet_diagnostics_complete = true;
    player.packet_video.packets = 501;
    player.packet_video.pts_packets = 501;
    player.packet_video.first_pts = 0;
    player.packet_video.last_pts = 20;
    g_strlcpy(player.last_warning_stage, "demux", sizeof(player.last_warning_stage));
    g_strlcpy(player.last_warning_reason, "PRIVATE_RECEIVER_FIXTURE", sizeof(player.last_warning_reason));
    format_mpv_trace(player, text, sizeof(text));
    g_assert_null(strstr(text, "PRIVATE_RECEIVER_FIXTURE"));
    g_assert_nonnull(strstr(text, "packet_capture_enabled=1 packet_capture_active=0 packet_capture_complete=1"));
    g_assert_nonnull(strstr(text, "video_packets=501"));
    g_assert_nonnull(strstr(text, "audio_packets=0"));
    g_assert_nonnull(strstr(text, "last_warning_stage=demux last_warning_reason=other"));
    g_assert_true(g_str_has_suffix(text, "demux_read_warnings=0"));
    player.child_generation = 22;
    player.child_pid = 456;
    format_mpv_trace(player, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "child_pid=456 child_generation=22 terminal_report=1"));
    g_strlcpy(player.video_codec, "hevc", sizeof(player.video_codec));
    format_mpv_trace(player, text, sizeof(text));
    g_assert_nonnull(strstr(text, "codec=hevc size=1280x720"));
    g_assert_cmpint(mpv_screen_error(player), ==, SCREEN_ERROR_SOURCE);
    player.error_code = MPV_BACKEND_ERROR_AUDIO_OUTPUT;
    g_assert_cmpint(mpv_screen_error(player), ==, SCREEN_ERROR_OUTPUT);
    g_strlcpy(player.error, "mpv media loading timed out", sizeof(player.error));
    g_assert_cmpint(mpv_screen_error(player), ==, SCREEN_ERROR_TIMEOUT);
}

static void test_mpv_progress_evidence() {
    mpv_progress_observer_t observer;
    mpv_backend_snapshot_t player = {};
    player.generation = 100;
    player.position_known = true;
    player.playback_restarted = true;
    g_assert_false(observe_mpv_progress(observer, player, 1000));
    g_assert_false(observe_mpv_progress(observer, player, 2000));
    g_assert_cmpint(observer.advanced_at, ==, 0);
    player.position = 0.04;
    g_assert_true(observe_mpv_progress(observer, player, 3000));
    player.actual_paused_known = true;
    player.actual_paused = true;
    player.position = 0.08;
    g_assert_false(observe_mpv_progress(observer, player, 4000));
    player.actual_paused = false;
    g_assert_false(observe_mpv_progress(observer, player, 5000));
    player.position = 0.12;
    g_assert_true(observe_mpv_progress(observer, player, 6000));
    player.generation++;
    g_assert_false(observe_mpv_progress(observer, player, 7000));
    g_assert_cmpint(observer.advanced_at, ==, 0);
    player.seeking = true;
    player.position = 30;
    g_assert_false(observe_mpv_progress(observer, player, 8000));
    player.seeking = false;
    g_assert_false(observe_mpv_progress(observer, player, 9000));
    player.position = 30.04;
    g_assert_true(observe_mpv_progress(observer, player, 10000));
}

static void test_active_mpv_to_mirroring_handoff() {
    /* This fixture ignores both quit and SIGTERM, requiring the supervisor's
     * bounded SIGKILL/reap path before the callback can acquire video output. */
    create_player("ignore-stop");
    open_video("active-handoff.m3u8");
    await_player(MPV_BACKEND_PLAYING);
    g_assert_true(player_snapshot().child_alive);
    std::atomic<int> result(-99);
    std::thread setup([&result]() { result = video_set_codec(NULL, VIDEO_CODEC_H264); });
    gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (!mirror_waiting && result == -99 && g_get_monotonic_time() < deadline) g_usleep(1000);
    g_assert_true(mirror_waiting);
    g_assert_cmpint(result, ==, -99);
    gboolean reaped_before_acquire = FALSE;
    while (result == -99 && g_get_monotonic_time() < deadline) {
        tick_receiver();
        if (player_snapshot().child_alive) {
            g_assert_cmpint(result, ==, -99);
            g_assert_cmpint(screen_snapshot().kind, ==, SCREEN_SESSION_DIRECT_VIDEO);
        }
        if (mpv_rebuild_pending) {
            g_assert_false(player_snapshot().child_alive);
            g_assert_cmpint(result, ==, -99);
            reaped_before_acquire = TRUE;
            g_assert_true(rebuild_video_outputs());
        }
    }
    setup.join();
    g_assert_true(reaped_before_acquire);
    g_assert_cmpint(result, ==, 0);
    g_assert_false(mpv_owns_output);
    g_assert_false(player_snapshot().child_alive);
    g_assert_false(playback_output_released); /* selected mirroring owns output */
    g_assert_cmpint(screen_status_renderer_is_visible(status_display), ==, compositor_owns_display);
    g_assert_cmpint(screen_snapshot().kind, ==, SCREEN_SESSION_MIRRORING);
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_OPENING);
    preserve_connections = true;
    url.clear();
    g_assert_true(rebuild_video_outputs());
    destroy_player();
}

static void test_gstreamer_callback_route() {
    g_assert_null(mpv_player);
    std::string source = "http://127.0.0.1:" + std::to_string(listener_port) + "/headless-fixture";
    on_video_request(NULL, true);
    on_video_play(NULL, source.c_str(), 0.0f, true);
    g_assert_cmpstr(url.c_str(), ==, source.c_str());
    g_assert_true(relaunch_video);
    g_assert_false(mpv_owns_output);
    /* Pending pause and stop cancel the queued GStreamer load without invoking
     * mpv, and use the same production rebuild/release path. */
    on_video_rate(NULL, 0.0f);
    on_video_stop(NULL);
    g_assert_true(url.empty());
    g_assert_true(rebuild_video_outputs());
    g_assert_true(screen_status_renderer_is_visible(status_display));
    g_assert_true(playback_output_released);
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_READY);

    /* The legacy EOS callback immediately constructs mirror shells. The
     * production post-loop path must still suspend them and restore idle. */
    on_video_request(NULL, true);
    on_video_play(NULL, source.c_str(), 0.0f, true);
    g_assert_true(rebuild_video_outputs());
    url.clear(); /* main_loop consumes this pending URL when attaching watches */
    video_reset(NULL, RESET_TYPE_HLS_EOS);
    g_assert_true(full_video_reset);
    g_assert_true(reset_loop);
    g_assert_true(rebuild_video_outputs());
    g_assert_true(playback_output_released);
    g_assert_true(screen_status_renderer_is_visible(status_display));
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_READY);
}

static void test_audio_snapshot_requires_samples() {
    bool no_sync = false;
    audio_renderer_init(render_logger, "fakesink", &no_sync, &no_sync, "");
    unsigned char compression = 2;
    audio_renderer_start(&compression);
    screen_status_audio_t observed;
    g_assert_true(audio_renderer_get_screen_snapshot(&observed));
    g_assert_cmpuint(observed.output_buffers, ==, 0);
    g_assert_cmpuint(observed.decoded_buffers, ==, 0);
    g_assert_true(observed.input_buffers_known);
    g_assert_true(observed.decoded_buffers_known);
    g_assert_cmpstr(observed.codec, ==, ""); /* negotiation is not sample evidence */
    audio_renderer_set_volume(0.4);
    g_assert_true(audio_renderer_get_screen_snapshot(&observed));
    g_assert_true(observed.volume_known);
    g_assert_cmpfloat(fabs(observed.volume - 0.4), <, 0.000001);
    g_assert_false(observed.muted);
    audio_renderer_stop();
    g_assert_false(audio_renderer_get_screen_snapshot(&observed));
    g_assert_cmpuint(observed.output_buffers, ==, 0);
    audio_renderer_destroy();
}

static void test_display_configuration() {
    const std::string saved_sink = videosink, saved_options = videosink_options;
    gchar *saved_display = g_strdup(g_getenv("WAYLAND_DISPLAY"));
    gchar *saved_runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    g_setenv("WAYLAND_DISPLAY", "test-compositor", TRUE);
    g_setenv("XDG_RUNTIME_DIR", "/tmp", TRUE);
    compositor_owns_display = true;
    videosink = "kmssink";
    g_assert_nonnull(display_owner_config_error());
    videosink = "waylandsink";
    videosink_options = "fullscreen=true";
    g_assert_nonnull(display_owner_config_error());
    videosink_options = "sync=true";
    g_assert_null(display_owner_config_error());
    videosink_options = "sync=true ! fakesink";
    g_assert_nonnull(display_owner_config_error());
    videosink_options = "sync=true";
    airplay_video_mpv = true;
    mpv_vo = "gpu"; mpv_context = "drm";
    g_assert_nonnull(display_owner_config_error());
    mpv_vo = "dmabuf-wayland"; mpv_context.clear();
    mpv_policy = MPV_DECODE_PI4_WAYLAND_EXPERIMENTAL;
    g_assert_null(display_owner_config_error());
    mpv_drm_device = "/dev/dri/card1";
    g_assert_nonnull(display_owner_config_error());
    mpv_drm_device.clear();
    compositor_owns_display = false;
    g_assert_nonnull(display_owner_config_error());
    compositor_owns_display = true;
    g_unsetenv("WAYLAND_DISPLAY");
    g_assert_nonnull(display_owner_config_error());
    g_assert_null(display_owner_config_error(false));
    mpv_drm_device = "/dev/dri/card1";
    g_assert_nonnull(display_owner_config_error(false));
    mpv_drm_device.clear();
    if (saved_display) g_setenv("WAYLAND_DISPLAY", saved_display, TRUE);
    if (saved_runtime) g_setenv("XDG_RUNTIME_DIR", saved_runtime, TRUE);
    else g_unsetenv("XDG_RUNTIME_DIR");
    g_free(saved_display); g_free(saved_runtime);
    mpv_vo.clear(); mpv_context.clear();
    mpv_policy = MPV_DECODE_SOFTWARE;
    airplay_video_mpv = compositor_owns_display = false;
    videosink = saved_sink; videosink_options = saved_options;
}

static void test_persistent_display_loss() {
    compositor_owns_display = true;
    compositor_display_failed = false;
    show_status_display();
    create_player("normal");
    open_video("queued-display-loss.m3u8");
    g_assert_true(mpv_owns_output);
    g_assert_false(mpv_output_prepared);
    g_assert_false(player_snapshot().child_alive);
    g_assert_true(screen_status_renderer_hide(status_display));
    receiver_screen_tick(NULL);
    g_assert_true(compositor_display_failed);
    g_assert_false(relaunch_video);
    g_assert_false(mpv_output_prepared);
    g_assert_false(player_snapshot().child_alive); /* No spawn behind a lost background. */
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_RECOVERY_REQUIRED);
    show_status_display();
    g_assert_false(screen_status_renderer_is_visible(status_display)); /* Failure is sticky. */
    destroy_player();
    mpv_owns_output = mpv_rebuild_pending = false;
    mpv_output_prepared = false;

    /* A post-session rebuild can detect the loss before the next main-loop
     * tick. It must still set the sticky flag used by cleanup's exit status. */
    compositor_display_failed = false;
    show_status_display();
    g_assert_true(screen_status_renderer_is_visible(status_display));
    g_assert_true(screen_status_renderer_hide(status_display));
    relaunch_video = true;
    g_assert_false(rebuild_video_outputs());
    g_assert_true(compositor_display_failed);
    g_assert_false(relaunch_video);
    g_assert_cmpint(screen_snapshot().state, ==, SCREEN_STATE_RECOVERY_REQUIRED);

    /* An already running player is retained until the existing shutdown
     * supervisor has explicitly stopped and reaped its process. */
    compositor_display_failed = false;
    show_status_display();
    create_player("normal");
    open_video("active-display-loss.m3u8");
    await_player(MPV_BACKEND_PLAYING);
    g_assert_true(player_snapshot().child_alive);
    g_assert_true(screen_status_renderer_hide(status_display));
    receiver_screen_tick(NULL);
    g_assert_true(compositor_display_failed);
    g_assert_true(player_snapshot().child_alive);
    destroy_player(); /* Confirms shutdown/reap still succeeds after loss. */
    mpv_owns_output = mpv_rebuild_pending = false;
    mpv_output_prepared = false;
    compositor_owns_display = compositor_display_failed = false;
}

static void test_metadata_bounds_and_recovery() {
    const std::string saved_filename = metadata_filename;
    const bool saved_coverart = render_coverart;
    render_coverart = false;
    gchar *directory = g_dir_make_tmp("uxplay-metadata-test-XXXXXX", NULL);
    g_assert_nonnull(directory);
    gchar *path = g_build_filename(directory, "metadata", NULL);
    metadata_filename = path;
    auto expect_file = [&](const char *expected) {
        gchar *contents = NULL;
        g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
        g_assert_cmpstr(contents, ==, expected);
        g_free(contents);
    };
    unsigned char valid[] = {'m','l','i','t',0,0,0,11,
        'm','i','n','m',0,0,0,3,'O','n','e'};
    audio_set_metadata(NULL, valid, sizeof(valid));
    expect_file("Title: One\n");

    /* Bytes beyond the declared request exist only to make an overread
     * deterministic without relying on a crash or a sanitizer. */
    unsigned char truncated[] = {'m','l','i','t',0,0,0,9,
        'm','i','n','m',0,0,0,4,'X','B','A','D'};
    audio_set_metadata(NULL, truncated, 17);
    expect_file("Title: One\n");

    unsigned char overflow[] = {'m','l','i','t',0,0,0,8,
        'm','i','n','m',0x80,0,0,0};
    audio_set_metadata(NULL, overflow, sizeof(overflow));
    expect_file("Title: One\n");
    audio_set_metadata(NULL, NULL, 8);
    audio_set_metadata(NULL, valid, 7);
    expect_file("Title: One\n");

    memcpy(valid + 16, "Two", 3);
    audio_set_metadata(NULL, valid, sizeof(valid));
    expect_file("Title: Two\n");
    metadata_filename = saved_filename;
    render_coverart = saved_coverart;
    g_unlink(path); g_rmdir(directory);
    g_free(path); g_free(directory);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--run-metadata-test")) {
        test_metadata_bounds_and_recovery();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--run-mirror-timing-test")) {
        test_mirror_timestamp_retry();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--run-startup-cli")) {
        atexit(assert_startup_isolation);
        return uxplay_program_main(argc - 1, argv + 1);
    }
    g_assert_cmpint(argc, ==, 2);
    fake_player = argv[1];
    gchar *control_home = g_dir_make_tmp("uxplay-control-test-XXXXXX", NULL);
    g_assert_nonnull(control_home);
    g_setenv("HOME", control_home, TRUE);
    test_startup_cli(argv[0], control_home);
    test_mirror_timestamp_retry();
    test_audio_only_liveness();
    test_metadata_bounds_and_recovery();
    gst_init(NULL, NULL);
    render_logger = logger_init();
    logger_set_callback(render_logger, discard_log, NULL);
    logger_set_level(render_logger, LOGGER_INFO);
    use_video = true;
    use_audio = false;
    h265_support = true;
    hls_support = true;
    render_coverart = false;
    video_sync = false;
    videosink = "fakesink";
    videosink_options = "sync=false async=false";
    video_parser = "h264parse";
    video_decoder = "avdec_h264";
    video_converter = "videoconvert";
    server_name = "Headless receiver";
    screen_info = SCREEN_INFO_DEBUG;
    debug_control_init(&screen_info);
    g_assert_cmpint(debug_fd, >=, 0);
    screen_status_init(screen_info, "Headless receiver");
    video_renderer_configure_screen(screen_info);
    /* A transient container-only receiver listener makes readiness real at
     * that boundary. Discovery registration is deliberately a fixture flag. */
    raop_callbacks_t callbacks = {};
    callbacks.audio_process = audio_process;
    callbacks.video_process = video_process;
    raop = raop_init(&callbacks);
    g_assert_nonnull(raop);
    g_assert_cmpint(raop_init2(raop, 0, "00:11:22:33:44:55", ""), ==, 0);
    g_assert_cmpint(raop_start_httpd(raop, &listener_port), ==, 1);
    receiver_registered = true;
    screen_status_set_readiness(true, true, true, true, true);
    status_display = screen_status_renderer_new(render_logger, "fakesink", "sync=false async=false");
    g_assert_nonnull(status_display);
    video_renderer_init(render_logger, "Headless receiver", videoflip, video_parser.c_str(), "",
        video_decoder.c_str(), video_converter.c_str(), videosink.c_str(), videosink_options.c_str(),
        false, false, true, false, 3, NULL);
    video_renderer_start();
    g_assert_true(video_renderer_suspend_output());
    show_status_display();

    test_display_configuration();
    test_mpv_trace_privacy_and_measurements();
    test_mpv_progress_evidence();
    for (bool compositor : {false, true}) {
        compositor_owns_display = compositor;
        test_mpv_callbacks_and_handover();
        test_failure_survives_release();
        test_preparation_failure_reports_terminal();
        test_active_mpv_to_mirroring_handoff();
        test_gstreamer_callback_route();
    }
    test_audio_snapshot_requires_samples();

    test_persistent_display_loss();

    video_renderer_destroy();
    g_assert_true(screen_status_renderer_free(status_display));
    status_display = NULL;
    raop_destroy(raop);
    raop = NULL;
    logger_destroy(render_logger);
    render_logger = NULL;
    debug_control_close();
    gchar *lock_path = g_build_filename(control_home, ".uxplay-control", "lock", NULL);
    g_unlink(lock_path); g_free(lock_path);
    gchar *dir_path = g_build_filename(control_home, ".uxplay-control", NULL);
    g_rmdir(dir_path); g_free(dir_path); g_rmdir(control_home); g_free(control_home);
    gst_deinit();
    return 0;
}
