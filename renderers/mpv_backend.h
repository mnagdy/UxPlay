/* Optional AirPlay URL/HLS player. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UXPLAY_MPV_BACKEND_H
#define UXPLAY_MPV_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mpv_backend_s mpv_backend_t;

typedef enum {
    MPV_DECODE_SOFTWARE = 0,
    MPV_DECODE_PI4_SAFE,
    MPV_DECODE_PI4_HEVC_EXPERIMENTAL
} mpv_decode_policy_t;

typedef struct {
    const char *executable;          /* NULL: search PATH for mpv. No shell. */
    const char *video_output;        /* NULL: mpv default; e.g. gpu or null. */
    const char *gpu_context;         /* e.g. drm */
    const char *gpu_api;             /* e.g. opengl */
    const char *drm_device;
    const char *drm_connector;
    const char *audio_device;        /* mpv device name, e.g. alsa/... */
    bool disable_audio;              /* preserve receiver -a behavior */
    bool fast_rendering;             /* opt-in mpv fast profile; decoder policy is separate */
    bool packet_diagnostics;         /* bounded selected-queue evidence, debug only */
    unsigned packet_diagnostic_window_ms; /* zero: 30000; maximum 30000 */
    mpv_decode_policy_t decode_policy;
    /* PI4_SAFE requires an explicitly qualified v4l2m2m-copy or v4l2m2m
     * path. This does not itself establish qualification on the device. */
    const char *qualified_h264_hwdec;
    unsigned startup_timeout_ms;     /* zero: 3000 */
    unsigned load_timeout_ms;        /* zero: 30000 */
    unsigned stop_timeout_ms;        /* zero: 1000, per graceful/TERM step */
} mpv_backend_config_t;

typedef enum {
    MPV_BACKEND_IDLE = 0,
    MPV_BACKEND_STARTING,
    MPV_BACKEND_LOADING,
    MPV_BACKEND_BUFFERING,
    MPV_BACKEND_PLAYING,
    MPV_BACKEND_PAUSED,
    MPV_BACKEND_STOPPING,
    MPV_BACKEND_ENDED,
    MPV_BACKEND_FAILED
} mpv_backend_state_t;

typedef enum {
    MPV_BACKEND_ERROR_NONE = 0,
    MPV_BACKEND_ERROR_STARTUP,
    MPV_BACKEND_ERROR_IPC,
    MPV_BACKEND_ERROR_LOAD,
    MPV_BACKEND_ERROR_PLAYBACK,
    MPV_BACKEND_ERROR_AUDIO_OUTPUT,
    MPV_BACKEND_ERROR_VIDEO_OUTPUT,
    MPV_BACKEND_ERROR_NO_MEDIA,
    MPV_BACKEND_ERROR_FORMAT,
    MPV_BACKEND_ERROR_CONTROL,
    MPV_BACKEND_ERROR_STOP
} mpv_backend_error_t;

typedef struct {
    bool reader_pts_known, cache_end_known, cache_duration_known;
    double reader_pts, cache_end, cache_duration;
} mpv_backend_cache_stream_t;

typedef struct {
    uint64_t packets, bytes, pts_packets;
    double first_pts, last_pts;      /* known iff pts_packets > 0; not decoded data */
    uint64_t first_at_ms, last_at_ms;
} mpv_backend_packet_stream_t;

