/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "screen_status.h"

#include <glib.h>
#include <math.h>
#include <string.h>

static GMutex status_mutex;
static screen_status_snapshot_t status;
static uint64_t next_generation;
static bool was_ready;
static screen_state_t interrupted_state;

/* Only short display labels enter the model. Never accept a location or a
 * credential-bearing string, even when a caller accidentally supplies one as
 * metadata. Invalid UTF-8 and display command/markup characters are omitted. */
static void safe_label(char *out, size_t capacity, const char *in, size_t limit) {
    size_t used = 0;
    if (!capacity) return;
    out[0] = '\0';
    if (!in) return;
    size_t length = strnlen(in, limit);
    for (size_t i = 0; i < length; i++) {
        if (strchr(":/@=?&\\", in[i])) return;
    }
    gchar *bounded = g_strndup(in, length);
    gchar *lower = g_ascii_strdown(bounded, -1);
    bool sensitive = strstr(lower, "www.") || strstr(lower, "password") ||
                     strstr(lower, "credential") || strstr(lower, "token") ||
                     strstr(lower, "authorization") || strstr(lower, "cookie");
    g_free(lower);
    if (sensitive) { g_free(bounded); return; }
    const char *p = bounded;
    const char *end = bounded + length;
    while (p < end && used + 1 < capacity) {
        gunichar ch = g_utf8_get_char_validated(p, (gssize)(end - p));
        if (ch == (gunichar)-1 || ch == (gunichar)-2) { p++; continue; }
        const char *after = g_utf8_next_char(p);
        bool allowed = g_unichar_isalnum(ch) || ch == ' ' || ch == '-' ||
                       ch == '_' || ch == '.' || ch == '(' || ch == ')' || ch == '+';
        if (allowed) {
            size_t bytes = (size_t)(after - p);
            if (used + bytes >= capacity) break;
            memcpy(out + used, p, bytes);
            used += bytes;
        } else if (used && out[used - 1] != ' ') {
            out[used++] = ' ';
        }
        p = after;
    }
    while (used && out[used - 1] == ' ') used--;
    out[used] = '\0';
    g_free(bounded);
}

static bool valid_mode(screen_info_mode_t mode) {
    return mode >= SCREEN_INFO_OFF && mode <= SCREEN_INFO_DEBUG;
}

static void change_state(screen_state_t state) {
    if (status.state == state) return;
    status.state = state;
    status.changed_at_us = g_get_monotonic_time();
    if (status.history_count == SCREEN_STATUS_HISTORY) {
        memmove(status.history, status.history + 1,
                sizeof(status.history[0]) * (SCREEN_STATUS_HISTORY - 1));
        status.history_count--;
    }
    screen_status_milestone_t *entry = &status.history[status.history_count++];
    entry->state = state;
    entry->elapsed_ms = status.requested_at_us > 0 ?
        (uint64_t)((status.changed_at_us - status.requested_at_us) / 1000) : 0;
    status.revision++;
}

static bool receiver_ready(void) {
    return status.network_ready && status.listener_ready &&
           status.discovery_ready && status.backend_ready;
}

static void update_idle(void) {
    if (status.session_active || status.state == SCREEN_STATE_FAILED ||
        status.state == SCREEN_STATE_RECOVERY_REQUIRED) return;
    if (!status.network_ready) change_state(SCREEN_STATE_WAITING_NETWORK);
    else if (receiver_ready() && status.output_released) {
        change_state(SCREEN_STATE_READY);
        was_ready = true;
    } else change_state(was_ready ? SCREEN_STATE_UNAVAILABLE : SCREEN_STATE_STARTING);
}

void screen_status_init(screen_info_mode_t mode, const char *receiver_name) {
    g_mutex_lock(&status_mutex);
    memset(&status, 0, sizeof(status));
    status.mode = valid_mode(mode) ? mode : SCREEN_INFO_OFF;
    status.state = SCREEN_STATE_STARTING;
    status.generation = ++next_generation;
    status.revision = 1;
    status.changed_at_us = g_get_monotonic_time();
    status.progress.buffer_percent = -1;
    safe_label(status.receiver_name, sizeof(status.receiver_name), receiver_name, 1024);
    if (!status.receiver_name[0]) g_strlcpy(status.receiver_name, "AirPlay receiver", sizeof(status.receiver_name));
    was_ready = false;
    interrupted_state = SCREEN_STATE_STARTING;
    g_mutex_unlock(&status_mutex);
}

