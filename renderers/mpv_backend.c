/* Supervised mpv JSON IPC adapter. SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "mpv_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <json-c/json.h>

#if defined(UXPLAY_MPV_HAVE_SPAWN_CLOSEFROM)
#define MPV_SPAWN_CLOSEFROM 1
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
#define MPV_SPAWN_CLOSEFROM 1
#endif
#endif

extern char **environ;

#define URL_LIMIT 8192
#define LINE_LIMIT 65536
#define TX_LIMIT 131072
#define REQUEST_LIMIT 96
#define READ_BUDGET 65536
#define OPTION_LIMIT 512
#define TERMINAL_DIAGNOSTIC_DRAIN_MS 200
#define PACKET_CAPTURE_MARKER "uxplay-packet-capture-boundary-v1"

enum request_kind {
    R_VERSION, R_COMMANDS, R_PROPERTIES, R_REQUIRED_OBSERVE, R_CHECK_PAUSE, R_OBSERVE, R_LOAD,
    R_PAUSE, R_SEEK, R_INITIAL_SEEK, R_VOLUME, R_OSD, R_QUIT, R_LOGS,
    R_PACKET_RESTORE, R_PACKET_WARN, R_PACKET_MARKER, R_PACKET_FINAL
};

struct request {
    int64_t id;
    enum request_kind kind;
    uint64_t seek_serial;
    uint64_t queued_at;
};

struct mpv_backend_s {
    pthread_mutex_t lock;
    mpv_backend_snapshot_t snapshot;
    mpv_backend_snapshot_t retiring;
    bool retiring_valid;
    mpv_backend_snapshot_t terminal_reports[4];
    size_t terminal_first, terminal_count;
    uint64_t terminal_reports_dropped;
    char executable[PATH_MAX];
    char video_output[OPTION_LIMIT];
    char gpu_context[OPTION_LIMIT];
    char gpu_api[OPTION_LIMIT];
    char drm_device[OPTION_LIMIT];
    char drm_connector[OPTION_LIMIT];
    char audio_device[OPTION_LIMIT];
    char h264_hwdec[64];
    bool disable_audio;
    bool fast_rendering;
    bool packet_diagnostics, packet_logs_acked, packet_restore_queued;
    bool packet_marker_pending, packet_marker_seen, packet_final_queued;
    uint64_t packet_marker_deadline;
    unsigned packet_window_ms;
    mpv_decode_policy_t decode_policy;
    unsigned startup_ms, load_ms, stop_ms;
    pid_t pid;
    int fd;
    uint64_t child_generation;
    uint64_t started_at, load_at, stop_at;
    uint64_t diagnostic_drain_until;
    unsigned stop_step;
    bool closing, want_session, stop_requested, ipc_ready, preflight;
    unsigned handshake_pending;
    bool pause_dirty, seek_dirty, volume_dirty, osd_dirty, initial_seek;
    double seek_seconds, volume;
    uint64_t seek_serial, seek_inflight, seek_started_at;
    bool seek_acked, seek_event, seek_restarted, inflight_initial_seek;
    bool mute;
    char osd[2049];
    char url[URL_LIMIT + 1];
    char rx[LINE_LIMIT + 1];
    size_t rx_size;
    char tx[TX_LIMIT];
    size_t tx_size, tx_sent;
    struct request requests[REQUEST_LIMIT];
    size_t request_count;
    int64_t next_request;
};

/* Observation IDs are fixed only inside a child. Its socket and request IDs
 * never survive replacement. Compare generation before accepting any event. */
static const char *const properties[] = {
    "time-pos", "duration", "pause", "paused-for-cache", "seekable",
    "demuxer-cache-state", "video-params", "container-fps", "video-bitrate",
    "audio-bitrate", "audio-params", "decoder-frame-drop-count", "current-tracks/video/codec",
    "current-tracks/video/decoder", "current-tracks/audio/codec", "hwdec-current", "current-vo", "current-ao",
    "current-tracks/video/codec-profile", "current-tracks/audio/decoder", "frame-drop-count",
    "cache-buffering-state", "avsync", "audio-pts", "core-idle", "track-list", "video-dec-params"
};
#define PROPERTY_COUNT (sizeof(properties) / sizeof(properties[0]))

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void copy_text(char *dst, size_t size, const char *src)
{
    if (dst && size) snprintf(dst, size, "%s", src ? src : "");
}

static void observation_ages(mpv_backend_snapshot_t *s)
{
    uint64_t now = now_ms();
    s->video_observation_age_ms = s->video_observed_at_ms ? (int64_t)(now - s->video_observed_at_ms) : -1;
    s->audio_observation_age_ms = s->audio_observed_at_ms ? (int64_t)(now - s->audio_observed_at_ms) : -1;
    s->cache_observation_age_ms = s->cache_observed_at_ms ? (int64_t)(now - s->cache_observed_at_ms) : -1;
    /* A read-budget yield leaves unread bytes queued; it is load evidence,
     * unlike the actual overflow/rejected-record counters below. */
    if (s->log_overflows || s->event_overflows ||
        s->packet_log_rejected || s->packet_capture_errors)
        s->packet_diagnostics_complete = false;
}

static bool option_copy(char *dst, size_t size, const char *src)
{
    if (!src) src = "";
    if (strnlen(src, size) >= size) return false;
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    copy_text(dst, size, src);
    return true;
}

static bool executable_path(const char *name, char *out, size_t size)
{
    if (!name || !*name) name = "mpv";
    if (strchr(name, '/')) {
        char resolved[PATH_MAX];
        struct stat st;
        if (!realpath(name, resolved) || access(resolved, X_OK) ||
            stat(resolved, &st) || !S_ISREG(st.st_mode)) return false;
        return option_copy(out, size, resolved);
    }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    for (const char *p = path; ; ) {
        const char *end = strchr(p, ':');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char candidate[PATH_MAX];
        /* Do not search implicit current-directory entries. */
        if (n && n + strlen(name) + 2 < sizeof(candidate)) {
            memcpy(candidate, p, n);
            candidate[n] = '/';
            strcpy(candidate + n + 1, name);
            if (executable_path(candidate, out, size)) return true;
        }
        if (!end) return false;
        p = end + 1;
    }
}

static bool valid_time(double value)
{
    return isfinite(value) && value >= 0 && value <= 1e12;
}

static bool valid_url(const char *url)
{
    if (!url || strnlen(url, URL_LIMIT + 1) > URL_LIMIT) return false;
    size_t prefix = !strncmp(url, "http://", 7) ? 7 :
                    !strncmp(url, "https://", 8) ? 8 : 0;
    if (!prefix || !url[prefix] || url[prefix] == '/') return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}

mpv_backend_t *mpv_backend_create(const mpv_backend_config_t *config,
                                  char *error, size_t error_size)
{
    mpv_backend_config_t defaults = {0};
    if (!config) config = &defaults;
    copy_text(error, error_size, "");
#if !defined(POSIX_SPAWN_CLOEXEC_DEFAULT) && !defined(MPV_SPAWN_CLOSEFROM)
    copy_text(error, error_size, "mpv requires spawn close-from support or close-on-exec by default");
    return NULL;
#endif
    if (config->decode_policy == MPV_DECODE_PI4_HEVC_EXPERIMENTAL) {
        copy_text(error, error_size, "mpv HEVC hardware playback has not been qualified");
        return NULL;
    }
    if (config->decode_policy != MPV_DECODE_SOFTWARE &&
        config->decode_policy != MPV_DECODE_PI4_SAFE) {
        copy_text(error, error_size, "Invalid mpv decoding policy");
        return NULL;
    }
    if (config->decode_policy == MPV_DECODE_PI4_SAFE &&
        (!config->qualified_h264_hwdec ||
         (strcmp(config->qualified_h264_hwdec, "v4l2m2m-copy") &&
          strcmp(config->qualified_h264_hwdec, "v4l2m2m")))) {
        copy_text(error, error_size, "mpv pi4-safe requires an explicitly qualified H.264 decoder");
        return NULL;
    }
    mpv_backend_t *b = calloc(1, sizeof(*b));
    if (!b) {
        copy_text(error, error_size, "Cannot allocate mpv backend");
        return NULL;
    }
    b->fd = -1;
    if (!executable_path(config->executable, b->executable, sizeof(b->executable))) {
        copy_text(error, error_size, "mpv executable is missing or not executable");
        free(b);
        return NULL;
    }
#define COPY_OPTION(field, input) \
    if (!option_copy(b->field, sizeof(b->field), config->input)) goto bad_option
    COPY_OPTION(video_output, video_output);
    COPY_OPTION(gpu_context, gpu_context);
    COPY_OPTION(gpu_api, gpu_api);
    COPY_OPTION(drm_device, drm_device);
    COPY_OPTION(drm_connector, drm_connector);
    COPY_OPTION(audio_device, audio_device);
    COPY_OPTION(h264_hwdec, qualified_h264_hwdec);
#undef COPY_OPTION
    b->decode_policy = config->decode_policy;
    b->disable_audio = config->disable_audio;
    b->fast_rendering = config->fast_rendering;
    b->packet_diagnostics = config->packet_diagnostics;
    b->packet_window_ms = config->packet_diagnostic_window_ms ? config->packet_diagnostic_window_ms : 30000;
    b->startup_ms = config->startup_timeout_ms ? config->startup_timeout_ms : 3000;
    b->load_ms = config->load_timeout_ms ? config->load_timeout_ms : 30000;
    b->stop_ms = config->stop_timeout_ms ? config->stop_timeout_ms : 1000;
    if (b->startup_ms > 120000 || b->load_ms > 300000 || b->stop_ms > 10000 || b->packet_window_ms > 30000)
        goto bad_option;
    b->volume = 100;
    b->snapshot.dropped_frames = b->snapshot.output_dropped_frames = -1;
    b->snapshot.selected_video_id = b->snapshot.selected_audio_id = -1;
    b->next_request = 1;
    if (pthread_mutex_init(&b->lock, NULL)) {
        copy_text(error, error_size, "Cannot initialize mpv backend lock");
        free(b);
        return NULL;
    }
    return b;
bad_option:
    copy_text(error, error_size, "Invalid or oversized mpv option");
    free(b);
    return NULL;
}

static bool current_session(mpv_backend_t *b, uint64_t generation)
{
    return !b->closing && b->want_session && generation == b->snapshot.generation;
}

