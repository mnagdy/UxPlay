#define _GNU_SOURCE
#include "playback_trace_file.h"

#ifdef __linux__
#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TRACE_LIMIT (1024U * 1024U)
#define MESSAGE_LIMIT 6144U
#define CURRENT "playback-trace.jsonl"
#define PREVIOUS "playback-trace.previous.jsonl"
#define LOCK_FILE ".playback-trace.lock"

static pthread_mutex_t trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *test_directory;
static size_t file_limit = TRACE_LIMIT;

static bool word_in(const char *word, const char *list) {
    size_t length = strlen(word);
    for (const char *p = list; *p;) {
        const char *end = strchr(p, ' ');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == length && !memcmp(p, word, n)) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

/* This is an independent privacy boundary before anything reaches disk.
 * Unknown fields/enum values fail closed. The exporter validates again. */
static const char *bool_keys =
    "ready child paused seeking position_known audio_position_known cache_known cache_bytes_known "
    "cache_speed_known avsync_known file_error_known exit_known error_reports_known actual_paused_known "
    "actual_paused core_idle_known core_idle cache_eof_known cache_eof cache_underrun_known cache_underrun "
    "cache_idle_known cache_idle selected_video_known selected_audio_known decoded_parameters_known "
    "http_status_known terminal_draining terminal_report video_reader_pts_known video_cache_end_known "
    "video_cache_duration_known audio_reader_pts_known audio_cache_end_known audio_cache_duration_known accepted "
    "packet_capture_enabled packet_capture_active packet_capture_complete";
static const char *unsigned_keys =
    "session audio_rate audio_channels video_decode_errors audio_decode_errors demux_errors network_errors "
    "video_output_errors audio_output_errors other_errors metadata_errors schema monotonic_ms decoded_width "
    "decoded_height warning_reports video_decode_warnings missing_reference_warnings invalid_data_warnings "
    "timestamp_warnings audio_output_warnings unknown_warnings reason_at_ms http_error_count hls_init_failures "
    "hls_segment_failures hls_reload_failures child_pid child_generation terminal_reports_dropped log_overflows "
    "event_overflows log_text_rejected ipc_read_budget_exhaustions http_status previous elapsed_ms history_write_failures "
    "packet_capture_started_ms packet_capture_ended_ms packet_capture_errors packet_log_rejected "
    "audio_packets video_packets audio_packet_bytes video_packet_bytes audio_packets_with_pts video_packets_with_pts "
    "audio_packet_first_at_ms audio_packet_last_at_ms video_packet_first_at_ms video_packet_last_at_ms "
    "last_warning_at_ms packet_corrupt_warnings pes_mismatch_warnings demux_read_warnings";
static const char *signed_keys =
    "cache_bytes cache_bytes_per_second decoder_drops output_drops error_code file_error_code exit_code signal "
    "selected_video_id selected_audio_id video_observation_age_ms audio_observation_age_ms cache_observation_age_ms";
static const char *decimal_keys =
    "start position audio_position cache_seconds avsync video_reader_pts video_cache_end video_cache_duration "
    "audio_reader_pts audio_cache_end audio_cache_duration rate volume value "
    "audio_packet_first_pts audio_packet_last_pts video_packet_first_pts video_packet_last_pts";
struct enum_field { const char *key; const char *values; };
static const struct enum_field enums[] = {
    {"state", "idle starting loading buffering playing paused stopping ended failed unknown"},
    {"request", "queued rejected"},
    {"route", "direct-http playlist-cache mirroring audio-only none unknown"},
    {"codec", "h264 hevc h265 vp8 vp9 av1 mpeg2video mpeg4 aac alac mp3 opus vorbis flac ac3 eac3 pcm other unknown none"},
    {"video_decoder", "h264 h264_v4l2m2m hevc hevc_v4l2m2m h265 vp8 vp9 av1 mpeg2video mpeg4 other unknown none"},
    {"audio_decoder", "aac aac_fixed mp3 mp3float ac3 eac3 alac flac opus vorbis pcm_s16le pcm_s24le unknown other"},
    {"pixel_format", "yuv420p yuv420p10 yuv420p10le yuv422p yuv422p10 yuv422p10le yuv444p yuv444p10 yuv444p10le nv12 drm_prime unknown other"},
    {"hwdec", "no drm drm-copy v4l2m2m v4l2m2m-copy vaapi vaapi-copy vdpau vdpau-copy unknown other"},
    {"vo", "gpu gpu-next drm x11 xv null unknown other"},
    {"ao", "alsa pulse pipewire jack null unknown other"},
    {"end_reason", "eof stop quit error redirect unknown other"},
    {"decode_policy", "software pi4-safe pi4-hevc-experimental"},
    {"control_error", "none seek-rejected volume-rejected overlay-rejected volume-ack-timeout overlay-ack-timeout seek-timeout other"},
    {"failure_stage", "unknown other startup ipc load playback control stop audio-output video-output"},
    {"diagnostic_reason", "http-client-error http-server-error hls-init-failure hls-segment-failure hls-reload-failure missing-reference video-decode-error invalid-data timestamp-discontinuity audio-output-underrun audio-output-init-error audio-decode-error pes-size-mismatch packet-corrupt demux-read-error hls-expired-segments hls-sequence-change virtual-terminal-unavailable frame-present-failure drm-display-failure unknown other"},
    {"last_warning_reason", "http-client-error http-server-error hls-init-failure hls-segment-failure hls-reload-failure missing-reference video-decode-error invalid-data timestamp-discontinuity audio-output-underrun audio-output-init-error audio-decode-error pes-size-mismatch packet-corrupt demux-read-error hls-expired-segments hls-sequence-change virtual-terminal-unavailable frame-present-failure drm-display-failure unknown other"},
    {"last_warning_stage", "video-decode audio-decode video-output audio-output demux source unclassified unknown"},
    {"action", "pause resume seek volume stop playlist-remove rate-ignored playlist-pause handover-stop"},
    {"result", "accepted rejected queued complete failed stale"},
    {"event", "accepted rejected replaced stopped ended failed preparing request source-failed output-release-begin output-release-complete output-release-failed receiver-rebuilt"},
    {"backend", "mpv gstreamer"},
    {"reason", "new-request sender-stop replacement eof error invalid-request stale-control shutdown unknown"}
};

static bool valid_number(const char *value, bool negative, bool decimal) {
    const char *p = value;
    if (negative && *p == '-') ++p;
    size_t digits = 0;
    while (*p >= '0' && *p <= '9') { ++p; ++digits; }
    if (!digits || digits > (decimal ? 15U : 20U)) return false;
    if (decimal && *p == '.') {
        ++p;
        size_t fraction = 0;
        while (*p >= '0' && *p <= '9') { ++p; ++fraction; }
        if (!fraction || fraction > 9) return false;
    }
    if (*p) return false;
    errno = 0;
    if (decimal) {
        long double number = strtold(value, NULL);
        return !errno && isfinite(number) && fabsl(number) <= 1e15L;
    }
    if (negative) (void)strtoll(value, NULL, 10);
    else (void)strtoull(value, NULL, 10);
    return !errno;
}

static bool valid_value(const char *key, const char *value) {
    if (word_in(key, bool_keys)) return !strcmp(value, "0") || !strcmp(value, "1");
    if (!strcmp(key, "schema")) return !strcmp(value, "2");
    if (!strcmp(key, "size")) {
        unsigned width, height; char trailing;
        return sscanf(value, "%5ux%5u%c", &width, &height, &trailing) == 2 &&
               width <= 32768 && height <= 32768 &&
               strspn(value, "0123456789x") == strlen(value);
    }
    if (word_in(key, unsigned_keys)) return valid_number(value, false, false);
    if (word_in(key, signed_keys)) return valid_number(value, true, false);
    if (word_in(key, decimal_keys)) return valid_number(value, true, true);
    for (size_t i = 0; i < G_N_ELEMENTS(enums); ++i)
        if (!strcmp(key, enums[i].key)) return word_in(value, enums[i].values);
    return false;
}

static bool valid_message(const char *text) {
    if (!text || !*text || strnlen(text, MESSAGE_LIMIT + 1) > MESSAGE_LIMIT) return false;
    const char *body; int kind;
    if (!strncmp(text, "MPV playback: ", 14)) { body = text + 14; kind = 0; }
    else if (!strncmp(text, "MPV control: ", 13)) { body = text + 13; kind = 1; }
    else if (!strncmp(text, "Playback session: ", 18)) { body = text + 18; kind = 2; }
    else return false;
    /* JSON encoding below is safe only because every byte is in this grammar. */
    for (const unsigned char *p = (const unsigned char *)body; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-' || *p == '.' || *p == '=' || *p == ' ')) return false;
    char copy[MESSAGE_LIMIT + 1]; strcpy(copy, body);
    char *seen[192]; size_t count = 0;
    bool session = false, discriminator = false;
    char *p = copy;
    while (*p) {
        if (count == G_N_ELEMENTS(seen) || *p == ' ') return false;
        char *end = strchr(p, ' ');
        if (end) { *end = 0; if (!end[1]) return false; }
        char *equals = strchr(p, '=');
        if (!equals || !equals[1]) return false;
        *equals = 0;
        for (size_t i = 0; i < count; ++i) if (!strcmp(seen[i], p)) return false;
        seen[count++] = p;
        if (!valid_value(p, equals + 1)) return false;
        if (!strcmp(p, "session")) session = strtoull(equals + 1, NULL, 10) > 0;
        if ((kind == 0 && (!strcmp(p, "state") || !strcmp(p, "request"))) ||
            (kind == 1 && !strcmp(p, "action")) || (kind == 2 && !strcmp(p, "event"))) discriminator = true;
        if (!end) break;
        p = end + 1;
    }
    return session && discriminator;
}

/* Open each component without following symlinks. Root-owned sticky /tmp is
 * allowed as an ancestor for tests; the final uxplay directory is private. */
static int open_directory(const char *path) {
    if (!path || path[0] != '/' || strlen(path) > 4096) return -1;
    char *copy = strdup(path), *save = NULL;
    if (!copy) return -1;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (char *part = strtok_r(copy, "/", &save); part && fd >= 0; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); fd = -1; break; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT) {
            if (mkdirat(fd, part, 0700) && errno != EEXIST) { close(fd); fd = -1; break; }
            next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        close(fd); fd = next;
        struct stat st;
        if (fd >= 0 && (fstat(fd, &st) || (st.st_uid != getuid() && st.st_uid != 0) ||
            ((st.st_mode & 0022) && !(st.st_uid == 0 && (st.st_mode & S_ISVTX))))) {
            close(fd); fd = -1;
        }
    }
    free(copy);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != getuid() || (st.st_mode & 0777) != 0700)) {
        close(fd); fd = -1;
    }
    return fd;
}