void screen_status_set_mode(screen_info_mode_t mode) {
    if (!valid_mode(mode)) return;
    g_mutex_lock(&status_mutex);
    if (status.mode != mode) { status.mode = mode; status.revision++; }
    g_mutex_unlock(&status_mutex);
}

screen_info_mode_t screen_status_get_mode(void) {
    g_mutex_lock(&status_mutex);
    screen_info_mode_t mode = status.mode;
    g_mutex_unlock(&status_mutex);
    return mode;
}

bool screen_status_parse_mode(const char *value, screen_info_mode_t *mode) {
    if (!value || !mode) return false;
    if (!strcmp(value, "off")) *mode = SCREEN_INFO_OFF;
    else if (!strcmp(value, "status")) *mode = SCREEN_INFO_STATUS;
    else if (!strcmp(value, "debug")) *mode = SCREEN_INFO_DEBUG;
    else return false;
    return true;
}

void screen_status_set_readiness(bool network, bool listener, bool discovery,
                                 bool backend, bool output_released) {
    g_mutex_lock(&status_mutex);
    if (status.network_ready != network || status.listener_ready != listener ||
        status.discovery_ready != discovery || status.backend_ready != backend ||
        status.output_released != output_released) status.revision++;
    status.network_ready = network;
    status.listener_ready = listener;
    status.discovery_ready = discovery;
    status.backend_ready = backend;
    status.output_released = output_released;
    if (status.session_active && status.state != SCREEN_STATE_FAILED &&
        status.state != SCREEN_STATE_RECOVERY_REQUIRED) {
        if (!receiver_ready()) {
            if (status.state != SCREEN_STATE_UNAVAILABLE) interrupted_state = status.state;
            change_state(SCREEN_STATE_UNAVAILABLE);
        } else if (status.state == SCREEN_STATE_UNAVAILABLE) change_state(interrupted_state);
    }
    update_idle();
    g_mutex_unlock(&status_mutex);
}

uint64_t screen_status_begin_session(screen_session_kind_t kind,
                                     screen_source_route_t route, const char *sender) {
    if (kind <= SCREEN_SESSION_NONE || kind > SCREEN_SESSION_AUDIO_ONLY ||
        route < SCREEN_ROUTE_UNKNOWN || route > SCREEN_ROUTE_RTP) return 0;
    g_mutex_lock(&status_mutex);
    status.generation = ++next_generation;
    status.kind = kind;
    status.route = route;
    status.session_active = true;
    status.output_observed = false;
    status.pause_requested = false;
    status.error = SCREEN_ERROR_NONE;
    status.error_detail[0] = '\0';
    status.requested_at_us = g_get_monotonic_time();
    status.last_progress_at_us = 0;
    status.history_count = 0;
    memset(&status.video, 0, sizeof(status.video));
    memset(&status.audio, 0, sizeof(status.audio));
    memset(&status.progress, 0, sizeof(status.progress));
    status.progress.buffer_percent = -1;
    safe_label(status.sender, sizeof(status.sender), sender, 1024);
    /* Record the request even if the previous session was also Incoming. */
    status.state = SCREEN_STATE_STARTING;
    change_state(SCREEN_STATE_INCOMING);
    uint64_t generation = status.generation;
    g_mutex_unlock(&status_mutex);
    return generation;
}

static bool current_session(uint64_t generation) {
    return generation != 0 && generation == status.generation;
}