typedef struct {
    uint64_t generation;
    mpv_backend_state_t state;
    bool child_alive;               /* supervisor owns a child, possibly retiring */
    uint64_t child_generation;       /* actual child, differs while replacement waits */
    int child_pid;                  /* final reports retain the reaped PID */
    bool terminal_diagnostics_draining;
    bool recovery_required;         /* unreaped child after TERM/KILL deadlines */
    bool ready;                     /* file-loaded; NOT physical HDMI proof */
    bool playback_restarted;        /* playback-restart event observed */
    bool requested_paused;
    bool actual_paused_known, actual_paused;
    bool core_idle_known, core_idle;
    bool seeking;                   /* latest accepted seek is not complete */
    bool buffering;
    bool seekable_known;
    bool seekable;
    bool position_known;
    bool duration_known;            /* false is valid for live input */
    double position;
    double duration;                /* never an end-of-stream sentinel */
    bool cache_range_known;
    double cache_start;
    double cache_end;
    bool cache_duration_known;
    double cache_duration;           /* approximate buffered seconds */
    bool cache_bytes_known;
    int64_t cache_bytes;             /* approximate forward packet bytes */
    bool cache_speed_known;
    int64_t cache_speed;             /* estimated source bytes/second */
    bool cache_buffering_known;
    double cache_buffering_percent;  /* fill until unpause, not whole file */
    bool cache_eof_known, cache_eof;
    bool cache_underrun_known, cache_underrun;
    bool cache_idle_known, cache_idle;
    mpv_backend_cache_stream_t cache_video, cache_audio;
    bool selected_video_known, selected_audio_known;
    int64_t selected_video_id, selected_audio_id; /* -1: no selected track */
    bool decoded_parameters_known;   /* video-dec-params, not container hint */
    int decoded_width, decoded_height;
    char decoded_pixel_format[64];
    /* Last successful decoded-video/audio-format/cache observation. Unknown
     * updates clear current availability but preserve these historical ages. */
    uint64_t video_observed_at_ms, audio_observed_at_ms, cache_observed_at_ms;
    int64_t video_observation_age_ms, audio_observation_age_ms, cache_observation_age_ms;
    bool avsync_known;
    double avsync;
    bool audio_position_known;
    double audio_position;          /* audio-pts, not audible-output proof */
    int width;
    int height;
    double fps;
    double video_bitrate;
    double audio_bitrate;
    int audio_samplerate;
    int audio_channels;
    int64_t dropped_frames;          /* decoder drops; -1: unavailable */
    int64_t output_dropped_frames;   /* VO drops; -1: unavailable */
    char video_codec[64];
    char video_decoder[96];
    char video_profile[64];
    char audio_codec[64];
    char audio_decoder[64];
    char hwdec_current[64];
    char video_output[64];
    char audio_output[64];
    char pixel_format[64];
    char hw_pixel_format[64];
    mpv_backend_error_t error_code;
    char failure_stage[32];          /* fixed adapter stage, never raw text */
    char end_reason[16];             /* allowlisted mpv end-file reason */
    char file_error[64];             /* allowlisted mpv API error text */
    bool file_error_code_known;
    int file_error_code;             /* mapped MPV_ERROR, not HTTP/FFmpeg code */
    bool child_exit_known;
    int child_exit_code;             /* -1 if terminated by signal */
    int child_signal;                /* 0 unless signalled */
    bool log_messages_active;
    uint64_t metadata_observation_errors;
    /* Diagnostic error-log module counts, not proof of fatal failure.
     * No log text or arbitrary module prefix is retained or forwarded. */
    char diagnostic_stage[32];
    uint64_t log_errors, video_decode_errors, audio_decode_errors;
    uint64_t demux_errors, network_errors, video_output_errors;
    uint64_t audio_output_errors, unclassified_errors;
    uint64_t log_warnings;
    uint64_t video_decode_warnings, missing_reference_warnings, invalid_data_warnings;
    uint64_t timestamp_warnings, audio_output_warnings, unknown_warnings;
    char diagnostic_reason[48];      /* fixed classified reason, never log text */
    uint64_t diagnostic_reason_at_ms;
    char last_warning_stage[32], last_warning_reason[48];
    uint64_t last_warning_at_ms;
    uint64_t packet_corrupt_warnings, pes_mismatch_warnings, demux_read_warnings;
    bool packet_diagnostics_enabled, packet_diagnostics_active, packet_diagnostics_complete;
    uint64_t packet_capture_started_at_ms, packet_capture_ended_at_ms;
    uint64_t packet_capture_errors, packet_log_rejected;
    /* Counts packets appended to selected queues of this media type. Multiple
     * selected tracks of one type are combined. Capture loss makes these lower
     * bounds; absence is evidence only when capture coverage is complete. */
    mpv_backend_packet_stream_t packet_video, packet_audio;
    bool last_http_status_known;
    int last_http_status;
    uint64_t http_error_count, hls_init_failures, hls_segment_failures, hls_reload_failures;
    uint64_t log_overflows, event_overflows; /* nonzero: evidence incomplete */
    uint64_t log_text_rejected, ipc_read_budget_exhaustions;
    uint64_t terminal_reports_dropped; /* bounded report queue overflow */
    char error[160];                /* fixed sanitized diagnostic, no URLs */
    char control_error[96];         /* nonfatal rejected seek/control */
} mpv_backend_snapshot_t;

/* No device acquisition in create. All config strings are copied. */
mpv_backend_t *mpv_backend_create(const mpv_backend_config_t *config,
                                  char *error, size_t error_size);
/* Startup-only check: spawn idle player, check IPC and required properties,
 * request quit, and reap with deadlines. Does not load media or open outputs.
 * May take startup + three stop deadlines. Never call in an HTTP callback. */
bool mpv_backend_preflight(const mpv_backend_config_t *config,
                           char *error, size_t error_size);

/* Thread-safe, bounded, nonblocking enqueue. Generation must increase for
 * replacement; stale controls return false. Only HTTP(S) media is accepted.
 * A replacement waits for the previous child to be reaped before spawning. */
bool mpv_backend_open(mpv_backend_t *backend, uint64_t generation,
                      const char *url, double start_seconds);
bool mpv_backend_pause(mpv_backend_t *backend, uint64_t generation);
bool mpv_backend_resume(mpv_backend_t *backend, uint64_t generation);
bool mpv_backend_seek(mpv_backend_t *backend, uint64_t generation, double seconds);
/* volume is linear percent 0..100; protocol dB conversion belongs to caller. */
bool mpv_backend_set_volume(mpv_backend_t *backend, uint64_t generation,
                            double volume, bool mute);
/* Plain diagnostic text, at most 2048 bytes. Rendered with property expansion
 * disabled; valid UTF-8 is preserved, unsafe controls stripped and malformed
 * UTF-8 bytes replaced for OSD safety. Newlines are permitted.
 * Refresh periodically while needed; an empty string clears the message. */
bool mpv_backend_set_osd(mpv_backend_t *backend, uint64_t generation, const char *text);
bool mpv_backend_stop(mpv_backend_t *backend, uint64_t generation);
/* Call regularly (e.g. every 10-50 ms) on the owning main loop. No blocking
 * network I/O or blocking child wait. Work per poll is bounded. */
void mpv_backend_poll(mpv_backend_t *backend);
void mpv_backend_snapshot(mpv_backend_t *backend, mpv_backend_snapshot_t *out);
/* Drain after poll, including during replacement. Each reaped child yields a
 * final generation-correlated report, even after a new open was queued.
 * Bounded queue of four; overflow is explicit in terminal_reports_dropped. */
bool mpv_backend_take_terminal_snapshot(mpv_backend_t *backend, mpv_backend_snapshot_t *out);
/* Nonblocking: disallow future opens, cancel queued replacement, stop child. */
void mpv_backend_shutdown(mpv_backend_t *backend);
/* Destroy after callers have quiesced, shutdown and child_alive == false.
 * Returns false rather than
 * abandoning an unreaped child; keep polling (service cgroup is last resort). */
bool mpv_backend_destroy(mpv_backend_t *backend);

#ifdef __cplusplus
}
#endif
#endif