bool mpv_backend_open(mpv_backend_t *b, uint64_t generation,
                      const char *url, double start_seconds)
{
    if (!b || !generation || !valid_url(url) || !valid_time(start_seconds)) return false;
    pthread_mutex_lock(&b->lock);
    bool ok = !b->closing && !b->snapshot.recovery_required &&
        generation > b->snapshot.generation;
    if (ok) {
        if (b->pid && b->child_generation == b->snapshot.generation) {
            b->retiring = b->snapshot;
            b->retiring_valid = true;
        }
        memset(&b->snapshot, 0, sizeof(b->snapshot));
        b->snapshot.dropped_frames = b->snapshot.output_dropped_frames = -1;
        b->snapshot.selected_video_id = b->snapshot.selected_audio_id = -1;
        b->diagnostic_drain_until = 0;
        b->snapshot.generation = generation;
        b->snapshot.state = MPV_BACKEND_STARTING;
        b->snapshot.child_alive = b->pid > 0;
        b->snapshot.child_pid = (int)b->pid;
        b->snapshot.child_generation = b->pid > 0 ? b->child_generation : 0;
        copy_text(b->url, sizeof(b->url), url);
        b->want_session = true;
        b->stop_requested = b->pid > 0;
        b->seek_seconds = start_seconds;
        b->seek_dirty = start_seconds > 0;
        b->initial_seek = start_seconds > 0;
        b->snapshot.seeking = b->seek_dirty;
        b->seek_serial = b->seek_dirty ? 1 : 0;
        b->seek_inflight = 0;
        b->pause_dirty = b->volume_dirty = true;
        b->osd[0] = 0;
        b->osd_dirty = false;
    }
    pthread_mutex_unlock(&b->lock);
    return ok;
}

static bool set_pause(mpv_backend_t *b, uint64_t generation, bool pause)
{
    if (!b) return false;
    pthread_mutex_lock(&b->lock);
    bool ok = current_session(b, generation);
    if (ok) {
        b->snapshot.requested_paused = pause;
        b->pause_dirty = true;
    }
    pthread_mutex_unlock(&b->lock);
    return ok;
}

bool mpv_backend_pause(mpv_backend_t *b, uint64_t generation)
{ return set_pause(b, generation, true); }

bool mpv_backend_resume(mpv_backend_t *b, uint64_t generation)
{ return set_pause(b, generation, false); }

bool mpv_backend_seek(mpv_backend_t *b, uint64_t generation, double seconds)
{
    if (!b || !valid_time(seconds)) return false;
    pthread_mutex_lock(&b->lock);
    bool ok = current_session(b, generation) && b->seek_serial < UINT64_MAX &&
        (!b->snapshot.ready || !b->snapshot.seekable_known || b->snapshot.seekable);
    if (ok) {
        b->seek_seconds = seconds;
        b->seek_dirty = true;
        b->initial_seek = false;
        ++b->seek_serial;
        b->snapshot.seeking = true;
        b->snapshot.control_error[0] = 0;
    }
    pthread_mutex_unlock(&b->lock);
    return ok;
}

bool mpv_backend_set_volume(mpv_backend_t *b, uint64_t generation,
                            double volume, bool mute)
{
    if (!b || !isfinite(volume) || volume < 0 || volume > 100) return false;
    pthread_mutex_lock(&b->lock);
    bool ok = !b->closing && generation == b->snapshot.generation;
    if (ok) {
        b->volume = volume;
        b->mute = mute;
        b->volume_dirty = true;
    }
    pthread_mutex_unlock(&b->lock);
    return ok;
}

static void copy_osd_utf8(char *out, const char *text, size_t length)
{
    size_t i = 0, written = 0;
    while (i < length) {
        const unsigned char *p = (const unsigned char *)text + i;
        size_t n = 1;
        uint32_t cp = p[0];
        bool valid = true;
        if (p[0] >= 0xc2 && p[0] <= 0xdf) { n = 2; cp = p[0] & 0x1f; }
        else if (p[0] >= 0xe0 && p[0] <= 0xef) { n = 3; cp = p[0] & 0x0f; }
        else if (p[0] >= 0xf0 && p[0] <= 0xf4) { n = 4; cp = p[0] & 0x07; }
        else if (p[0] >= 0x80) valid = false;
        if (n > length - i) valid = false;
        for (size_t j = 1; valid && j < n; ++j) {
            if ((p[j] & 0xc0) != 0x80) valid = false;
            else cp = (cp << 6) | (p[j] & 0x3f);
        }
        if (valid && ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
                      (n == 4 && cp < 0x10000) || cp > 0x10ffff ||
                      (cp >= 0xd800 && cp <= 0xdfff))) valid = false;
        if (!valid) {
            out[written++] = '?';
            ++i;
            continue;
        }
        /* mpv's opaque OSD control bytes are not valid UTF-8. Also strip
         * terminal/C1 controls and explicit bidi overrides from plain labels.
         * Natural RTL text, combining marks, joiners and emoji remain intact. */
        bool control = (cp < 0x20 && cp != '\n') || (cp >= 0x7f && cp <= 0x9f) ||
            (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069) ||
            cp == 0x200e || cp == 0x200f;
        if (!control) {
            memcpy(out + written, p, n);
            written += n;
        }
        i += n;
    }
    out[written] = 0; /* Output cannot be longer than the bounded input. */
}

bool mpv_backend_set_osd(mpv_backend_t *b, uint64_t generation, const char *text)
{
    if (!b || !text) return false;
    size_t length = strnlen(text, 2049);
    if (length > 2048) return false;
    pthread_mutex_lock(&b->lock);
    bool ok = current_session(b, generation);
    if (ok) {
        copy_osd_utf8(b->osd, text, length);
        b->osd_dirty = true;
    }
    pthread_mutex_unlock(&b->lock);
    return ok;
}

bool mpv_backend_stop(mpv_backend_t *b, uint64_t generation)
{
    if (!b) return false;
    pthread_mutex_lock(&b->lock);
    bool ok = generation == b->snapshot.generation;
    if (ok) {
        b->want_session = false;
        b->stop_requested = b->pid > 0;
        memset(b->url, 0, sizeof(b->url));
        b->snapshot.ready = false;
        b->snapshot.seeking = false;
        b->seek_dirty = false;
        b->seek_inflight = 0;
        b->snapshot.state = b->pid > 0 ? MPV_BACKEND_STOPPING : MPV_BACKEND_IDLE;
    }
    pthread_mutex_unlock(&b->lock);
    return ok;
}

static void close_ipc(mpv_backend_t *b)
{
    if (b->fd >= 0) close(b->fd);
    b->fd = -1;
    b->diagnostic_drain_until = 0;
    b->snapshot.terminal_diagnostics_draining = false;
    b->rx_size = b->tx_size = b->tx_sent = b->request_count = 0;
    memset(b->rx, 0, sizeof(b->rx));
    memset(b->tx, 0, sizeof(b->tx));
}

static void fail_as(mpv_backend_t *b, mpv_backend_error_t code,
                    const char *stage, const char *message)
{
    /* Keep the first failure through shutdown/IPC EOF; later cleanup cannot
     * replace a useful end-file diagnosis with a generic channel failure. */
    if (b->snapshot.error_code == MPV_BACKEND_ERROR_NONE) {
        b->snapshot.error_code = code;
        copy_text(b->snapshot.failure_stage, sizeof(b->snapshot.failure_stage), stage);
        copy_text(b->snapshot.error, sizeof(b->snapshot.error), message);
    }
    b->snapshot.state = MPV_BACKEND_FAILED;
    b->snapshot.ready = false;
    b->snapshot.seeking = false;
    b->seek_dirty = false;
    b->seek_inflight = 0;
    b->want_session = false;
    b->stop_requested = b->pid > 0;
    memset(b->url, 0, sizeof(b->url));
    /* EOF is also mpv's parent-death mechanism. Do not continue a partial
     * control stream or replay media after an IPC failure. */
    if (!b->diagnostic_drain_until) close_ipc(b);
    else {
        /* Keep only the receive side briefly for queued terminal diagnostics.
         * No queued control/media command may escape after terminal failure. */
        b->tx_size = b->tx_sent = b->request_count = 0;
        memset(b->tx, 0, sizeof(b->tx));
    }
}

static void fail(mpv_backend_t *b, const char *message)
{
    b->diagnostic_drain_until = 0; /* Invalid/failed IPC is never kept alive. */
    fail_as(b, MPV_BACKEND_ERROR_IPC, "ipc", message);
}

static bool queue_command_maybe(mpv_backend_t *b, enum request_kind kind,
                                struct json_object *command, bool required)
{
    if (!command) { if (required) fail(b, "Cannot allocate mpv control message"); return false; }
    if (b->request_count >= REQUEST_LIMIT || b->next_request == INT64_MAX) {
        json_object_put(command);
        if (required) fail(b, "mpv control queue exceeded its limit");
        return false;
    }
    struct json_object *obj = json_object_new_object();
    if (!obj) {
        json_object_put(command);
        if (required) fail(b, "Cannot allocate mpv control message");
        return false;
    }
    int64_t id = b->next_request++;
    json_object_object_add(obj, "command", command);
    json_object_object_add(obj, "request_id", json_object_new_int64(id));
    const char *wire = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
    size_t n = strlen(wire);
    if (b->tx_sent) {
        memmove(b->tx, b->tx + b->tx_sent, b->tx_size - b->tx_sent);
        b->tx_size -= b->tx_sent;
        b->tx_sent = 0;
    }
    if (n + 1 > TX_LIMIT - b->tx_size) {
        json_object_put(obj);
        if (required) fail(b, "mpv control queue exceeded its limit");
        return false;
    }
    memcpy(b->tx + b->tx_size, wire, n);
    b->tx_size += n;
    b->tx[b->tx_size++] = '\n';
    b->requests[b->request_count++] = (struct request){id, kind,
        kind == R_SEEK || kind == R_INITIAL_SEEK ? b->seek_inflight : 0, now_ms()};
    json_object_put(obj);
    return true;
}

static bool queue_command(mpv_backend_t *b, enum request_kind kind, struct json_object *command)
{
    return queue_command_maybe(b, kind, command, true);
}

static struct json_object *command_new(const char *name)
{
    struct json_object *a = json_object_new_array();
    if (a) json_object_array_add(a, json_object_new_string(name));
    return a;
}

static bool get_property(mpv_backend_t *b, enum request_kind kind, const char *name)
{
    struct json_object *a = command_new("get_property");
    if (a) json_object_array_add(a, json_object_new_string(name));
    return queue_command(b, kind, a);
}

static bool set_property(mpv_backend_t *b, enum request_kind kind,
                          const char *name, struct json_object *value)
{
    struct json_object *a = command_new("set_property");
    if (a) {
        json_object_array_add(a, json_object_new_string(name));
        json_object_array_add(a, value);
    } else if (value) json_object_put(value);
    return queue_command(b, kind, a);
}

static void count_one(uint64_t *count);