bool screen_status_event(uint64_t generation, screen_status_event_t event) {
    if (event < SCREEN_EVENT_PREPARING || event > SCREEN_EVENT_RECOVERY_REQUIRED) return false;
    g_mutex_lock(&status_mutex);
    bool accepted = current_session(generation);
    if (!accepted) goto done;
    if (event == SCREEN_EVENT_RECOVERY_REQUIRED) {
        change_state(SCREEN_STATE_RECOVERY_REQUIRED);
        status.output_released = false;
        goto done;
    }
    if (event == SCREEN_EVENT_STOPPED) {
        status.session_active = false;
        status.pause_requested = false;
        status.revision++;
        /* Stopping is not evidence that the output has been released. */
        update_idle();
        goto done;
    }
    if (event == SCREEN_EVENT_RECOVERED) {
        if (!status.output_released || !receiver_ready()) { accepted = false; goto done; }
        status.session_active = false;
        status.pause_requested = false;
        status.error = SCREEN_ERROR_NONE;
        status.error_detail[0] = '\0';
        change_state(SCREEN_STATE_STARTING);
        update_idle();
        goto done;
    }
    if (!status.session_active || status.state == SCREEN_STATE_FAILED ||
        status.state == SCREEN_STATE_RECOVERY_REQUIRED || status.state == SCREEN_STATE_UNAVAILABLE) {
        accepted = false;
        goto done;
    }
    if ((status.state == SCREEN_STATE_STOPPING || status.state == SCREEN_STATE_SWITCHING) &&
        event != SCREEN_EVENT_STOPPING && event != SCREEN_EVENT_SWITCHING) {
        accepted = false;
        goto done;
    }
    switch (event) {
    case SCREEN_EVENT_PREPARING: change_state(status.pause_requested ? SCREEN_STATE_PAUSED : SCREEN_STATE_PREPARING); break;
    case SCREEN_EVENT_OPENING: change_state(status.pause_requested ? SCREEN_STATE_PAUSED : SCREEN_STATE_OPENING); break;
    case SCREEN_EVENT_BUFFERING: change_state(status.pause_requested ? SCREEN_STATE_PAUSED : SCREEN_STATE_BUFFERING); break;
    case SCREEN_EVENT_WAITING_DATA: change_state(status.pause_requested ? SCREEN_STATE_PAUSED : SCREEN_STATE_WAITING_DATA); break;
    case SCREEN_EVENT_OUTPUT_PROGRESS:
        status.output_observed = true;
        status.last_progress_at_us = g_get_monotonic_time();
        status.revision++;
        if (!status.pause_requested && status.state != SCREEN_STATE_SEEKING) change_state(SCREEN_STATE_PLAYING);
        break;
    case SCREEN_EVENT_PAUSED: status.pause_requested = true; change_state(SCREEN_STATE_PAUSED); break;
    case SCREEN_EVENT_RESUMED:
        status.pause_requested = false;
        change_state(SCREEN_STATE_WAITING_DATA);
        break;
    case SCREEN_EVENT_SEEKING: change_state(SCREEN_STATE_SEEKING); break;
    case SCREEN_EVENT_SEEK_COMPLETE:
        change_state(status.pause_requested ? SCREEN_STATE_PAUSED : SCREEN_STATE_WAITING_DATA); break;
    case SCREEN_EVENT_SWITCHING: change_state(SCREEN_STATE_SWITCHING); break;
    case SCREEN_EVENT_STOPPING: change_state(SCREEN_STATE_STOPPING); break;
    default: break;
    }
done:
    g_mutex_unlock(&status_mutex);
    return accepted;
}

bool screen_status_fail(uint64_t generation, screen_status_error_t error) {
    return screen_status_fail_detail(generation, error, NULL);
}

bool screen_status_fail_detail(uint64_t generation, screen_status_error_t error,
                               const char *detail) {
    if (error <= SCREEN_ERROR_NONE || error > SCREEN_ERROR_INTERNAL) return false;
    char clean[sizeof(status.error_detail)];
    safe_label(clean, sizeof(clean), detail, 1024);
    g_mutex_lock(&status_mutex);
    bool accepted = current_session(generation);
    if (accepted) {
        status.error = error;
        status.last_error = error;
        status.last_error_generation = generation;
        g_strlcpy(status.error_detail, clean, sizeof(status.error_detail));
        change_state(SCREEN_STATE_FAILED);
        status.revision++;
    }
    g_mutex_unlock(&status_mutex);
    return accepted;
}

