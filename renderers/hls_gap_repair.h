/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HLS_GAP_REPAIR_H
#define HLS_GAP_REPAIR_H

#include <gst/gst.h>
#include "../lib/logger.h"

G_BEGIN_DECLS
typedef struct hls_gap_repair_s hls_gap_repair_t;

/* Observe only push-mode MP4 demuxers owned by hlsdemux2. Attach before
 * starting playbin; free after its transition to NULL has completed. This
 * preserves audio selection and emits missing-data GAP events, never samples.
 */
hls_gap_repair_t *hls_gap_repair_attach(GstElement *pipeline, logger_t *logger,
                                      guint64 session_id);
void hls_gap_repair_free(hls_gap_repair_t *repair);

/* TRUE only for a repaired, single-audio-track stream that has never supplied
 * a real audio buffer. Its GAP events cannot backpressure through an audio
 * decoder that has not yet been created. The renderer may ignore the adaptive
 * buffering gate after video preroll, while preserving play/pause/stop intent.
 * This becomes FALSE as soon as any real audio sample arrives. */
gboolean hls_gap_repair_gap_only_audio(hls_gap_repair_t *repair);
G_END_DECLS
#endif
