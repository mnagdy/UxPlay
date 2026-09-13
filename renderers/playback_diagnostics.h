/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PLAYBACK_DIAGNOSTICS_H
#define PLAYBACK_DIAGNOSTICS_H

#include <gst/gst.h>
#include "../lib/logger.h"
#include "direct_playback_state.h"
#include "screen_status.h"

G_BEGIN_DECLS

typedef struct playback_diagnostics_s playback_diagnostics_t;

/* Fixed, allowlisted observations. No caps strings, URI, error text or payload
 * can enter this snapshot. Dimensions describe negotiated buffers, not HDMI.
 * Continuous counts are opt-in so screen-info=off retains the old probes. */
typedef struct {
    screen_status_video_t video;
    screen_status_audio_t audio;
    gint64 last_media_at_us, last_output_at_us;
    gboolean failed;
} playback_diagnostics_snapshot_t;

typedef void (*playback_diagnostics_video_presenter_t)(GstPad *pad,
                                                       GstPadProbeInfo *info,
                                                       gpointer data);

/* Configure before starting the pipeline. The presenter is called only at
 * actual video sink buffers and must not change caps or wait for the main loop.
 * Its data must remain alive until the pipeline is NULL and diagnostics freed. */
void playback_diagnostics_enable_screen(playback_diagnostics_t *diagnostics,
    playback_diagnostics_video_presenter_t presenter, gpointer data);
void playback_diagnostics_get_snapshot(playback_diagnostics_t *diagnostics,
                                       playback_diagnostics_snapshot_t *snapshot);

/* Attach before starting the direct-playback pipeline. requested_at_us is from
 * g_get_monotonic_time(); pass 0 if the request time is unavailable. The logger
 * must outlive this object. No media URI or arbitrary caps are logged.
 */
playback_diagnostics_t *playback_diagnostics_attach(GstElement *playbin,
                                                   logger_t *logger,
                                                   gint64 requested_at_us,
                                                   guint64 session_id);

/* Relay pipeline bus messages from the renderer's existing bus callback. */
void playback_diagnostics_message(playback_diagnostics_t *diagnostics,
                                  GstMessage *message);

/* Called by the renderer's existing lifetime-serialized timer, even when no
 * bus messages arrive. Neither method changes playback state. */
void playback_diagnostics_tick(playback_diagnostics_t *diagnostics,
                               const direct_playback_state_t *state);
void playback_diagnostics_control(playback_diagnostics_t *diagnostics,
                                  const direct_playback_state_t *state);

/* Call after the pipeline has completed its transition to NULL and its bus
 * callback has stopped using diagnostics. This disconnects signals and removes
 * any probes that have not fired. It does not change pipeline state itself.
 */
void playback_diagnostics_free(playback_diagnostics_t *diagnostics);

G_END_DECLS
#endif