#define SANITIZE_FIELD(copy, source, field) safe_label((copy).field, sizeof((copy).field), (source)->field, sizeof((source)->field))
bool screen_status_set_video(uint64_t generation, const screen_status_video_t *video) {
    if (!video) return false;
    screen_status_video_t clean = *video;
    SANITIZE_FIELD(clean, video, backend); SANITIZE_FIELD(clean, video, codec);
    SANITIZE_FIELD(clean, video, profile); SANITIZE_FIELD(clean, video, decode_policy);
    SANITIZE_FIELD(clean, video, decoder); SANITIZE_FIELD(clean, video, memory);
    SANITIZE_FIELD(clean, video, pixel_format);
    SANITIZE_FIELD(clean, video, overlay_reason);
    if (clean.width > 32768) clean.width = 0;
    if (clean.height > 32768) clean.height = 0;
    if (clean.output_width > 32768) clean.output_width = 0;
    if (clean.output_height > 32768) clean.output_height = 0;
    if (clean.fps_num > 1000000 || clean.fps_den > 1000000 || !clean.fps_den) clean.fps_num = clean.fps_den = 0;
    if (clean.bit_depth > 64) clean.bit_depth = 0;
    if (!clean.hardware_known) clean.hardware_active = false;
    if (!clean.overlay_known) clean.overlay_supported = false;
    if (!clean.output_dropped_known) clean.output_dropped_frames = 0;
    g_mutex_lock(&status_mutex);
    bool accepted = current_session(generation);
    if (accepted) {
        if (clean.output_buffers_known && clean.output_buffers > status.video.output_buffers)
            status.last_progress_at_us = g_get_monotonic_time();
        status.video = clean;
        status.revision++;
    }
    g_mutex_unlock(&status_mutex);
    return accepted;
}

bool screen_status_set_audio(uint64_t generation, const screen_status_audio_t *audio) {
    if (!audio) return false;
    screen_status_audio_t clean = *audio;
    SANITIZE_FIELD(clean, audio, codec); SANITIZE_FIELD(clean, audio, output);
    if (clean.channels > 64) clean.channels = 0;
    if (clean.sample_rate > 1536000) clean.sample_rate = 0;
    if (!isfinite(clean.volume) || clean.volume < 0 || clean.volume > 1000) clean.volume_known = false;
    if (!clean.volume_known) clean.volume = 0;
    g_mutex_lock(&status_mutex);
    bool accepted = current_session(generation);
    if (accepted) {
        if (clean.output_buffers_known && clean.output_buffers > status.audio.output_buffers)
            status.last_progress_at_us = g_get_monotonic_time();
        status.audio = clean;
        status.revision++;
    }
    g_mutex_unlock(&status_mutex);
    return accepted;
}
#undef SANITIZE_FIELD

