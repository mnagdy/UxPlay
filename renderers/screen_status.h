/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SCREEN_STATUS_H
#define SCREEN_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SCREEN_INFO_OFF, SCREEN_INFO_STATUS, SCREEN_INFO_DEBUG } screen_info_mode_t;
typedef enum { SCREEN_SESSION_NONE, SCREEN_SESSION_DIRECT_VIDEO,
               SCREEN_SESSION_MIRRORING, SCREEN_SESSION_AUDIO_ONLY } screen_session_kind_t;
typedef enum { SCREEN_ROUTE_UNKNOWN, SCREEN_ROUTE_DIRECT_HTTP,
               SCREEN_ROUTE_PLAYLIST_CACHE, SCREEN_ROUTE_RTP } screen_source_route_t;
typedef enum {
    SCREEN_STATE_STARTING, SCREEN_STATE_WAITING_NETWORK, SCREEN_STATE_READY,
    SCREEN_STATE_INCOMING, SCREEN_STATE_PREPARING, SCREEN_STATE_OPENING,
    SCREEN_STATE_BUFFERING, SCREEN_STATE_WAITING_DATA, SCREEN_STATE_PLAYING,
    SCREEN_STATE_PAUSED, SCREEN_STATE_SEEKING, SCREEN_STATE_SWITCHING,
    SCREEN_STATE_STOPPING, SCREEN_STATE_FAILED, SCREEN_STATE_UNAVAILABLE,
    SCREEN_STATE_RECOVERY_REQUIRED
} screen_state_t;
typedef enum {
    SCREEN_EVENT_PREPARING, SCREEN_EVENT_OPENING, SCREEN_EVENT_BUFFERING,
    SCREEN_EVENT_WAITING_DATA, SCREEN_EVENT_OUTPUT_PROGRESS,
    SCREEN_EVENT_PAUSED, SCREEN_EVENT_RESUMED, SCREEN_EVENT_SEEKING,
    SCREEN_EVENT_SEEK_COMPLETE, SCREEN_EVENT_SWITCHING, SCREEN_EVENT_STOPPING,
    SCREEN_EVENT_STOPPED, SCREEN_EVENT_RECOVERED, SCREEN_EVENT_RECOVERY_REQUIRED
} screen_status_event_t;
typedef enum {
    SCREEN_ERROR_NONE, SCREEN_ERROR_NETWORK, SCREEN_ERROR_PLAYLIST,
    SCREEN_ERROR_SOURCE, SCREEN_ERROR_DECODER, SCREEN_ERROR_OUTPUT,
    SCREEN_ERROR_BACKEND, SCREEN_ERROR_TIMEOUT, SCREEN_ERROR_INTERNAL
} screen_status_error_t;

typedef struct {
    char backend[32], codec[32], profile[32], decode_policy[32];
    char decoder[48], memory[32], pixel_format[32], overlay_reason[48];
    unsigned width, height, fps_num, fps_den, bit_depth;
    unsigned output_width, output_height;
    bool hardware_known, hardware_active;
    bool decoded_parameters_known; /* Decoded-format property available; not a frame count. */
    bool overlay_known, overlay_supported, dropped_known;
    bool output_dropped_known;
    bool input_buffers_known, decoded_buffers_known, output_buffers_known;
    uint64_t input_buffers, decoded_buffers, output_buffers, dropped_frames;
    uint64_t output_dropped_frames; /* Video output drops, distinct from decoder drops. */
} screen_status_video_t;

typedef struct {
    char codec[32], output[48];
    unsigned channels, sample_rate;
    bool input_buffers_known, decoded_buffers_known, output_buffers_known;
    uint64_t input_buffers, decoded_buffers, output_buffers;
    bool volume_known, muted;
    double volume;
} screen_status_audio_t;