static bool safe_file(int fd, struct stat *st) {
    return !fstat(fd, st) && S_ISREG(st->st_mode) && st->st_uid == getuid() &&
           st->st_nlink == 1 && (st->st_mode & 0777) == 0600;
}

static bool read_boot_id(char result[33]) {
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    char raw[38]; ssize_t n = read(fd, raw, sizeof(raw)); close(fd);
    if (n < 36 || n > 37) return false;
    size_t at = 0;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (raw[i] != '-') return false; }
        else if ((raw[i] >= '0' && raw[i] <= '9') || (raw[i] >= 'a' && raw[i] <= 'f')) result[at++] = raw[i];
        else return false;
    }
    result[at] = 0;
    return at == 32;
}

bool playback_trace_file_set_directory_for_tests(const char *directory, size_t max_bytes) {
    if (directory && (directory[0] != '/' || max_bytes < 256 || max_bytes > TRACE_LIMIT)) return false;
    if (pthread_mutex_trylock(&trace_mutex)) return false;
    char *copy = directory ? strdup(directory) : NULL;
    if (directory && !copy) { pthread_mutex_unlock(&trace_mutex); return false; }
    free(test_directory); test_directory = copy;
    file_limit = directory ? max_bytes : TRACE_LIMIT;
    pthread_mutex_unlock(&trace_mutex);
    return true;
}