bool screen_status_set_progress(uint64_t generation, const screen_status_progress_t *progress) {
    if (!progress) return false;
    screen_status_progress_t clean = *progress;
    if (!isfinite(clean.position_seconds) || clean.position_seconds < 0 || clean.position_seconds > 1e12) clean.position_known = false;
    if (!isfinite(clean.duration_seconds) || clean.duration_seconds < 0 || clean.duration_seconds > 1e12) clean.duration_known = false;
    if (clean.buffer_percent < 0 || clean.buffer_percent > 100) clean.buffer_known = false;
    if (!clean.position_known) clean.position_seconds = 0;
    if (!clean.duration_known) clean.duration_seconds = 0;
    if (!clean.buffer_known) clean.buffer_percent = -1;
    if (!clean.live_known) clean.live = false;
    if (!isfinite(clean.cache_seconds) || clean.cache_seconds < 0 || clean.cache_seconds > 1e12)
        clean.cache_seconds_known = false;
    if (!isfinite(clean.avsync) || fabs(clean.avsync) > 1e12) clean.avsync_known = false;
    if (!isfinite(clean.audio_position) || fabs(clean.audio_position) > 1e12) clean.audio_position_known = false;
    if (!clean.cache_seconds_known) clean.cache_seconds = 0;
    if (!clean.avsync_known) clean.avsync = 0;
    if (!clean.audio_position_known) clean.audio_position = 0;
    if (!clean.actual_paused_known) clean.actual_paused = false;
    if (!clean.core_idle_known) clean.core_idle = false;
    if (!clean.cache_eof_known) clean.cache_eof = false;
    if (!clean.cache_underrun_known) clean.cache_underrun = false;
    if (!clean.cache_idle_known) clean.cache_idle = false;
    if (clean.last_http_status < 100 || clean.last_http_status > 599) clean.last_http_status_known = false;
    if (!clean.last_http_status_known) clean.last_http_status = 0;
    safe_label(clean.diagnostic_stage, sizeof(clean.diagnostic_stage),
               progress->diagnostic_stage, sizeof(progress->diagnostic_stage));
    static const char *stages[] = {"video-decode", "audio-decode", "video-output",
                                  "audio-output", "demux", "source", "unclassified"};
    bool stage_known = false;
    for (size_t i = 0; i < G_N_ELEMENTS(stages); i++)
        if (!strcmp(clean.diagnostic_stage, stages[i])) stage_known = true;
    if (!stage_known) clean.diagnostic_stage[0] = '\0';
    safe_label(clean.diagnostic_reason, sizeof(clean.diagnostic_reason),
               progress->diagnostic_reason, sizeof(progress->diagnostic_reason));
    static const char *reasons[] = {"http-client-error", "http-server-error", "hls-init-failure",
        "hls-segment-failure", "hls-reload-failure", "missing-reference", "video-decode-error",
        "invalid-data", "timestamp-discontinuity", "audio-output-underrun",
        "audio-output-init-error", "audio-decode-error", "pes-size-mismatch", "packet-corrupt",
        "demux-read-error", "hls-expired-segments", "hls-sequence-change",
        "virtual-terminal-unavailable", "frame-present-failure", "drm-display-failure"};
    bool reason_known = false;
    for (size_t i = 0; i < G_N_ELEMENTS(reasons); i++)
        if (!strcmp(clean.diagnostic_reason, reasons[i])) reason_known = true;
    if (!reason_known) clean.diagnostic_reason[0] = '\0';
    safe_label(clean.last_warning_stage, sizeof(clean.last_warning_stage),
               progress->last_warning_stage, sizeof(progress->last_warning_stage));
    safe_label(clean.last_warning_reason, sizeof(clean.last_warning_reason),
               progress->last_warning_reason, sizeof(progress->last_warning_reason));
    stage_known = reason_known = false;
    for (size_t i = 0; i < G_N_ELEMENTS(stages); i++)
        if (!strcmp(clean.last_warning_stage, stages[i])) stage_known = true;
    for (size_t i = 0; i < G_N_ELEMENTS(reasons); i++)
        if (!strcmp(clean.last_warning_reason, reasons[i])) reason_known = true;
    if (!stage_known) clean.last_warning_stage[0] = '\0';
    if (!reason_known) clean.last_warning_reason[0] = '\0';
    if (!clean.packet_capture_enabled) {
        clean.packet_capture_active = clean.packet_capture_complete = false;
        clean.video_packets = clean.audio_packets = 0;
    }
    if (!clean.diagnostics_known) {
        clean.diagnostic_stage[0] = '\0';
        clean.diagnostic_reason[0] = '\0';
        clean.last_warning_stage[0] = clean.last_warning_reason[0] = '\0';
        clean.last_http_status_known = false;
        clean.last_http_status = 0;
        clean.video_decode_errors = clean.audio_decode_errors = 0;
        clean.demux_errors = clean.network_errors = 0;
        clean.video_output_errors = clean.audio_output_errors = clean.unclassified_errors = 0;
        clean.log_warnings = clean.video_decode_warnings = clean.missing_reference_warnings = 0;
        clean.invalid_data_warnings = clean.timestamp_warnings = clean.audio_output_warnings = clean.unknown_warnings = 0;
    }
    g_mutex_lock(&status_mutex);
    bool accepted = current_session(generation);
    if (accepted) { status.progress = clean; status.revision++; }
    g_mutex_unlock(&status_mutex);
    return accepted;
}

void screen_status_get_snapshot(screen_status_snapshot_t *snapshot) {
    if (!snapshot) return;
    g_mutex_lock(&status_mutex);
    *snapshot = status;
    g_mutex_unlock(&status_mutex);
}

const char *screen_status_state_name(screen_state_t state, screen_session_kind_t kind) {
    switch (state) {
    case SCREEN_STATE_STARTING: return "Starting receiver";
    case SCREEN_STATE_WAITING_NETWORK: return "Waiting for network";
    case SCREEN_STATE_READY: return "Ready to receive";
    case SCREEN_STATE_INCOMING: return "Incoming request";
    case SCREEN_STATE_PREPARING: return "Preparing stream";
    case SCREEN_STATE_OPENING: return "Opening video";
    case SCREEN_STATE_BUFFERING: return "Buffering";
    case SCREEN_STATE_WAITING_DATA: return "Waiting for playback progress";
    case SCREEN_STATE_PLAYING:
        if (kind == SCREEN_SESSION_MIRRORING) return "Screen mirroring";
        if (kind == SCREEN_SESSION_AUDIO_ONLY) return "Audio connected";
        return "Playing";
    case SCREEN_STATE_PAUSED: return "Paused";
    case SCREEN_STATE_SEEKING: return "Seeking";
    case SCREEN_STATE_SWITCHING: return "Switching streams";
    case SCREEN_STATE_STOPPING: return "Stopping";
    case SCREEN_STATE_FAILED: return "Playback failed";
    case SCREEN_STATE_UNAVAILABLE: return "Receiver unavailable";
    case SCREEN_STATE_RECOVERY_REQUIRED: return "Recovery required";
    default: return "Receiver unavailable";
    }
}