static void packet_request_warn(mpv_backend_t *b)
{
    struct json_object *a = command_new("request_log_messages");
    if (a) json_object_array_add(a, json_object_new_string("warn"));
    if (!queue_command_maybe(b, R_PACKET_WARN, a, false))
        count_one(&b->snapshot.packet_capture_errors);
}

static void packet_capture_failed(mpv_backend_t *b)
{
    count_one(&b->snapshot.packet_capture_errors);
    b->snapshot.packet_diagnostics_active = b->snapshot.packet_diagnostics_complete = false;
    if (!b->snapshot.packet_capture_ended_at_ms)
        b->snapshot.packet_capture_ended_at_ms = now_ms();
    b->packet_marker_pending = false;
    b->packet_marker_deadline = 0;
}

static bool packet_set_levels(mpv_backend_t *b, enum request_kind kind, const char *levels)
{
    struct json_object *a = command_new("set_property");
    if (a) {
        json_object_array_add(a, json_object_new_string("msg-level"));
        json_object_array_add(a, json_object_new_string(levels));
    }
    return queue_command_maybe(b, kind, a, false);
}

static void packet_final_levels(mpv_backend_t *b)
{
    if (b->packet_final_queued) return;
    b->packet_final_queued = true;
    if (!packet_set_levels(b, R_PACKET_FINAL, "all=warn")) {
        packet_capture_failed(b);
        packet_request_warn(b); /* Failed capture only: replacing buffer may discard logs. */
    }
}

static void stop_packet_diagnostics(mpv_backend_t *b, uint64_t now)
{
    if (b->packet_marker_pending && now >= b->packet_marker_deadline) {
        packet_capture_failed(b);
        packet_final_levels(b);
    }
    if (!b->packet_diagnostics || b->packet_restore_queued || !b->load_at ||
        now - b->snapshot.packet_capture_started_at_ms < b->packet_window_ms)
        return;
    b->packet_restore_queued = true;
    /* Keep consuming packet logs. A command acknowledgement can overtake
     * queued log events; the cplayer marker below is ordered in their FIFO. */
    if (!packet_set_levels(b, R_PACKET_RESTORE, "all=warn,cplayer=info")) {
        packet_capture_failed(b);
        packet_final_levels(b);
    }
}

static bool spawn_player(mpv_backend_t *b)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) {
        fail_as(b, MPV_BACKEND_ERROR_STARTUP, "startup", "Cannot create private mpv IPC channel");
        return false;
    }
    int parent_fd = sockets[0], child_fd = sockets[1];
    if (fcntl(parent_fd, F_SETFD, FD_CLOEXEC) ||
        fcntl(child_fd, F_SETFD, FD_CLOEXEC) ||
        fcntl(parent_fd, F_SETFL, O_NONBLOCK)) {
        close(parent_fd); close(child_fd);
        fail_as(b, MPV_BACKEND_ERROR_STARTUP, "startup", "Cannot configure private mpv IPC channel");
        return false;
    }
#ifdef SO_NOSIGPIPE
    int no_signal = 1;
    setsockopt(parent_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof(no_signal));
#endif
    char optional[9][OPTION_LIMIT + 48];
    char *argv[64];
    int argc = 0, n = 0;
    argv[argc++] = b->executable;
    argv[argc++] = "--no-config";
    argv[argc++] = "--idle=yes";
    argv[argc++] = "--pause=yes";
    argv[argc++] = "--force-window=no";
    argv[argc++] = "--fullscreen=yes";
    argv[argc++] = "--terminal=no";
    argv[argc++] = b->packet_diagnostics && !b->preflight ?
        "--msg-level=all=warn,lavf=trace" : "--msg-level=all=no";
    argv[argc++] = "--osc=no";
    argv[argc++] = "--osd-level=0";
    argv[argc++] = "--load-scripts=no";
    argv[argc++] = "--load-unsafe-playlists=no";
    argv[argc++] = "--sub-auto=no";
    argv[argc++] = "--audio-file-auto=no";
    argv[argc++] = "--ytdl=no";
    argv[argc++] = "--input-default-bindings=no";
    argv[argc++] = "--input-terminal=no";
    argv[argc++] = "--input-media-keys=no";
    argv[argc++] = "--input-cursor=no";
    argv[argc++] = "--resume-playback=no";
    argv[argc++] = "--save-position-on-quit=no";
    argv[argc++] = "--keep-open=no";
    argv[argc++] = "--audio-display=no";
    if (b->fast_rendering) argv[argc++] = "--profile=fast";
    if (b->disable_audio) argv[argc++] = "--aid=no";
    argv[argc++] = "--input-ipc-client=fd://3";
    /* Keep nested media references in the FFmpeg demuxer where the protocol
     * whitelist applies; do not expand arbitrary mpv playlist formats. */
    argv[argc++] = "--demuxer=lavf";
    argv[argc++] = "--demuxer-lavf-o=protocol_whitelist=[http,https,tcp,tls,crypto,httpproxy]";
    if (b->decode_policy == MPV_DECODE_SOFTWARE) {
        argv[argc++] = "--hwdec=no";
    } else {
        snprintf(optional[n], sizeof(optional[n]), "--hwdec=%s", b->h264_hwdec);
        argv[argc++] = optional[n++];
        argv[argc++] = "--hwdec-codecs=h264";
    }
#define ADD_OPTION(flag, field) do { \
    if (b->field[0]) { \
        snprintf(optional[n], sizeof(optional[n]), "--" flag "=%s", b->field); \
        argv[argc++] = optional[n++]; \
    } \
} while (0)
    ADD_OPTION("vo", video_output);
    ADD_OPTION("gpu-context", gpu_context);
    ADD_OPTION("gpu-api", gpu_api);
    ADD_OPTION("drm-device", drm_device);
    ADD_OPTION("drm-connector", drm_connector);
    ADD_OPTION("audio-device", audio_device);
#undef ADD_OPTION
    argv[argc] = NULL;

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int e = posix_spawn_file_actions_init(&actions);
    if (e) goto spawn_failed;
    e = posix_spawnattr_init(&attr);
    if (e) { posix_spawn_file_actions_destroy(&actions); goto spawn_failed; }
    /* Close the parent endpoint before putting the child endpoint at fd 3.
     * This order also works when the original parent endpoint is fd 3. */
    e = posix_spawn_file_actions_addclose(&actions, parent_fd);
    if (!e) e = posix_spawn_file_actions_adddup2(&actions, child_fd, 3);
    if (!e && child_fd != 3) e = posix_spawn_file_actions_addclose(&actions, child_fd);
    if (!e) e = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!e) e = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (!e) e = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    short flags = 0;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
#ifdef MPV_SPAWN_CLOSEFROM
    if (!e) e = posix_spawn_file_actions_addclosefrom_np(&actions, 4);
#endif
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGPIPE);
    flags |= POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (!e) e = posix_spawnattr_setsigmask(&attr, &empty);
    if (!e) e = posix_spawnattr_setsigdefault(&attr, &defaults);
    if (!e) e = posix_spawnattr_setflags(&attr, flags);
    pid_t pid = 0;
    if (!e) e = posix_spawn(&pid, b->executable, &actions, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (e) goto spawn_failed;
    close(child_fd);
    b->fd = parent_fd;
    b->pid = pid;
    b->child_generation = b->snapshot.generation;
    b->snapshot.child_alive = true;
    b->snapshot.child_pid = (int)b->pid;
    b->snapshot.child_generation = b->child_generation;
    b->started_at = now_ms();
    b->load_at = 0;
    b->ipc_ready = false;
    b->stop_step = 0;
    b->stop_requested = false;
    return get_property(b, R_VERSION, "mpv-version");
spawn_failed:
    close(parent_fd); close(child_fd);
    fail_as(b, MPV_BACKEND_ERROR_STARTUP, "startup", "Cannot start mpv with the selected configuration");
    return false;
}

static bool json_number(struct json_object *v, double *out)
{
    *out = 0;
    if (!v || (json_object_get_type(v) != json_type_double &&
               json_object_get_type(v) != json_type_int)) return false;
    double n = json_object_get_double(v);
    if (!isfinite(n) || fabs(n) > 1e12) return false;
    *out = n;
    return true;
}

static struct json_object *member(struct json_object *o, const char *key)
{
    struct json_object *v = NULL;
    if (o && json_object_get_type(o) == json_type_object)
        json_object_object_get_ex(o, key, &v);
    return v;
}

static bool json_string_is(struct json_object *v, const char *value)
{
    return v && json_object_get_type(v) == json_type_string &&
        json_object_get_string_len(v) == (int)strlen(value) &&
        !strcmp(json_object_get_string(v), value);
}

static bool json_count(struct json_object *v, int64_t *out)
{
    if (!v || json_object_get_type(v) != json_type_int || json_object_get_int64(v) < 0) return false;
    *out = json_object_get_int64(v);
    return true;
}

static bool json_flag(struct json_object *v, bool *out)
{
    if (!v || json_object_get_type(v) != json_type_boolean) return false;
    *out = json_object_get_boolean(v);
    return true;
}

static void track_selection(mpv_backend_snapshot_t *s, struct json_object *v)
{
    bool valid = v && json_object_get_type(v) == json_type_array && json_object_array_length(v) <= 128;
    int64_t video = -1, audio = -1;
    for (size_t i = 0; valid && i < json_object_array_length(v); ++i) {
        struct json_object *t = json_object_array_get_idx(v, i);
        bool selected;
        if (!json_flag(member(t, "selected"), &selected)) { valid = false; break; }
        if (!selected) continue;
        int64_t id;
        if (!json_count(member(t, "id"), &id)) { valid = false; break; }
        if (json_string_is(member(t, "type"), "video")) video = id;
        else if (json_string_is(member(t, "type"), "audio")) audio = id;
    }
    s->selected_video_known = s->selected_audio_known = valid;
    s->selected_video_id = valid ? video : -1;
    s->selected_audio_id = valid ? audio : -1;
}

static void stream_cache(mpv_backend_snapshot_t *s, struct json_object *value)
{
    memset(&s->cache_video, 0, sizeof(s->cache_video));
    memset(&s->cache_audio, 0, sizeof(s->cache_audio));
    struct json_object *streams = member(value, "ts-per-stream");
    if (!streams || json_object_get_type(streams) != json_type_array) return;
    size_t n = json_object_array_length(streams);
    for (size_t i = 0; i < n && i < 16; ++i) {
        struct json_object *v = json_object_array_get_idx(streams, i);
        mpv_backend_cache_stream_t *out = json_string_is(member(v, "type"), "video") ? &s->cache_video :
            json_string_is(member(v, "type"), "audio") ? &s->cache_audio : NULL;
        if (!out) continue;
        out->reader_pts_known = json_number(member(v, "reader-pts"), &out->reader_pts);
        out->cache_end_known = json_number(member(v, "cache-end"), &out->cache_end);
        out->cache_duration_known = json_number(member(v, "cache-duration"), &out->cache_duration) && out->cache_duration >= 0;
    }
}

