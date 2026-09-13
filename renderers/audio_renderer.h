/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifndef AUDIO_RENDERER_H
#define AUDIO_RENDERER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "../lib/logger.h"
#include "screen_status.h"

bool gstreamer_init();
void audio_renderer_init(logger_t *logger, const char* audiosink, const bool *audio_sync, const bool *video_sync, const char *artp_pipeline);
void audio_renderer_start(unsigned char* compression_type);
void audio_renderer_stop();
void audio_renderer_render_buffer(unsigned char* data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time);
void audio_renderer_set_volume(double volume);
void audio_renderer_flush();
void audio_renderer_destroy();
unsigned int audio_renderer_listen(void *loop, int id);
/* Read-only evidence from the active RAOP pipeline. Counts exclude previous
 * connections; a true return means an active renderer, not audible playback.
 * Output samples must be nonzero to claim observed audio progress. */
bool audio_renderer_get_screen_snapshot(screen_status_audio_t *snapshot);
#ifdef __cplusplus
}
#endif

#endif //AUDIO_RENDERER_H