static const char *error_name(screen_status_error_t error) {
    switch (error) {
    case SCREEN_ERROR_NETWORK: return "Network unavailable";
    case SCREEN_ERROR_PLAYLIST: return "Playlist unavailable";
    case SCREEN_ERROR_SOURCE: return "Stream data unavailable";
    case SCREEN_ERROR_DECODER: return "Decoder could not open this stream";
    case SCREEN_ERROR_OUTPUT: return "Display or audio output unavailable";
    case SCREEN_ERROR_BACKEND: return "Playback backend failed";
    case SCREEN_ERROR_TIMEOUT: return "Playback did not make progress";
    case SCREEN_ERROR_INTERNAL: return "Receiver could not complete playback";
    default: return "Unknown";
    }
}

static const char *known(const char *value) { return value[0] ? value : "Unknown"; }
static const char *observed_flag(bool reported, bool value) {
    return reported ? (value ? "yes" : "no") : "Unknown";
}

size_t screen_status_format(const screen_status_snapshot_t *s, char *text, size_t capacity, bool compact) {
    if (!text || !capacity) return 0;
    text[0] = '\0';
    if (!s || s->mode == SCREEN_INFO_OFF) return 0;
    if (compact && s->mode == SCREEN_INFO_STATUS &&
        (s->state == SCREEN_STATE_PLAYING || s->state == SCREEN_STATE_READY)) return 0;
    GString *out = g_string_new(NULL);
    if (!compact) g_string_append_printf(out, "%s\n\n", known(s->receiver_name));
    g_string_append(out, screen_status_state_name(s->state, s->kind));
    double elapsed = s->requested_at_us > 0 ?
        MAX(0.0, (double)(g_get_monotonic_time() - s->requested_at_us) / G_USEC_PER_SEC) : 0;
    if (s->error != SCREEN_ERROR_NONE) g_string_append_printf(out, "\n%s [E%d]", error_name(s->error), s->error);
    if (s->error != SCREEN_ERROR_NONE && s->error_detail[0])
        g_string_append_printf(out, "\n%s", s->error_detail);
    if (s->mode == SCREEN_INFO_STATUS) {
        if (s->state == SCREEN_STATE_READY) {
            g_string_append_printf(out, "\n\nChoose %s in your app's AirPlay menu.", known(s->receiver_name));
        } else if (s->session_active && s->state != SCREEN_STATE_PLAYING && s->state != SCREEN_STATE_PAUSED) {
            if (s->state == SCREEN_STATE_BUFFERING && s->progress.buffer_known)
                g_string_append_printf(out, "\n%d%% buffered", s->progress.buffer_percent);
            else g_string_append_printf(out, "\n%.0f seconds since request", elapsed);
        }
    } else {
        bool mpv = !strcmp(s->video.backend, "mpv");
        const char *kind = s->kind == SCREEN_SESSION_DIRECT_VIDEO ? "AirPlay video" :
                           s->kind == SCREEN_SESSION_MIRRORING ? "Mirroring" :
                           s->kind == SCREEN_SESSION_AUDIO_ONLY ? "Audio only" : "Receiver";
        const char *route = s->route == SCREEN_ROUTE_DIRECT_HTTP ? "direct HTTP" :
                            s->route == SCREEN_ROUTE_PLAYLIST_CACHE ? "cached HLS" :
                            s->route == SCREEN_ROUTE_RTP ? "RTP" : "Unknown route";
        g_string_append_printf(out, " | %s | %s | session %" G_GUINT64_FORMAT " | %.1fs\n",
                               kind, route, s->generation, elapsed);
        if (s->sender[0]) g_string_append_printf(out, "Sender: %s\n", s->sender);
        g_string_append_printf(out, mpv ? "Video info: %s %s | " : "Video: %s %s | ",
                               known(s->video.codec), known(s->video.profile));
        if (s->video.width && s->video.height) g_string_append_printf(out, "%ux%u", s->video.width, s->video.height);
        else g_string_append(out, "Detecting size");
        if (s->video.fps_den) g_string_append_printf(out, " %.2f fps", (double)s->video.fps_num / s->video.fps_den);
        else g_string_append(out, " | FPS Unknown");
        if (s->video.bit_depth) g_string_append_printf(out, " %u-bit", s->video.bit_depth);
        if (mpv) g_string_append_printf(out, " | %s", known(s->video.pixel_format));
        g_string_append_printf(out, "\nDecode: %s | requested %s | %s (%s)",
                               known(s->video.backend), known(s->video.decode_policy), known(s->video.decoder),
                               s->video.hardware_known ? (s->video.hardware_active ? "hardware" : "software") : "Unknown path");
        if (!mpv) g_string_append_printf(out, " | %s", known(s->video.memory));
        else g_string_append_printf(out, " | decoded params %s", s->video.decoded_parameters_known ? "available" : "Unknown");
        g_string_append(out, "\nProgress: ");
        if (mpv) {
            if (s->progress.position_known) g_string_append_printf(out, "position %.1fs", s->progress.position_seconds);
            else g_string_append(out, "position Unknown");
            g_string_append(out, " | cache ");
            if (s->progress.cache_seconds_known) g_string_append_printf(out, "%.1fs", s->progress.cache_seconds);
            else g_string_append(out, "Unknown");
            if (s->progress.buffer_known) g_string_append_printf(out, " | buffer %d%%", s->progress.buffer_percent);
        } else {
            if (s->progress.position_known) g_string_append_printf(out, "position %.1fs | ", s->progress.position_seconds);
            if (s->video.output_buffers_known) g_string_append_printf(out, "%" G_GUINT64_FORMAT " video sink buffers", s->video.output_buffers);
            else g_string_append(out, "video sink buffers Unknown");
            g_string_append(out, " | dropped ");
            if (s->video.dropped_known) g_string_append_printf(out, "%" G_GUINT64_FORMAT, s->video.dropped_frames);
            else g_string_append(out, "Unknown");
            if (s->progress.buffer_known) g_string_append_printf(out, " | buffer %d%%", s->progress.buffer_percent);
            else g_string_append(out, " | buffer Unknown");
        }
        if (s->last_progress_at_us) g_string_append_printf(out, mpv ? " | last progress %.1fs ago" : " | last output %.1fs ago",
            MAX(0.0, (double)(g_get_monotonic_time() - s->last_progress_at_us) / G_USEC_PER_SEC));
        if (mpv) {
            g_string_append_printf(out, "\nPlayer: pause %s | core idle %s | cache EOF %s | underrun %s | cache idle %s",
                observed_flag(s->progress.actual_paused_known, s->progress.actual_paused),
                observed_flag(s->progress.core_idle_known, s->progress.core_idle),
                observed_flag(s->progress.cache_eof_known, s->progress.cache_eof),
                observed_flag(s->progress.cache_underrun_known, s->progress.cache_underrun),
                observed_flag(s->progress.cache_idle_known, s->progress.cache_idle));
        }
        g_string_append_printf(out, "\nAudio: %s | ", known(s->audio.codec));
        if (s->audio.sample_rate) g_string_append_printf(out, "%u Hz", s->audio.sample_rate);
        else g_string_append(out, "rate Unknown");
        if (s->audio.channels) g_string_append_printf(out, " %u ch", s->audio.channels);
        if (!mpv) {
            if (s->audio.output_buffers_known) g_string_append_printf(out, " | %" G_GUINT64_FORMAT " sink buffers", s->audio.output_buffers);
            else g_string_append(out, " | sink buffers Unknown");
        }
        g_string_append_printf(out, " | %s", known(s->audio.output));
        if (s->audio.volume_known) g_string_append_printf(out, " | %s%.2f", s->audio.muted ? "muted " : "volume ", s->audio.volume);
        if (mpv) {
            g_string_append(out, " | PTS ");
            if (s->progress.audio_position_known) g_string_append_printf(out, "%.1fs", s->progress.audio_position);
            else g_string_append(out, "Unknown");
            g_string_append(out, " | AV sync ");
            if (s->progress.avsync_known) g_string_append_printf(out, "%+.3fs", s->progress.avsync);
            else g_string_append(out, "Unknown");
            g_string_append(out, "\nDecoder drops: ");
            if (s->video.dropped_known) g_string_append_printf(out, "%" G_GUINT64_FORMAT, s->video.dropped_frames);
            else g_string_append(out, "Unknown");
            g_string_append(out, " | VO drops: ");
            if (s->video.output_dropped_known) g_string_append_printf(out, "%" G_GUINT64_FORMAT, s->video.output_dropped_frames);
            else g_string_append(out, "Unknown");
            if (s->progress.diagnostics_known) {
                if (s->progress.packet_capture_enabled)
                    g_string_append_printf(out, "\nStartup packets: video %" G_GUINT64_FORMAT
                        " | audio %" G_GUINT64_FORMAT " | capture %s",
                        s->progress.video_packets, s->progress.audio_packets,
                        s->progress.packet_capture_active ? "collecting" :
                        s->progress.packet_capture_complete ? "complete" : "incomplete");
                g_string_append_printf(out, "\nError reports: Vdec %" G_GUINT64_FORMAT " Adec %" G_GUINT64_FORMAT
                    " Demux %" G_GUINT64_FORMAT " Net %" G_GUINT64_FORMAT " Vout %" G_GUINT64_FORMAT
                    " Aout %" G_GUINT64_FORMAT " Other %" G_GUINT64_FORMAT,
                    s->progress.video_decode_errors, s->progress.audio_decode_errors,
                    s->progress.demux_errors, s->progress.network_errors,
                    s->progress.video_output_errors, s->progress.audio_output_errors, s->progress.unclassified_errors);
                if (s->progress.diagnostic_stage[0]) g_string_append_printf(out, " | last %s", s->progress.diagnostic_stage);
                g_string_append_printf(out, "\nWarning reports: %" G_GUINT64_FORMAT " | refs %" G_GUINT64_FORMAT
                    " invalid %" G_GUINT64_FORMAT " timestamps %" G_GUINT64_FORMAT,
                    s->progress.log_warnings, s->progress.missing_reference_warnings,
                    s->progress.invalid_data_warnings, s->progress.timestamp_warnings);
                g_string_append_printf(out, "\nLast reason: %s | HTTP ", known(s->progress.diagnostic_reason));
                if (s->progress.last_http_status_known) g_string_append_printf(out, "%d", s->progress.last_http_status);
                else g_string_append(out, "Unknown");
                if (s->progress.log_warnings)
                    g_string_append_printf(out, "\nLast warning: %s | %s",
                        known(s->progress.last_warning_stage), known(s->progress.last_warning_reason));
            } else g_string_append(out, "\nError reports unavailable\nWarning reports unavailable");
        }
        g_string_append_printf(out, "\nOverlay: %s", s->video.overlay_known ?
            (s->video.overlay_supported ? "Available" : "Unavailable") : "Not reported");
        if (s->video.overlay_reason[0]) g_string_append_printf(out, " (%s)", s->video.overlay_reason);
        if (!compact && s->last_error != SCREEN_ERROR_NONE)
            g_string_append_printf(out, "\nLast error: E%d, session %" G_GUINT64_FORMAT " - %s",
                                   s->last_error, s->last_error_generation, error_name(s->last_error));
        if (!compact && s->history_count) {
            unsigned first = s->history_count > 3 ? s->history_count - 3 : 0;
            g_string_append(out, "\nRecent:");
            for (unsigned i = first; i < s->history_count; i++)
                g_string_append_printf(out, " %.1fs %s%s", s->history[i].elapsed_ms / 1000.0,
                    screen_status_state_name(s->history[i].state, s->kind),
                    i + 1 < s->history_count ? ";" : "");
        }
    }
    g_strlcpy(text, out->str, capacity);
    /* A caller's short destination must still contain valid UTF-8. */
    const char *invalid = NULL;
    if (!g_utf8_validate(text, -1, &invalid)) text[invalid - text] = '\0';
    size_t stored = strlen(text);
    g_string_free(out, TRUE);
    return stored;
}