static void metadata_text(char *out, size_t size, struct json_object *v)
{
    out[0] = 0;
    if (!v || json_object_get_type(v) != json_type_string) return;
    const char *s = json_object_get_string(v);
    /* Metadata is not a general-purpose mpv message channel. Avoid exposing
     * source URLs if a broken player supplies them as decoder properties. */
    if ((size_t)json_object_get_string_len(v) != strlen(s) || strpbrk(s, ":/@=?&\\")) return;
    size_t n = 0;
    while (*s && n + 1 < size) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 32 && c < 127) out[n++] = (char)c;
    }
    out[n] = 0;
}

static void update_state(mpv_backend_t *b)
{
    if (!b->snapshot.ready || b->stop_requested || b->stop_step || !b->want_session) return;
    b->snapshot.state = b->snapshot.requested_paused ? MPV_BACKEND_PAUSED :
        b->snapshot.buffering ? MPV_BACKEND_BUFFERING : MPV_BACKEND_PLAYING;
}

static void property_changed(mpv_backend_t *b, int index, struct json_object *v)
{
    mpv_backend_snapshot_t *s = &b->snapshot;
    double d = 0;
    bool number = json_number(v, &d);
    bool boolean = v && json_object_get_type(v) == json_type_boolean;
    switch (index) {
    case 0:
        s->position_known = number && d >= 0;
        if (s->position_known) s->position = d;
        if (!s->position_known || s->position < s->cache_start || s->position > s->cache_end)
            s->cache_range_known = false;
        break;
    case 1: s->duration_known = number && d >= 0; if (s->duration_known) s->duration = d; break;
    case 2:
        s->actual_paused_known = boolean;
        s->actual_paused = boolean && json_object_get_boolean(v);
        break; /* Observed pause is evidence; it never overwrites requested intent. */
    case 3: s->buffering = boolean && json_object_get_boolean(v); update_state(b); break;
    case 4: s->seekable_known = boolean; s->seekable = boolean && json_object_get_boolean(v); break;
    case 5: {
        s->cache_range_known = false;
        s->cache_duration = 0;
        s->cache_duration_known = json_number(member(v, "cache-duration"), &d) && d >= 0;
        if (s->cache_duration_known) s->cache_duration = d;
        s->cache_bytes_known = json_count(member(v, "fw-bytes"), &s->cache_bytes);
        s->cache_speed_known = json_count(member(v, "raw-input-rate"), &s->cache_speed);
        s->cache_eof_known = json_flag(member(v, "eof"), &s->cache_eof);
        s->cache_underrun_known = json_flag(member(v, "underrun"), &s->cache_underrun);
        s->cache_idle_known = json_flag(member(v, "idle"), &s->cache_idle);
        stream_cache(s, v);
        if (s->cache_duration_known || s->cache_bytes_known || s->cache_speed_known ||
            s->cache_eof_known || s->cache_underrun_known || s->cache_idle_known)
            s->cache_observed_at_ms = now_ms();
        struct json_object *ranges = member(v, "seekable-ranges");
        if (!ranges || json_object_get_type(ranges) != json_type_array) break;
        size_t count = json_object_array_length(ranges);
        /* Report only the contiguous range containing current position, not
         * a fabricated union bridging a discontinuity or an entire duration. */
        for (size_t i = 0; i < count && i < 128; ++i) {
            struct json_object *r = json_object_array_get_idx(ranges, i);
            double start, end;
            if (json_number(member(r, "start"), &start) &&
                json_number(member(r, "end"), &end) && start >= 0 && end >= start &&
                s->position_known && start <= s->position && end >= s->position) {
                s->cache_range_known = true; s->cache_start = start; s->cache_end = end; break;
            }
        }
        break;
    }
    case 6:
        s->width = s->height = 0;
        if (json_number(member(v, "w"), &d) && d > 0 && d <= INT_MAX) s->width = (int)d;
        if (json_number(member(v, "h"), &d) && d > 0 && d <= INT_MAX) s->height = (int)d;
        metadata_text(s->pixel_format, sizeof(s->pixel_format), member(v, "pixelformat"));
        metadata_text(s->hw_pixel_format, sizeof(s->hw_pixel_format), member(v, "hw-pixelformat"));
        break;
    case 7: s->fps = number && d > 0 && d <= 1000 ? d : 0; break;
    case 8: s->video_bitrate = number && d >= 0 ? d : 0; break;
    case 9: s->audio_bitrate = number && d >= 0 ? d : 0; break;
    case 10:
        s->audio_samplerate = s->audio_channels = 0;
        if (json_number(member(v, "samplerate"), &d) && d > 0 && d <= INT_MAX) s->audio_samplerate = (int)d;
        if (json_number(member(v, "channel-count"), &d) && d > 0 && d <= INT_MAX) s->audio_channels = (int)d;
        if (s->audio_samplerate && s->audio_channels) s->audio_observed_at_ms = now_ms();
        break;
    case 11:
        if (v && json_object_get_type(v) == json_type_int && json_object_get_int64(v) >= 0)
            s->dropped_frames = json_object_get_int64(v);
        else s->dropped_frames = -1;
        break;
    case 12: metadata_text(s->video_codec, sizeof(s->video_codec), v); break;
    case 13: metadata_text(s->video_decoder, sizeof(s->video_decoder), v); break;
    case 14: metadata_text(s->audio_codec, sizeof(s->audio_codec), v); break;
    case 15: metadata_text(s->hwdec_current, sizeof(s->hwdec_current), v); break;
    case 16: metadata_text(s->video_output, sizeof(s->video_output), v); break;
    case 17: metadata_text(s->audio_output, sizeof(s->audio_output), v); break;
    case 18: metadata_text(s->video_profile, sizeof(s->video_profile), v); break;
    case 19: metadata_text(s->audio_decoder, sizeof(s->audio_decoder), v); break;
    case 20: if (!json_count(v, &s->output_dropped_frames)) s->output_dropped_frames = -1; break;
    case 21:
        s->cache_buffering_known = number && d >= 0 && d <= 100;
        if (s->cache_buffering_known) s->cache_buffering_percent = d;
        break;
    case 22: s->avsync_known = number; if (number) s->avsync = d; break;
    case 23: s->audio_position_known = number; if (number) s->audio_position = d; break;
    case 24: s->core_idle_known = boolean; s->core_idle = boolean && json_object_get_boolean(v); break;
    case 25: track_selection(s, v); break;
    case 26:
        s->decoded_width = s->decoded_height = 0;
        if (json_number(member(v, "w"), &d) && d > 0 && d <= INT_MAX) s->decoded_width = (int)d;
        if (json_number(member(v, "h"), &d) && d > 0 && d <= INT_MAX) s->decoded_height = (int)d;
        metadata_text(s->decoded_pixel_format, sizeof(s->decoded_pixel_format), member(v, "pixelformat"));
        s->decoded_parameters_known = s->decoded_width && s->decoded_height && s->decoded_pixel_format[0];
        if (s->decoded_parameters_known) s->video_observed_at_ms = now_ms();
        break;
    }
}

static bool list_contains(struct json_object *list, const char *name, bool objects)
{
    if (!list || json_object_get_type(list) != json_type_array) return false;
    size_t length = json_object_array_length(list);
    for (size_t i = 0; i < length; ++i) {
        struct json_object *item = json_object_array_get_idx(list, i);
        if (json_string_is(objects ? member(item, "name") : item, name)) return true;
    }
    return false;
}

static void start_media(mpv_backend_t *b)
{
    /* Warning/error events use this same private IPC owner. Raw messages are
     * discarded; only fixed module categories are recorded below. */
    struct json_object *logs = command_new("request_log_messages");
    b->packet_logs_acked = b->packet_restore_queued = false;
    b->packet_marker_pending = b->packet_marker_seen = b->packet_final_queued = false;
    b->packet_marker_deadline = 0;
    b->snapshot.packet_diagnostics_enabled = b->packet_diagnostics;
    if (b->packet_diagnostics) b->snapshot.packet_capture_started_at_ms = now_ms();
    if (logs) json_object_array_add(logs, json_object_new_string(b->packet_diagnostics ? "terminal-default" : "warn"));
    if (!queue_command(b, R_LOGS, logs)) return;
    for (size_t i = 5; i < PROPERTY_COUNT; ++i) {
        struct json_object *a = command_new("observe_property");
        if (a) {
            json_object_array_add(a, json_object_new_int((int)i + 1));
            json_object_array_add(a, json_object_new_string(properties[i]));
        }
        if (!queue_command(b, R_OBSERVE, a)) return;
    }
    /* The process starts paused. A seek and the preserved pause intent are
     * applied after file-loaded, preventing audio from starting at zero. */
    struct json_object *a = command_new("loadfile");
    if (a) {
        json_object_array_add(a, json_object_new_string(b->url));
        json_object_array_add(a, json_object_new_string("replace"));
    }
    if (!queue_command(b, R_LOAD, a)) return;
    memset(b->url, 0, sizeof(b->url));
    b->snapshot.state = MPV_BACKEND_LOADING;
    b->load_at = now_ms();
}

static void finish_seek(mpv_backend_t *b)
{
    b->seek_inflight = 0;
    b->seek_acked = b->seek_event = b->seek_restarted = false;
    b->snapshot.seeking = b->seek_dirty;
}

static void maybe_finish_seek(mpv_backend_t *b)
{
    if (b->seek_inflight && b->seek_acked && b->seek_event && b->seek_restarted)
        finish_seek(b);
}

