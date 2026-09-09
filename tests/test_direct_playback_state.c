/*
 * Regression tests for direct playback control and seek time conversion.
 * Copyright (C) 2026 UxPlay contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <float.h>
#include <inttypes.h>
#include <stdio.h>

#include "../renderers/direct_playback_state.h"

static void test_startup_and_rebuffering(void) {
    direct_playback_state_t state;
    direct_playback_state_reset(&state);
    assert(state.intent == DIRECT_PLAYBACK_PLAYING);
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);

    /* Completing download buffering before preroll is not sufficient. */
    assert(direct_playback_state_buffer(&state, 100));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);
    state.preroll_complete = true;
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PLAYING);

    /* An empty buffer must pause just as a partially filled buffer does. */
    assert(direct_playback_state_buffer(&state, 0));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);
    assert(direct_playback_state_buffer(&state, 50));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);
    assert(direct_playback_state_buffer(&state, 100));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PLAYING);

    /* Sources without BUFFERING messages can play after preroll. */
    direct_playback_state_reset(&state);
    state.preroll_complete = true;
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PLAYING);

    /* Finishing preroll while buffering must not start playback early. */
    direct_playback_state_reset(&state);
    assert(direct_playback_state_buffer(&state, 25));
    state.preroll_complete = true;
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);
}

static void test_user_intent_survives_buffer_events(void) {
    direct_playback_state_t state;
    direct_playback_state_reset(&state);
    state.preroll_complete = true;
    assert(direct_playback_state_buffer(&state, 25));

    state.intent = DIRECT_PLAYBACK_PAUSED;
    assert(direct_playback_state_buffer(&state, 100));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);
    assert(state.intent == DIRECT_PLAYBACK_PAUSED);

    assert(direct_playback_state_buffer(&state, 0));
    state.intent = DIRECT_PLAYBACK_PLAYING;
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);
    assert(direct_playback_state_buffer(&state, 100));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PLAYING);

    state.intent = DIRECT_PLAYBACK_STOPPED;
    assert(direct_playback_state_buffer(&state, 0));
    assert(direct_playback_state_buffer(&state, 100));
    state.preroll_complete = true;
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_STOPPED);
    assert(state.intent == DIRECT_PLAYBACK_STOPPED);

    /* A new /play request resets the previous stop, but must preroll again. */
    direct_playback_state_reset(&state);
    assert(state.intent == DIRECT_PLAYBACK_PLAYING);
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);
}

static void test_live_source_does_not_wait_for_preroll(void) {
    direct_playback_state_t state;
    direct_playback_state_reset(&state);
    state.live = true;
    assert(direct_playback_state_buffer(&state, 0));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PLAYING);

    state.intent = DIRECT_PLAYBACK_PAUSED;
    assert(direct_playback_state_buffer(&state, 100));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);

    state.intent = DIRECT_PLAYBACK_STOPPED;
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_STOPPED);

    direct_playback_state_reset(&state);
    assert(!state.live);
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);
}

static void test_invalid_buffer_reports_preserve_state(void) {
    direct_playback_state_t state;
    direct_playback_state_reset(&state);
    state.preroll_complete = true;
    assert(!direct_playback_state_buffer(&state, -1));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PLAYING);
    assert(direct_playback_state_buffer(&state, 20));
    assert(!direct_playback_state_buffer(&state, 101));
    assert(direct_playback_state_target(&state) == DIRECT_PLAYBACK_PAUSED);
}

static void test_seek_conversion(void) {
    int64_t ns = -1;
    assert(direct_playback_seconds_to_ns(0.0, &ns) && ns == 0);
    assert(direct_playback_seconds_to_ns(-0.0, &ns) && ns == 0);
    assert(direct_playback_seconds_to_ns(1.25, &ns) && ns == INT64_C(1250000000));
    assert(direct_playback_seconds_to_ns(0.0000000005, &ns) && ns == 0);
    assert(direct_playback_seconds_to_ns(2148.0, &ns) && ns == INT64_C(2148000000000));
    assert(direct_playback_seconds_to_ns(7200.5, &ns) && ns == INT64_C(7200500000000));
    assert(direct_playback_seconds_to_ns(86400.0, &ns) && ns == INT64_C(86400000000000));

    /* The greatest nearby representable seconds below the signed boundary
     * must still convert without invoking an out-of-range integer cast. */
    double limit_seconds = 0x1p63 / 1000000000.0;
    double below_limit_seconds = nextafter(limit_seconds, 0.0);
    assert(direct_playback_seconds_to_ns(below_limit_seconds, &ns));
    assert(ns > INT64_C(9000000000000000000));
    assert(ns < INT64_MAX);

    double rejected[] = {
        -1.0, -DBL_MIN, NAN, INFINITY, -INFINITY, DBL_MAX,
        limit_seconds, nextafter(limit_seconds, INFINITY)
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        ns = INT64_C(123456789);
        assert(!direct_playback_seconds_to_ns(rejected[i], &ns));
        assert(ns == INT64_C(123456789));
    }
    assert(!direct_playback_seconds_to_ns(1.0, NULL));
}

int main(void) {
    test_startup_and_rebuffering();
    test_user_intent_survives_buffer_events();
    test_live_source_does_not_wait_for_preroll();
    test_invalid_buffer_reports_preserve_state();
    test_seek_conversion();
    puts("Direct playback state and seek conversion tests passed.");
    return 0;
}