bool playback_trace_file_write(const char *text) {
    if (!valid_message(text) || pthread_mutex_trylock(&trace_mutex)) return false;
    bool result = false;
    int dir = -1, lock = -1, file = -1;
    char *path = NULL;
    char boot[33], record[MESSAGE_LIMIT + 256];
    struct timespec real, mono;
    if (!read_boot_id(boot) || clock_gettime(CLOCK_REALTIME, &real) || clock_gettime(CLOCK_MONOTONIC, &mono)) goto done;
    int length = snprintf(record, sizeof(record),
        "{\"MESSAGE\":\"%s\",\"_BOOT_ID\":\"%s\",\"_PID\":\"%lu\",\"__REALTIME_TIMESTAMP\":\"%llu\",\"__MONOTONIC_TIMESTAMP\":\"%llu\"}\n",
        text, boot, (unsigned long)getpid(),
        (unsigned long long)real.tv_sec * 1000000ULL + (unsigned long long)real.tv_nsec / 1000ULL,
        (unsigned long long)mono.tv_sec * 1000000ULL + (unsigned long long)mono.tv_nsec / 1000ULL);
    if (length <= 0 || (size_t)length >= sizeof(record) || (size_t)length > file_limit) goto done;
    if (test_directory) path = g_strdup(test_directory);
    else {
#if GLIB_CHECK_VERSION(2, 72, 0)
        path = g_build_filename(g_get_user_state_dir(), "uxplay", NULL);
#else
        const char *state = g_getenv("XDG_STATE_HOME");
        path = state && state[0] == '/' ? g_build_filename(state, "uxplay", NULL) :
               g_build_filename(g_get_home_dir(), ".local", "state", "uxplay", NULL);
#endif
    }
    dir = open_directory(path);
    if (dir < 0) goto done;
    lock = openat(dir, LOCK_FILE, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    struct stat st;
    if (lock < 0 || !safe_file(lock, &st) || flock(lock, LOCK_EX | LOCK_NB)) goto done;
    file = openat(dir, CURRENT, O_RDWR | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (file < 0 || !safe_file(file, &st) || st.st_size < 0) goto done;
    if ((uint64_t)st.st_size > file_limit) goto done;
    if (st.st_size) {
        char last;
        if (pread(file, &last, 1, st.st_size - 1) != 1) goto done;
        if (last != '\n') {
            /* Repair only a bounded incomplete tail left by interrupted I/O;
             * otherwise the next good record would be lost with that tail. */
            char tail[MESSAGE_LIMIT + 256];
            size_t size = (uint64_t)st.st_size < sizeof(tail) ? (size_t)st.st_size : sizeof(tail);
            off_t start = st.st_size - (off_t)size, end = -1;
            if (pread(file, tail, size, start) != (ssize_t)size) goto done;
            for (size_t i = size; i > 0; --i) if (tail[i - 1] == '\n') { end = start + (off_t)i; break; }
            if (end < 0) { if (start) goto done; end = 0; }
            if (ftruncate(file, end)) goto done;
            st.st_size = end;
        }
    }
    if ((uint64_t)st.st_size + (size_t)length > file_limit) {
        struct stat previous;
        if (!fstatat(dir, PREVIOUS, &previous, AT_SYMLINK_NOFOLLOW)) {
            if (!S_ISREG(previous.st_mode) || previous.st_uid != getuid() || previous.st_nlink != 1 ||
                (previous.st_mode & 0777) != 0600) goto done;
        } else if (errno != ENOENT) goto done;
        if (renameat(dir, CURRENT, dir, PREVIOUS)) goto done;
        close(file); file = openat(dir, CURRENT, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
        if (file < 0 || !safe_file(file, &st)) goto done;
    }
    ssize_t wrote = write(file, record, (size_t)length);
    if (wrote == length) result = true;
    else if (wrote > 0) (void)ftruncate(file, st.st_size); /* Do not retain a torn JSON line. */
done:
    if (file >= 0) close(file);
    if (lock >= 0) close(lock);
    if (dir >= 0) close(dir);
    g_free(path);
    pthread_mutex_unlock(&trace_mutex);
    return result;
}
#else
/* Persistent receiver diagnostics currently use Linux boot/procfs identity. */
bool playback_trace_file_write(const char *text) { (void)text; return false; }
bool playback_trace_file_set_directory_for_tests(const char *directory, size_t max_bytes) {
    (void)directory; (void)max_bytes; return false;
}
#endif
