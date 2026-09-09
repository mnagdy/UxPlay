/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PLAYBACK_DIAGNOSTICS_H
#define PLAYBACK_DIAGNOSTICS_H

#include <gst/gst.h>
#include "../lib/logger.h"

G_BEGIN_DECLS

typedef struct playback_diagnostics_s playback_diagnostics_t;

/* Attach before starting the direct-playback pipeline. requested_at_us is from
 * g_get_monotonic_time(); pass 0 if the request time is unavailable. The logger
 * must outlive this object. No media URI or arbitrary caps are logged.
 */
playback_diagnostics_t *playback_diagnostics_attach(GstElement *playbin,
                                                   logger_t *logger,
                                                   gint64 requested_at_us);

/* Relay pipeline bus messages from the renderer's existing bus callback. */
void playback_diagnostics_message(playback_diagnostics_t *diagnostics,
                                  GstMessage *message);

/* Call after the pipeline has completed its transition to NULL and its bus
 * callback has stopped using diagnostics. This disconnects signals and removes
 * any probes that have not fired. It does not change pipeline state itself.
 */
void playback_diagnostics_free(playback_diagnostics_t *diagnostics);

G_END_DECLS
#endif