static void response(mpv_backend_t *b, struct json_object *obj)
{
    struct json_object *id_obj = member(obj, "request_id");
    if (!id_obj || json_object_get_type(id_obj) != json_type_int) return;
    int64_t id = json_object_get_int64(id_obj);
    size_t i;
    for (i = 0; i < b->request_count && b->requests[i].id != id; ++i) {}
    if (i == b->request_count) return; /* stale or unsolicited reply */
    enum request_kind kind = b->requests[i].kind;
    uint64_t seek_serial = b->requests[i].seek_serial;
    b->requests[i] = b->requests[--b->request_count];
    if (b->child_generation != b->snapshot.generation || b->stop_step || b->stop_requested) return;
    bool success = json_string_is(member(obj, "error"), "success");
    struct json_object *data = member(obj, "data");
    if (!success) {
        switch (kind) {
        case R_OBSERVE:
            if (b->snapshot.metadata_observation_errors < UINT64_MAX) ++b->snapshot.metadata_observation_errors;
            break; /* Optional metadata can be unavailable. */
        case R_LOGS:
            b->snapshot.log_messages_active = false;
            if (b->packet_diagnostics) count_one(&b->snapshot.packet_capture_errors);
            break;
        case R_PACKET_RESTORE:
        case R_PACKET_MARKER:
            packet_capture_failed(b);
            packet_final_levels(b);
            break;
        case R_PACKET_FINAL:
            packet_capture_failed(b);
            packet_request_warn(b);
            break;
        case R_PACKET_WARN: packet_capture_failed(b); break;
        case R_OSD: copy_text(b->snapshot.control_error, sizeof(b->snapshot.control_error), "mpv rejected the diagnostic overlay"); break;
        case R_SEEK:
            if (seek_serial == b->seek_inflight) {
                copy_text(b->snapshot.control_error, sizeof(b->snapshot.control_error), "mpv rejected seek");
                finish_seek(b);
            }
            break;
        case R_VOLUME: copy_text(b->snapshot.control_error, sizeof(b->snapshot.control_error), "mpv rejected volume control"); break;
        case R_QUIT: break;
        case R_PAUSE: fail_as(b, MPV_BACKEND_ERROR_CONTROL, "control", "mpv rejected playback pause/resume control"); break;
        case R_INITIAL_SEEK:
            if (seek_serial == b->seek_inflight)
                fail_as(b, MPV_BACKEND_ERROR_CONTROL, "control", "mpv could not seek to the requested starting position");
            break;
        case R_LOAD: fail_as(b, MPV_BACKEND_ERROR_LOAD, "load", "mpv rejected the media load request"); break;
        default: fail_as(b, MPV_BACKEND_ERROR_STARTUP, "startup", "mpv does not provide the required JSON IPC capabilities"); break;
        }
        return;
    }
    switch (kind) {
    case R_LOGS:
        b->snapshot.log_messages_active = true;
        b->packet_logs_acked = b->packet_diagnostics;
        b->snapshot.packet_diagnostics_active = b->packet_diagnostics && !b->packet_restore_queued;
        break;
    case R_PACKET_RESTORE: {
        struct json_object *a = command_new("print-text");
        if (a) json_object_array_add(a, json_object_new_string(PACKET_CAPTURE_MARKER));
        b->packet_marker_pending = true;
        b->packet_marker_deadline = now_ms() + b->startup_ms;
        if (!queue_command_maybe(b, R_PACKET_MARKER, a, false)) {
            packet_capture_failed(b);
            packet_final_levels(b);
        }
        break;
    }
    case R_PACKET_FINAL:
        b->snapshot.packet_diagnostics_complete = b->packet_logs_acked && b->packet_marker_seen &&
            (b->snapshot.packet_audio.packets || b->snapshot.packet_video.packets);
        observation_ages(&b->snapshot);
        break;
    case R_PACKET_MARKER:
    case R_PACKET_WARN: break;
    case R_SEEK:
    case R_INITIAL_SEEK:
        if (seek_serial == b->seek_inflight) {
            b->seek_acked = true;
            maybe_finish_seek(b);
        }
        break;
    case R_VERSION:
        if (!data || json_object_get_type(data) != json_type_string)
            fail_as(b, MPV_BACKEND_ERROR_STARTUP, "startup", "mpv returned an invalid IPC handshake");
        else get_property(b, R_COMMANDS, "command-list");
        break;
    case R_COMMANDS: {
        /* get/set/observe_property are JSON IPC commands, not entries in
         * mpv's native command-list. Exercise them separately below. */
        const char *required[] = {"loadfile", "seek", "quit", "show-text"};
        for (size_t j = 0; j < sizeof(required)/sizeof(required[0]); ++j) {
            if (!list_contains(data, required[j], true)) {
                fail_as(b, MPV_BACKEND_ERROR_STARTUP, "startup", "mpv is missing a required JSON IPC command"); return;
            }
        }
        get_property(b, R_PROPERTIES, "property-list");
        break;
    }
    case R_PROPERTIES:
        for (size_t j = 0; j < 5; ++j) {
            if (!list_contains(data, properties[j], false)) {
                fail_as(b, MPV_BACKEND_ERROR_STARTUP, "startup", "mpv is missing a required playback property"); return;
            }
        }
        b->handshake_pending = 6;
        for (size_t j = 0; j < 5; ++j) {
            struct json_object *a = command_new("observe_property");
            if (a) {
                json_object_array_add(a, json_object_new_int((int)j + 1));
                json_object_array_add(a, json_object_new_string(properties[j]));
            }
            if (!queue_command(b, R_REQUIRED_OBSERVE, a)) return;
        }
        set_property(b, R_CHECK_PAUSE, "pause", json_object_new_boolean(true));
        break;
    case R_REQUIRED_OBSERVE:
    case R_CHECK_PAUSE:
        if (b->handshake_pending && --b->handshake_pending == 0) {
            b->ipc_ready = true;
            if (!b->preflight) start_media(b);
        }
        break;
    default: break;
    }
}

/* mpv 0.40 JSON emits file_error as a fixed string, not the C API integer.
 * Map only its documented API vocabulary. Never retain unknown error text.
 * These are MPV_ERROR codes, not FFmpeg errors or HTTP response codes. */
static void capture_file_error(mpv_backend_snapshot_t *s, struct json_object *value)
{
    static const char *const errors[] = {
        "success", "event queue full", "memory allocation failed", "core not initialized",
        "invalid parameter", "option not found", "unsupported format for accessing option",
        "error setting option", "property not found", "unsupported format for accessing property",
        "property unavailable", "error accessing property", "error running command", "loading failed",
        "audio output initialization failed", "video output initialization failed",
        "no audio or video data played", "unrecognized file format", "not supported",
        "operation not implemented", "something happened"
    };
    s->file_error_code_known = false;
    s->file_error_code = 0;
    copy_text(s->file_error, sizeof(s->file_error), "unknown");
    for (size_t i = 1; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        if (json_string_is(value, errors[i])) {
            s->file_error_code_known = true;
            s->file_error_code = -(int)i;
            copy_text(s->file_error, sizeof(s->file_error), errors[i]);
            break;
        }
    }
}

static bool log_module(struct json_object *prefix, const char *module)
{
    if (!prefix || json_object_get_type(prefix) != json_type_string) return false;
    const char *p = json_object_get_string(prefix);
    size_t n = strlen(module);
    return !strncmp(p, module, n) && (!p[n] || p[n] == '/');
}

static void count_one(uint64_t *count)
{
    if (*count < UINT64_MAX) ++*count;
}

static bool text_starts(const char *text, const char *prefix)
{
    return text && !strncmp(text, prefix, strlen(prefix));
}

static const char *diagnostic_text(struct json_object *obj)
{
    struct json_object *v = member(obj, "text");
    if (!v || json_object_get_type(v) != json_type_string) return NULL;
    const char *p = json_object_get_string(v);
    size_t n = (size_t)json_object_get_string_len(v);
    if (n > 4096 || n != strlen(p)) return NULL;
    /* The FFmpeg bridge adds an AVClass name to the text. Only known static
     * class names are stripped; arbitrary text never becomes a stored label. */
    static const char *const classes[] = {"http: ", "https: ", "hls: ", "hevc: ", "h264: ", "aac: ", "mpegts: "};
    for (size_t i = 0; i < sizeof(classes)/sizeof(classes[0]); ++i)
        if (text_starts(p, classes[i])) return p + strlen(classes[i]);
    return p;
}

static bool packet_float(const char *text, double *out)
{
    /* mpv's %f emits a decimal number. A missing timestamp is MP_NOPTS_VALUE,
     * -2^63, not zero. Reject alternative syntax and nonfinite values. */
    const char *p = text;
    if (*p == '-') ++p;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
    if (*p++ != '.') return false;
    const char *decimal = p;
    while (*p >= '0' && *p <= '9') ++p;
    if (p == decimal || *p) return false;
    char *end;
    errno = 0;
    *out = strtod(text, &end);
    return !errno && !*end && isfinite(*out) &&
        (fabs(*out) <= 1e12 || *out == -0x1p63);
}

static bool packet_uint(const char *text, uint64_t *out)
{
    char *end;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    if (errno || *end || n > INT64_MAX) return false;
    *out = n;
    return true;
}

static void capture_packet_diagnostic(mpv_backend_snapshot_t *s, struct json_object *obj)
{
    if (!s->packet_diagnostics_enabled ||
        (!s->packet_diagnostics_active && !s->packet_capture_ended_at_ms) ||
        !json_string_is(member(obj, "level"), "trace") ||
        !json_string_is(member(obj, "prefix"), "lavf")) return;
    const char *t = diagnostic_text(obj);
    if (!t) {
        struct json_object *raw = member(obj, "text");
        if (raw && json_object_get_type(raw) == json_type_string &&
            text_starts(json_object_get_string(raw), "append packet to "))
            count_one(&s->packet_log_rejected);
        return;
    }
    if (!text_starts(t, "append packet to ")) return;
    char type[8], bytes[21], pts[48], dts[48], pos[21], num[3], forward[21];
    int consumed = 0;
    bool ok = sscanf(t, "append packet to %7[a-z]: size=%20[0-9] pts=%47[-0-9.] dts=%47[-0-9.] pos=%20[-0-9] [num=%2[>1] size=%20[0-9]]%n",
        type, bytes, pts, dts, pos, num, forward, &consumed) == 7 && consumed > 0 &&
        (!t[consumed] || !strcmp(t + consumed, "\n"));
    uint64_t size, fw;
    double timestamp, decode_timestamp;
    char *position_end = NULL;
    errno = 0;
    if (ok) (void)strtoll(pos, &position_end, 10);
    ok = ok && !errno && position_end && !*position_end &&
        (!strcmp(num, "1") || !strcmp(num, ">1")) &&
        packet_uint(bytes, &size) && packet_uint(forward, &fw) &&
        packet_float(pts, &timestamp) && packet_float(dts, &decode_timestamp);
    if (!ok) { count_one(&s->packet_log_rejected); return; }
    mpv_backend_packet_stream_t *stream = !strcmp(type, "video") ? &s->packet_video :
        !strcmp(type, "audio") ? &s->packet_audio : NULL;
    if (!stream) return;
    if (!s->packet_diagnostics_active) {
        /* A producer racing the verbosity change invalidates the FIFO boundary.
         * Never silently turn a late audio packet into a zero-audio conclusion. */
        if (!s->packet_capture_errors) count_one(&s->packet_capture_errors);
        s->packet_diagnostics_complete = false;
        return;
    }
    uint64_t now = now_ms();
    if (!stream->packets) stream->first_at_ms = now;
    count_one(&stream->packets);
    stream->bytes = size > UINT64_MAX - stream->bytes ? UINT64_MAX : stream->bytes + size;
    stream->last_at_ms = now;
    if (timestamp != -0x1p63) {
        if (!stream->pts_packets) stream->first_pts = timestamp;
        count_one(&stream->pts_packets);
        stream->last_pts = timestamp;
    }
}