typedef struct {
    bool position_known, duration_known, buffer_known, live_known, live;
    double position_seconds, duration_seconds;
    int buffer_percent;
    bool cache_seconds_known, avsync_known, audio_position_known;
    double cache_seconds, avsync, audio_position;
    bool actual_paused_known, actual_paused, core_idle_known, core_idle;
    bool cache_eof_known, cache_eof, cache_underrun_known, cache_underrun;
    bool cache_idle_known, cache_idle;
    bool diagnostics_known;
    char diagnostic_stage[32]; /* Fixed backend stage label, never raw log text. */
    char diagnostic_reason[48];
    char last_warning_stage[32], last_warning_reason[48];
    bool packet_capture_enabled, packet_capture_active, packet_capture_complete;
    uint64_t video_packets, audio_packets;
    bool last_http_status_known;
    int last_http_status;
    uint64_t video_decode_errors, audio_decode_errors, demux_errors, network_errors;
    uint64_t video_output_errors, audio_output_errors, unclassified_errors;
    uint64_t log_warnings, video_decode_warnings, missing_reference_warnings, invalid_data_warnings;
    uint64_t timestamp_warnings, audio_output_warnings, unknown_warnings;
} screen_status_progress_t;

typedef struct {
    screen_state_t state;
    uint64_t elapsed_ms;
} screen_status_milestone_t;

#define SCREEN_STATUS_HISTORY 8
typedef struct {
    screen_info_mode_t mode;
    screen_state_t state;
    screen_session_kind_t kind;
    screen_source_route_t route;
    uint64_t generation, revision;
    int64_t requested_at_us, changed_at_us, last_progress_at_us;
    bool network_ready, listener_ready, discovery_ready, backend_ready, output_released;
    bool session_active, output_observed, pause_requested;
    char receiver_name[96], sender[64];
    screen_status_video_t video;
    screen_status_audio_t audio;
    screen_status_progress_t progress;
    screen_status_error_t error, last_error;
    char error_detail[160];         /* Fixed, sanitized failure explanation. */
    uint64_t last_error_generation;
    screen_status_milestone_t history[SCREEN_STATUS_HISTORY];
    unsigned history_count;
} screen_status_snapshot_t;

/* Global, internally serialized model. init resets content but never reuses a
 * generation, including across reinitialization in the same receiver process. */
void screen_status_init(screen_info_mode_t mode, const char *receiver_name);
void screen_status_set_mode(screen_info_mode_t mode);
screen_info_mode_t screen_status_get_mode(void);
bool screen_status_parse_mode(const char *value, screen_info_mode_t *mode);
void screen_status_set_readiness(bool network, bool listener, bool discovery,
                                 bool backend, bool output_released);
uint64_t screen_status_begin_session(screen_session_kind_t kind,
                                     screen_source_route_t route, const char *sender);
bool screen_status_event(uint64_t generation, screen_status_event_t event);
bool screen_status_fail(uint64_t generation, screen_status_error_t error);
/* For fixed backend diagnostics, never raw player logs or source metadata. */
bool screen_status_fail_detail(uint64_t generation, screen_status_error_t error,
                               const char *detail);
bool screen_status_set_video(uint64_t generation, const screen_status_video_t *video);
bool screen_status_set_audio(uint64_t generation, const screen_status_audio_t *audio);
bool screen_status_set_progress(uint64_t generation, const screen_status_progress_t *progress);
void screen_status_get_snapshot(screen_status_snapshot_t *snapshot);

/* Plain, bounded UTF-8 text only. Each presenter must escape the markup syntax
 * of its output API; stream text is never used as format strings or commands.
 * compact STATUS output is empty while playing or ready; DEBUG is persistent.
 * Returns the number of bytes stored, excluding the terminator. */
size_t screen_status_format(const screen_status_snapshot_t *snapshot,
                           char *text, size_t capacity, bool compact);
const char *screen_status_state_name(screen_state_t state, screen_session_kind_t kind);

#ifdef __cplusplus
}
#endif
#endif
