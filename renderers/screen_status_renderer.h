/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SCREEN_STATUS_RENDERER_H
#define SCREEN_STATUS_RENDERER_H

#include <stdbool.h>
#include "../lib/logger.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct screen_status_renderer_s screen_status_renderer_t;

/* Construction does not create a pipeline or acquire any device. The caller
 * owns display arbitration: show only after all other video owners release it.
 * Sink options are a property list, never a pipeline extension. This renderer
 * has no audio elements. The logger must outlive this object. */
screen_status_renderer_t *screen_status_renderer_new(logger_t *logger,
                                                     const char *sink,
                                                     const char *sink_options);
bool screen_status_renderer_show(screen_status_renderer_t *renderer);
/* Updates an already shown surface. Never opens or reacquires a display.
 * Call at about 1 Hz, and immediately after meaningful state changes. */
bool screen_status_renderer_tick(screen_status_renderer_t *renderer);
/* Returns true only once the pipeline has released its output. All entry
 * points serialize renderer lifetime; callers must not race free with calls.
 * GStreamer/kernel calls themselves cannot be made safe against a wedged DRM
 * driver by an application timeout. Do not claim recovery from this return. */
bool screen_status_renderer_hide(screen_status_renderer_t *renderer);
bool screen_status_renderer_is_visible(screen_status_renderer_t *renderer);
/* Returns false without freeing when output release is unconfirmed; retain
 * the object for recovery. No pipeline may outlive its owning object. */
bool screen_status_renderer_free(screen_status_renderer_t *renderer);

#ifdef __cplusplus
}
#endif
#endif