static void capture_log_diagnostic(mpv_backend_snapshot_t *s, struct json_object *obj)
{
    capture_packet_diagnostic(s, obj);
    bool warning = json_string_is(member(obj, "level"), "warn");
    bool error = json_string_is(member(obj, "level"), "error") || json_string_is(member(obj, "level"), "fatal");
    if (!warning && !error) return;
    struct json_object *prefix = member(obj, "prefix");
    if (json_string_is(prefix, "overflow")) {
        count_one(&s->log_overflows);
        return;
    }
    /* Error module counts and warning counts remain separate. Classification
     * is diagnostic only: it cannot stop/resume playback or change ownership. */
    const char *stage = "unclassified";
    uint64_t *count = &s->unclassified_errors;
    if (log_module(prefix, "ffmpeg/video") || log_module(prefix, "vd")) {
        stage = "video-decode"; count = &s->video_decode_errors;
    } else if (log_module(prefix, "ffmpeg/audio") || log_module(prefix, "ad")) {
        stage = "audio-decode"; count = &s->audio_decode_errors;
    } else if (log_module(prefix, "vo")) {
        stage = "video-output"; count = &s->video_output_errors;
    } else if (log_module(prefix, "ao")) {
        stage = "audio-output"; count = &s->audio_output_errors;
    } else if (log_module(prefix, "demux") || log_module(prefix, "lavf") || log_module(prefix, "ffmpeg/demuxer")) {
        stage = "demux"; count = &s->demux_errors;
    } else if (log_module(prefix, "stream")) {
        stage = "source"; count = &s->network_errors;
    }
    if (error) { count_one(count); count_one(&s->log_errors); }
    else count_one(&s->log_warnings);
    if (error) copy_text(s->diagnostic_stage, sizeof(s->diagnostic_stage), stage);

    const char *t = diagnostic_text(obj), *reason = NULL;
    uint64_t *warning_count = NULL;
    if (!t) count_one(&s->log_text_rejected);
    bool decoder = log_module(prefix, "ffmpeg/video") || log_module(prefix, "vd");
    bool audio = log_module(prefix, "ffmpeg/audio") || log_module(prefix, "ad") || log_module(prefix, "ao");
    bool source = log_module(prefix, "ffmpeg") || log_module(prefix, "lavf") || log_module(prefix, "demux");
    bool player = log_module(prefix, "cplayer");
    if (source && text_starts(t, "HTTP error ")) {
        const char *p = t + strlen("HTTP error ");
        if (strlen(p) >= 3 && p[0] >= '4' && p[0] <= '5' &&
            p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' &&
            (!p[3] || p[3] == ' ' || p[3] == '\r' || p[3] == '\n')) {
            s->last_http_status_known = true;
            s->last_http_status = (p[0]-'0')*100 + (p[1]-'0')*10 + p[2]-'0';
            count_one(&s->http_error_count);
            reason = p[0] == '4' ? "http-client-error" : "http-server-error";
        }
    } else if (source && text_starts(t, "Failed to open an initialization section in playlist ")) {
        reason = "hls-init-failure"; count_one(&s->hls_init_failures);
    } else if (source && (text_starts(t, "Failed to open segment ") || text_starts(t, "Error when loading first segment "))) {
        reason = "hls-segment-failure"; count_one(&s->hls_segment_failures);
    } else if (source && text_starts(t, "Failed to reload playlist ")) {
        reason = "hls-reload-failure"; count_one(&s->hls_reload_failures);
    } else if (decoder && (text_starts(t, "Could not find ref with POC ") ||
               text_starts(t, "PPS id out of range") || text_starts(t, "Missing reference picture"))) {
        reason = "missing-reference"; warning_count = &s->missing_reference_warnings;
    } else if (decoder && text_starts(t, "Error while decoding frame")) {
        reason = "video-decode-error"; warning_count = &s->video_decode_warnings;
    } else if ((decoder || audio || source) && (text_starts(t, "Invalid data found when processing input") ||
               text_starts(t, "Error parsing NAL unit") || text_starts(t, "Error splitting the input into NAL units") ||
               text_starts(t, "Skipping invalid undecodable NALU") || text_starts(t, "Invalid NAL unit size"))) {
        reason = "invalid-data"; warning_count = &s->invalid_data_warnings;
    } else if ((decoder || audio || player || source) && (text_starts(t, "Invalid audio PTS:") ||
               text_starts(t, "Invalid video timestamp:") || text_starts(t, "No video PTS!") ||
               text_starts(t, "Reset playback due to audio timestamp reset.") || text_starts(t, "Linearizing discontinuity:"))) {
        reason = "timestamp-discontinuity"; warning_count = &s->timestamp_warnings;
    } else if ((audio || player) && text_starts(t, "Audio device underrun detected.")) {
        reason = "audio-output-underrun"; warning_count = &s->audio_output_warnings;
    } else if ((audio || player) && (text_starts(t, "Could not open/initialize audio device") ||
               text_starts(t, "Error reinitializing audio.") || text_starts(t, "Audio filter initialized failed!"))) {
        reason = "audio-output-init-error"; warning_count = &s->audio_output_warnings;
    } else if (audio && text_starts(t, "Error decoding audio.")) {
        reason = "audio-decode-error";
    } else if (source && text_starts(t, "PES packet size mismatch")) {
        stage = "demux";
        reason = "pes-size-mismatch"; warning_count = &s->pes_mismatch_warnings;
    } else if (source && text_starts(t, "Packet corrupt (stream = ")) {
        stage = "demux";
        reason = "packet-corrupt"; warning_count = &s->packet_corrupt_warnings;
    } else if (source && text_starts(t, "error reading packet: ")) {
        stage = "demux";
        reason = "demux-read-error"; warning_count = &s->demux_read_warnings;
    } else if (source && text_starts(t, "skipping ") && t && strstr(t, " segments ahead, expired from playlists")) {
        stage = "demux";
        reason = "hls-expired-segments";
    } else if (source && (text_starts(t, "Media sequence changed unexpectedly:") ||
               text_starts(t, "The m3u8 list sequence may have been wrapped."))) {
        stage = "demux";
        reason = "hls-sequence-change";
    } else if (log_module(prefix, "vo") && (text_starts(t, "Can't open TTY for VT control:") ||
               text_starts(t, "Failed to set up VT switcher."))) {
        reason = "virtual-terminal-unavailable";
    } else if (log_module(prefix, "vo") && text_starts(t, "Failed presenting frame!")) {
        reason = "frame-present-failure";
    } else if (log_module(prefix, "vo") && (text_starts(t, "Failed to acquire DRM master:") ||
               text_starts(t, "Failed to commit ModeSetting atomic request:"))) {
        reason = "drm-display-failure";
    }
    if (warning && warning_count) count_one(warning_count);
    else if (warning && !reason) count_one(&s->unknown_warnings);
    if (error) copy_text(s->diagnostic_stage, sizeof(s->diagnostic_stage), stage);
    if (warning) {
        copy_text(s->last_warning_stage, sizeof(s->last_warning_stage), stage);
        copy_text(s->last_warning_reason, sizeof(s->last_warning_reason), reason ? reason : "unknown");
        s->last_warning_at_ms = now_ms();
    }
    if (reason) {
        copy_text(s->diagnostic_reason, sizeof(s->diagnostic_reason), reason);
        s->diagnostic_reason_at_ms = now_ms();
    }
}

static void retiring_terminal(mpv_backend_snapshot_t *s, struct json_object *obj)
{
    struct json_object *reason = member(obj, "reason");
    static const char *const reasons[] = {"eof", "stop", "quit", "error", "redirect"};
    copy_text(s->end_reason, sizeof(s->end_reason), "unknown");
    for (size_t i = 0; i < sizeof(reasons)/sizeof(reasons[0]); ++i)
        if (json_string_is(reason, reasons[i])) copy_text(s->end_reason, sizeof(s->end_reason), reasons[i]);
    if (json_string_is(reason, "error")) {
        capture_file_error(s, member(obj, "file_error"));
        if (s->error_code == MPV_BACKEND_ERROR_NONE) {
            s->error_code = s->ready ? MPV_BACKEND_ERROR_PLAYBACK : MPV_BACKEND_ERROR_LOAD;
            const char *stage = s->ready ? "playback" : "load";
            switch (s->file_error_code) {
            case -14: s->error_code = MPV_BACKEND_ERROR_AUDIO_OUTPUT; stage = "audio-output"; break;
            case -15: s->error_code = MPV_BACKEND_ERROR_VIDEO_OUTPUT; stage = "video-output"; break;
            case -16: s->error_code = MPV_BACKEND_ERROR_NO_MEDIA; break;
            case -17: s->error_code = MPV_BACKEND_ERROR_FORMAT; break;
            default: break;
            }
            copy_text(s->failure_stage, sizeof(s->failure_stage), stage);
            copy_text(s->error, sizeof(s->error), "mpv playback ended before successful completion");
        }
        s->state = MPV_BACKEND_FAILED;
    } else if (!s->error_code && json_string_is(reason, "eof")) s->state = MPV_BACKEND_ENDED;
}

