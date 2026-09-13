/*
 * UxPlay direct playback state and time conversion helpers.
 * Copyright (C) 2026 UxPlay contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef DIRECT_PLAYBACK_STATE_H
#define DIRECT_PLAYBACK_STATE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DIRECT_PLAYBACK_STOPPED,
    DIRECT_PLAYBACK_PAUSED,
    DIRECT_PLAYBACK_PLAYING
} direct_playback_target_t;

/* The owner serializes all access; these helpers do not provide locking. */
typedef struct {
    direct_playback_target_t intent;
    bool buffering;
    bool live;
    bool preroll_complete;
    bool failed;
} direct_playback_state_t;

/* Called for a new play request, not when rebuilding its renderer. */
static inline void direct_playback_state_reset(direct_playback_state_t *state) {
    state->intent = DIRECT_PLAYBACK_PLAYING;
    state->buffering = false;
    state->live = false;
    state->preroll_complete = false;
    state->failed = false;
}

/* Failure is terminal for this request; only a new request resets it. */
static inline void direct_playback_state_fail(direct_playback_state_t *state) {
    state->failed = true;
    state->intent = DIRECT_PLAYBACK_STOPPED;
    state->buffering = false;
    state->live = false;
    state->preroll_complete = false;
}

static inline direct_playback_target_t
direct_playback_state_target(const direct_playback_state_t *state) {
    if (state->failed) return DIRECT_PLAYBACK_STOPPED;
    if (state->intent != DIRECT_PLAYBACK_PLAYING) {
        return state->intent;
    }
    /* A GStreamer live source cannot preroll or fill buffers while paused. */
    if (state->live || (state->preroll_complete && !state->buffering)) {
        return DIRECT_PLAYBACK_PLAYING;
    }
    return DIRECT_PLAYBACK_PAUSED;
}

/* Buffering reports never change the user's requested playback state. */
static inline bool
direct_playback_state_buffer(direct_playback_state_t *state, int percent) {
    if (state->failed || percent < 0 || percent > 100) {
        return false;
    }
    state->buffering = percent < 100;
    return true;
}

/* Convert only after range checks: casting NaN or an oversized double to a
 * signed integer is undefined. INT64_MAX rounds to 2^63 as a double, so the
 * exclusive 2^63 boundary also rejects that rounded value. Rejections leave
 * the caller's output unchanged. Valid fractional nanoseconds are truncated.
 */
static inline bool direct_playback_seconds_to_ns(double seconds, int64_t *ns) {
    if (!ns || !isfinite(seconds) || seconds < 0.0) {
        return false;
    }
    double nanoseconds = seconds * 1000000000.0;
    if (!isfinite(nanoseconds) || nanoseconds >= 0x1p63) {
        return false;
    }
    *ns = (int64_t) nanoseconds;
    return true;
}

#endif /* DIRECT_PLAYBACK_STATE_H */