static void message(mpv_backend_t *b, struct json_object *obj)
{
    if (json_object_get_type(obj) != json_type_object) {
        fail(b, "mpv sent an invalid IPC message"); return;
    }
    struct json_object *event = member(obj, "event");
    if (!event) { response(b, obj); return; }
    if (b->child_generation != b->snapshot.generation) {
        /* Replacement metadata cannot update the new session. Preserve only
         * diagnostic/terminal evidence for the actual retiring child. */
        if (b->retiring_valid && b->retiring.generation == b->child_generation) {
            if (json_string_is(event, "log-message")) capture_log_diagnostic(&b->retiring, obj);
            else if (json_string_is(event, "end-file")) retiring_terminal(&b->retiring, obj);
            else if (json_string_is(event, "event-queue-overflow")) count_one(&b->retiring.event_overflows);
        }
        return;
    }
    /* The same owned child's final diagnostics remain relevant after a user
     * stop. Only normal lifecycle/property processing is disabled below. */
    if (json_string_is(event, "log-message")) {
        capture_log_diagnostic(&b->snapshot, obj);
        if (b->packet_marker_pending && !b->stop_requested && !b->stop_step &&
            json_string_is(member(obj, "prefix"), "cplayer") &&
            json_string_is(member(obj, "level"), "info") &&
            json_string_is(member(obj, "text"), PACKET_CAPTURE_MARKER "\n")) {
            b->packet_marker_seen = true;
            b->packet_marker_pending = false;
            b->packet_marker_deadline = 0;
            b->snapshot.packet_diagnostics_active = false;
            b->snapshot.packet_capture_ended_at_ms = now_ms();
            packet_final_levels(b);
        }
        return;
    }
    if (json_string_is(event, "event-queue-overflow")) {
        count_one(&b->snapshot.event_overflows);
        return;
    }
    if (b->stop_requested || b->stop_step || !b->want_session) {
        if (json_string_is(event, "end-file")) {
            mpv_backend_state_t state = b->snapshot.state;
            retiring_terminal(&b->snapshot, obj);
            b->snapshot.state = state; /* Evidence cannot change stop ownership/control. */
        }
        return;
    }
    if (json_string_is(event, "property-change")) {
        struct json_object *id = member(obj, "id");
        if (!id || json_object_get_type(id) != json_type_int) return;
        int64_t observation_id = json_object_get_int64(id);
        if (observation_id > 0 && observation_id <= (int64_t)PROPERTY_COUNT &&
            json_string_is(member(obj, "name"), properties[observation_id - 1]))
            property_changed(b, (int)observation_id - 1, member(obj, "data"));
    } else if (json_string_is(event, "file-loaded")) {
        b->snapshot.ready = true;
        update_state(b);
    } else if (json_string_is(event, "playback-restart")) {
        b->snapshot.playback_restarted = true;
        if (b->seek_inflight && b->seek_event) b->seek_restarted = true;
        maybe_finish_seek(b);
        update_state(b);
    } else if (json_string_is(event, "seek")) {
        if (b->seek_inflight) b->seek_event = true;
    } else if (json_string_is(event, "end-file")) {
        struct json_object *reason = member(obj, "reason");
        static const char *const reasons[] = {"eof", "stop", "quit", "error", "redirect"};
        copy_text(b->snapshot.end_reason, sizeof(b->snapshot.end_reason), "unknown");
        for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i)
            if (json_string_is(reason, reasons[i]))
                copy_text(b->snapshot.end_reason, sizeof(b->snapshot.end_reason), reasons[i]);
        if (json_string_is(reason, "redirect")) return;
        b->diagnostic_drain_until = now_ms() + TERMINAL_DIAGNOSTIC_DRAIN_MS;
        b->snapshot.terminal_diagnostics_draining = true;
        b->tx_size = b->tx_sent = b->request_count = 0;
        memset(b->tx, 0, sizeof(b->tx));
        if (json_string_is(reason, "eof")) {
            b->snapshot.state = MPV_BACKEND_ENDED;
            b->snapshot.ready = false;
            b->snapshot.seeking = false;
            b->want_session = false;
            b->stop_requested = true;
        } else {
            const char *stage = b->snapshot.ready ? "playback" : "load";
            mpv_backend_error_t code = b->snapshot.ready ? MPV_BACKEND_ERROR_PLAYBACK : MPV_BACKEND_ERROR_LOAD;
            if (json_string_is(reason, "error")) {
                capture_file_error(&b->snapshot, member(obj, "file_error"));
                switch (b->snapshot.file_error_code) {
                case -14: code = MPV_BACKEND_ERROR_AUDIO_OUTPUT; stage = "audio-output"; break;
                case -15: code = MPV_BACKEND_ERROR_VIDEO_OUTPUT; stage = "video-output"; break;
                case -16: code = MPV_BACKEND_ERROR_NO_MEDIA; break;
                case -17: code = MPV_BACKEND_ERROR_FORMAT; break;
                default: break;
                }
            }
            fail_as(b, code, stage, "mpv playback ended before successful completion");
        }
    } else if (json_string_is(event, "shutdown")) {
        fail_as(b, MPV_BACKEND_ERROR_PLAYBACK, "playback", "mpv shut down unexpectedly");
    }
}

static void read_ipc(mpv_backend_t *b)
{
    size_t budget = READ_BUDGET;
    while (b->fd >= 0 && budget) {
        size_t capacity = LINE_LIMIT - b->rx_size;
        if (!capacity) { fail(b, "mpv IPC message exceeded its size limit"); return; }
        if (capacity > budget) capacity = budget;
        ssize_t n = recv(b->fd, b->rx + b->rx_size, capacity, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) return;
            if (!b->stop_step && !b->stop_requested) fail(b, "mpv IPC read failed");
            else close_ipc(b);
            return;
        }
        if (!n) {
            if (!b->stop_step && !b->stop_requested) fail(b, "mpv closed its IPC channel unexpectedly");
            else close_ipc(b);
            return;
        }
        budget -= (size_t)n;
        b->rx_size += (size_t)n;
        size_t consumed = 0;
        while (consumed < b->rx_size) {
            char *newline = memchr(b->rx + consumed, '\n', b->rx_size - consumed);
            if (!newline) break;
            size_t len = (size_t)(newline - (b->rx + consumed));
            if (!len) { consumed++; continue; }
            struct json_tokener *tok = json_tokener_new_ex(20);
            if (!tok) { fail(b, "Cannot parse mpv IPC message"); return; }
            json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
            struct json_object *obj = json_tokener_parse_ex(tok, b->rx + consumed, (int)len);
            enum json_tokener_error err = json_tokener_get_error(tok);
            size_t end = json_tokener_get_parse_end(tok);
            json_tokener_free(tok);
            if (!obj || err != json_tokener_success || end != len) {
                if (obj) json_object_put(obj);
                fail(b, "mpv sent malformed JSON IPC data"); return;
            }
            message(b, obj);
            json_object_put(obj);
            if (b->fd < 0) return;
            consumed += len + 1;
        }
        if (consumed) {
            memmove(b->rx, b->rx + consumed, b->rx_size - consumed);
            b->rx_size -= consumed;
        }
    }
    if (!budget) {
        mpv_backend_snapshot_t *s = b->retiring_valid && b->retiring.generation == b->child_generation ? &b->retiring : &b->snapshot;
        count_one(&s->ipc_read_budget_exhaustions);
    }
}

static void flush_ipc(mpv_backend_t *b)
{
    if (b->fd < 0 || b->tx_sent == b->tx_size) return;
    size_t amount = b->tx_size - b->tx_sent;
    if (amount > READ_BUDGET) amount = READ_BUDGET;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    ssize_t n = send(b->fd, b->tx + b->tx_sent, amount, flags);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        if (!b->stop_step && !b->stop_requested) fail(b, "mpv IPC write failed");
        else close_ipc(b);
        return;
    }
    b->tx_sent += (size_t)n;
    if (b->tx_sent == b->tx_size) {
        memset(b->tx, 0, b->tx_size);
        b->tx_sent = b->tx_size = 0;
    }
}

static void enqueue_terminal_report(mpv_backend_t *b, const mpv_backend_snapshot_t *source,
                                     pid_t pid, int wait_status, bool wait_known)
{
    if (!source->generation) return; /* startup-only preflight */
    const size_t capacity = sizeof(b->terminal_reports)/sizeof(b->terminal_reports[0]);
    if (b->terminal_count == capacity) {
        b->terminal_first = (b->terminal_first + 1) % capacity;
        --b->terminal_count;
        count_one(&b->terminal_reports_dropped);
    }
    mpv_backend_snapshot_t *out = &b->terminal_reports[(b->terminal_first + b->terminal_count++) % capacity];
    *out = *source;
    out->child_alive = false;
    out->child_pid = (int)pid; /* retain identity in final reports */
    out->child_generation = source->generation;
    out->terminal_diagnostics_draining = out->recovery_required = false;
    out->ready = out->seeking = false;
    out->packet_diagnostics_active = false;
    if (out->packet_diagnostics_enabled && !out->packet_capture_ended_at_ms)
        out->packet_capture_ended_at_ms = now_ms();
    out->child_exit_known = wait_known && (WIFEXITED(wait_status) || WIFSIGNALED(wait_status));
    out->child_exit_code = wait_known && WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
    out->child_signal = wait_known && WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0;
    if (out->error_code) out->state = MPV_BACKEND_FAILED;
    else if (out->state != MPV_BACKEND_ENDED) out->state = MPV_BACKEND_IDLE;
    out->terminal_reports_dropped = b->terminal_reports_dropped;
    observation_ages(out);
}

static void reap_child(mpv_backend_t *b)
{
    if (!b->pid) return;
    int status = 0;
    pid_t result = waitpid(b->pid, &status, WNOHANG);
    if (!result || (result < 0 && errno == EINTR)) return;
    if (result < 0 && errno != ECHILD) { fail(b, "Cannot check the mpv child process"); return; }
    /* A fast failing child can exit with its end-file event still queued in
     * the socket. Drain bounded pending data before closing/reclassifying it. */
    if (result > 0 && b->fd >= 0) read_ipc(b);
    if (result > 0 && b->child_generation == b->snapshot.generation) {
        b->snapshot.child_exit_known = WIFEXITED(status) || WIFSIGNALED(status);
        b->snapshot.child_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        b->snapshot.child_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    }
    bool unexpected = !b->stop_requested && !b->stop_step &&
        b->child_generation == b->snapshot.generation;
    bool was_ready = b->ipc_ready;
    pid_t reaped_pid = b->pid;
    b->pid = 0;
    b->snapshot.child_alive = false;
    if (b->child_generation == b->snapshot.generation) {
        b->snapshot.packet_diagnostics_active = false;
        if (b->snapshot.packet_diagnostics_enabled && !b->snapshot.packet_capture_ended_at_ms)
            b->snapshot.packet_capture_ended_at_ms = now_ms();
    }
    b->snapshot.child_pid = 0;
    b->snapshot.child_generation = 0;
    b->snapshot.recovery_required = false;
    close_ipc(b);
    b->ipc_ready = false;
    b->stop_step = 0;
    b->stop_requested = false;
    if (unexpected) fail_as(b, was_ready ? MPV_BACKEND_ERROR_PLAYBACK : MPV_BACKEND_ERROR_STARTUP,
                            was_ready ? "playback" : "startup",
                            "mpv exited unexpectedly or rejected its configuration");
    else if (!b->want_session && b->snapshot.state == MPV_BACKEND_STOPPING)
        b->snapshot.state = MPV_BACKEND_IDLE;
    if (b->retiring_valid && b->retiring.generation == b->child_generation) {
        enqueue_terminal_report(b, &b->retiring, reaped_pid, status, result > 0);
        memset(&b->retiring, 0, sizeof(b->retiring));
        b->retiring_valid = false;
    } else if (b->child_generation == b->snapshot.generation) {
        enqueue_terminal_report(b, &b->snapshot, reaped_pid, status, result > 0);
    }
}

static void stop_child(mpv_backend_t *b, uint64_t now)
{
    if (!b->pid) return;
    if (!b->stop_step) {
        b->stop_step = 1;
        b->stop_at = now;
        if (b->fd >= 0) queue_command(b, R_QUIT, command_new("quit"));
    } else if (now - b->stop_at >= b->stop_ms) {
        b->stop_at = now;
        if (b->stop_step == 1) {
            close_ipc(b);
            kill(b->pid, SIGTERM);
            b->stop_step = 2;
        } else if (b->stop_step == 2) {
            kill(b->pid, SIGKILL);
            b->stop_step = 3;
        } else if (b->stop_step == 3) {
            b->stop_step = 4;
            fail_as(b, MPV_BACKEND_ERROR_STOP, "stop", "mpv did not release its process; device recovery may be required");
            b->snapshot.recovery_required = true;
        }
    }
}

static void apply_controls(mpv_backend_t *b)
{
    if (!b->snapshot.ready || b->fd < 0 || !b->ipc_ready) return;
    if (b->volume_dirty) {
        b->volume_dirty = false;
        if (!set_property(b, R_VOLUME, "volume", json_object_new_double(b->volume))) return;
        if (!set_property(b, R_VOLUME, "mute", json_object_new_boolean(b->mute))) return;
    }
    if (b->seek_dirty && !b->seek_inflight) {
        b->seek_dirty = false;
        b->seek_inflight = b->seek_serial;
        b->seek_started_at = now_ms();
        b->seek_acked = b->seek_event = b->seek_restarted = false;
        b->inflight_initial_seek = b->initial_seek;
        struct json_object *a = command_new("seek");
        if (a) {
            json_object_array_add(a, json_object_new_double(b->seek_seconds));
            json_object_array_add(a, json_object_new_string("absolute+exact"));
        }
        if (!queue_command(b, b->initial_seek ? R_INITIAL_SEEK : R_SEEK, a)) return;
        b->initial_seek = false;
    }
    if (b->pause_dirty) {
        b->pause_dirty = false;
        if (!set_property(b, R_PAUSE, "pause", json_object_new_boolean(b->snapshot.requested_paused))) return;
        update_state(b);
    }
    if (b->osd_dirty) {
        b->osd_dirty = false;
        struct json_object *a = command_new("raw");
        if (a) {
            json_object_array_add(a, json_object_new_string("show-text"));
            json_object_array_add(a, json_object_new_string(b->osd));
            json_object_array_add(a, json_object_new_int(2000));
            json_object_array_add(a, json_object_new_int(0));
        }
        queue_command(b, R_OSD, a);
    }
}

static void check_control_deadlines(mpv_backend_t *b, uint64_t now)
{
    for (size_t i = 0; i < b->request_count; ) {
        struct request r = b->requests[i];
        if (now - r.queued_at < b->startup_ms ||
            (r.kind != R_PAUSE && r.kind != R_VOLUME && r.kind != R_OSD && r.kind != R_OBSERVE &&
             r.kind != R_LOGS && r.kind != R_PACKET_RESTORE && r.kind != R_PACKET_WARN &&
             r.kind != R_PACKET_MARKER && r.kind != R_PACKET_FINAL)) {
            ++i;
            continue;
        }
        b->requests[i] = b->requests[--b->request_count];
        if (r.kind == R_PAUSE) {
            fail_as(b, MPV_BACKEND_ERROR_CONTROL, "control", "mpv playback control acknowledgement timed out");
            return;
        }
        if (r.kind == R_VOLUME)
            copy_text(b->snapshot.control_error, sizeof(b->snapshot.control_error), "mpv volume acknowledgement timed out");
        else if (r.kind == R_OSD)
            copy_text(b->snapshot.control_error, sizeof(b->snapshot.control_error), "mpv overlay acknowledgement timed out");
        else if (r.kind == R_OBSERVE && b->snapshot.metadata_observation_errors < UINT64_MAX)
            ++b->snapshot.metadata_observation_errors;
        else if ((r.kind == R_LOGS && b->packet_diagnostics) ||
                 r.kind == R_PACKET_RESTORE || r.kind == R_PACKET_WARN ||
                 r.kind == R_PACKET_MARKER || r.kind == R_PACKET_FINAL) {
            packet_capture_failed(b);
            if (r.kind == R_PACKET_RESTORE || r.kind == R_PACKET_MARKER) packet_final_levels(b);
            else if (r.kind == R_PACKET_FINAL) packet_request_warn(b);
        }
    }
}

void mpv_backend_poll(mpv_backend_t *b)
{
    if (!b) return;
    pthread_mutex_lock(&b->lock);
    reap_child(b);
    if (!b->pid && b->want_session && !b->closing && b->snapshot.state != MPV_BACKEND_FAILED)
        spawn_player(b);
    if (b->pid) {
        uint64_t now = now_ms();
        if (b->stop_requested || b->closing || b->child_generation != b->snapshot.generation) {
            if (!b->diagnostic_drain_until || now >= b->diagnostic_drain_until ||
                b->closing || b->child_generation != b->snapshot.generation) {
                b->diagnostic_drain_until = 0;
                b->snapshot.terminal_diagnostics_draining = false;
                stop_child(b, now);
            }
        }
        flush_ipc(b);
        read_ipc(b);
        now = now_ms();
        if (!b->stop_requested && !b->stop_step && b->want_session) {
            if (!b->ipc_ready && now - b->started_at >= b->startup_ms)
                fail_as(b, MPV_BACKEND_ERROR_STARTUP, "startup", "mpv IPC startup timed out");
            else if (!b->preflight && b->load_at && !b->snapshot.ready && now - b->load_at >= b->load_ms)
                fail_as(b, MPV_BACKEND_ERROR_LOAD, "load", "mpv media loading timed out");
            else {
                check_control_deadlines(b, now);
                stop_packet_diagnostics(b, now);
                if (b->seek_inflight && now - b->seek_started_at >= b->load_ms) {
                    if (b->inflight_initial_seek) {
                        fail_as(b, MPV_BACKEND_ERROR_CONTROL, "control", "mpv starting-position seek timed out");
                    } else {
                        copy_text(b->snapshot.control_error, sizeof(b->snapshot.control_error), "mpv seek completion timed out");
                        /* Discard request IDs so late replies cannot complete
                         * or reject a subsequent seek. */
                        for (size_t i = 0; i < b->request_count; ) {
                            if (b->requests[i].seek_serial == b->seek_inflight)
                                b->requests[i] = b->requests[--b->request_count];
                            else ++i;
                        }
                        finish_seek(b);
                    }
                }
                apply_controls(b);
            }
        }
        flush_ipc(b);
        reap_child(b);
    }
    pthread_mutex_unlock(&b->lock);
}

void mpv_backend_snapshot(mpv_backend_t *b, mpv_backend_snapshot_t *out)
{
    if (!b || !out) return;
    pthread_mutex_lock(&b->lock);
    *out = b->snapshot;
    out->terminal_reports_dropped = b->terminal_reports_dropped;
    observation_ages(out);
    pthread_mutex_unlock(&b->lock);
}

bool mpv_backend_take_terminal_snapshot(mpv_backend_t *b, mpv_backend_snapshot_t *out)
{
    if (!b || !out) return false;
    pthread_mutex_lock(&b->lock);
    bool found = b->terminal_count > 0;
    if (found) {
        *out = b->terminal_reports[b->terminal_first];
        memset(&b->terminal_reports[b->terminal_first], 0, sizeof(*out));
        b->terminal_first = (b->terminal_first + 1) % (sizeof(b->terminal_reports)/sizeof(b->terminal_reports[0]));
        --b->terminal_count;
        out->terminal_reports_dropped = b->terminal_reports_dropped;
    }
    pthread_mutex_unlock(&b->lock);
    return found;
}

void mpv_backend_shutdown(mpv_backend_t *b)
{
    if (!b) return;
    pthread_mutex_lock(&b->lock);
    b->closing = true;
    b->want_session = false;
    b->stop_requested = b->pid > 0;
    memset(b->url, 0, sizeof(b->url));
    b->snapshot.ready = false;
    b->snapshot.seeking = false;
    b->seek_dirty = false;
    b->seek_inflight = 0;
    if (b->snapshot.state != MPV_BACKEND_FAILED && b->snapshot.state != MPV_BACKEND_ENDED)
        b->snapshot.state = b->pid > 0 ? MPV_BACKEND_STOPPING : MPV_BACKEND_IDLE;
    pthread_mutex_unlock(&b->lock);
}

bool mpv_backend_destroy(mpv_backend_t *b)
{
    if (!b) return true;
    pthread_mutex_lock(&b->lock);
    if (b->pid || b->want_session) { pthread_mutex_unlock(&b->lock); return false; }
    close_ipc(b);
    pthread_mutex_unlock(&b->lock);
    pthread_mutex_destroy(&b->lock);
    memset(b, 0, sizeof(*b));
    free(b);
    return true;
}

bool mpv_backend_preflight(const mpv_backend_config_t *config,
                           char *error, size_t error_size)
{
    mpv_backend_t *b = mpv_backend_create(config, error, error_size);
    if (!b) return false;
    b->preflight = true;
    b->want_session = true;
    b->snapshot.generation = 1;
    b->snapshot.state = MPV_BACKEND_STARTING;
    bool success = false;
    uint64_t end = now_ms() + b->startup_ms + 100;
    struct timespec tick = {0, 5000000};
    do {
        mpv_backend_poll(b);
        if (b->ipc_ready) { success = true; break; }
        if (b->snapshot.state == MPV_BACKEND_FAILED) break;
        nanosleep(&tick, NULL);
    } while (now_ms() < end);
    if (!success) copy_text(error, error_size, b->snapshot.error[0] ? b->snapshot.error : "mpv IPC preflight timed out");
    mpv_backend_shutdown(b);
    end = now_ms() + 3 * b->stop_ms + 200;
    while (b->pid && now_ms() < end) {
        mpv_backend_poll(b);
        nanosleep(&tick, NULL);
    }
    if (b->pid) {
        /* No playback/device opens were requested in preflight. Retaining
         * ownership is safer than pretending an uninterruptible child died.
         * The service cgroup must terminate remaining children on exit. */
        copy_text(error, error_size, "mpv preflight child did not exit; service cleanup is required");
        return false;
    }
    mpv_backend_destroy(b);
    return success;
}
