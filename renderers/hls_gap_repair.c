/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hls_gap_repair.h"
#include <string.h>

/* hlsdemux2 appeared in 1.22. Keep older supported UxPlay builds compiling,
 * with this compatibility path inactive instead of requiring newer APIs. */
#if GST_CHECK_VERSION(1, 22, 0)

/* qtdemux's pull-mode scheduler emits catch-up gaps for sparse streams. Its
 * push-mode scheduler does not. hlsdemux2 then waits for timed data from every
 * selected track, even if the downloaded fragments contain only video.
 *
 * Do not infer missing audio from wall time or from video running ahead within
 * an MP4 fragment: muxers may put every video sample before the audio samples.
 * A top-level MP4 styp box identifies the beginning of the next media segment.
 * Only when that header precedes new sample data and all bytes before it were
 * delivered in an earlier buffer is the previous segment known complete.
 * The separately version-checked 1.26.2 completion adapter below also permits
 * repair immediately after a whole segment finishes, without waiting for the
 * next segment to become available.
 * Only a complete segment containing video and no audio is repaired.
 * Real audio remains selected and is never discarded or replaced here.
 */
#define MAX_DEMUXERS 8
#define MAX_TRACKS 8
#define MAX_REPORTS 16
#define MAX_GAP (60 * GST_SECOND)
#define FRAGMENT_OVERLAP (250 * GST_MSECOND)

typedef struct gap_demux_s gap_demux_t;
typedef struct {
    hls_gap_repair_t *owner;
    GstElement *adaptive;
    gulong notify;
} completion_watch_t;
typedef struct {
    gap_demux_t *demux;
    GstPad *pad;
    gulong probe;
    gboolean audio;
    gboolean video;
    gboolean active;
    gboolean have_segment;
    GstSegment segment;
    guint64 fragment_buffers;
    guint64 total_buffers;
    gboolean repaired;
    GstClockTime covered_until;
} gap_track_t;

struct gap_demux_s {
    hls_gap_repair_t *owner;
    GstElement *element;
    GstElement *adaptive;
    GstPad *sink;
    gulong sink_probe;
    gulong pad_added;
    gulong pad_removed;
    GMutex lock;
    GPtrArray *tracks;
    gboolean fragment_started;
    gboolean fragment_completed;
    GstClockTime fragment_video_start;
    GstClockTime fragment_video_end;
    guint64 fragment_number;
    gboolean disabled;
    guint input_reports;
    guint8 box_header[16];
    guint box_header_bytes;
    guint64 box_remaining;
    gboolean seen_media;
};

struct hls_gap_repair_s {
    GstElement *pipeline;
    logger_t *logger;
    guint64 session_id;
    gulong element_added;
    GMutex lock;
    GPtrArray *demuxers;
    GPtrArray *completion_watches;
    guint reports;
};

typedef struct {
    GstPad *pad;
    GstClockTime start;
    GstClockTime duration;
    guint64 fragment;
    gap_track_t *track;
} pending_gap_t;

static const gchar *factory_name(GstElement *element) {
    GstElementFactory *factory = gst_element_get_factory(element);
    return factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : "";
}

static GstElement *find_hls_parent(GstElement *element) {
    GstObject *parent = gst_object_get_parent(GST_OBJECT(element));
    while (parent) {
        if (GST_IS_ELEMENT(parent) && !strcmp(factory_name(GST_ELEMENT(parent)), "hlsdemux2")) {
            return GST_ELEMENT(parent); /* transfer full */
        }
        GstObject *next = gst_object_get_parent(parent);
        gst_object_unref(parent);
        parent = next;
    }
    return NULL;
}

/* Called under the demux lock, including on seek/flush/discontinuity. */
static void reset_fragment(gap_demux_t *d, gboolean reset_coverage) {
    d->fragment_completed = FALSE;
    d->fragment_video_start = GST_CLOCK_TIME_NONE;
    d->fragment_video_end = GST_CLOCK_TIME_NONE;
    for (guint i = 0; i < d->tracks->len; i++) {
        gap_track_t *t = g_ptr_array_index(d->tracks, i);
        t->fragment_buffers = 0;
        if (reset_coverage) {
            t->covered_until = GST_CLOCK_TIME_NONE;
            t->repaired = FALSE;
        }
    }
}

static GstClockTime to_running_time(gap_track_t *t, GstClockTime timestamp) {
    if (!t->have_segment || !GST_CLOCK_TIME_IS_VALID(timestamp) ||
        t->segment.format != GST_FORMAT_TIME || t->segment.rate != 1.0 ||
        t->segment.applied_rate != 1.0) return GST_CLOCK_TIME_NONE;
    return gst_segment_to_running_time(&t->segment, GST_FORMAT_TIME, timestamp);
}

static GstPadProbeReturn track_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void)pad;
    gap_track_t *t = user_data;
    gap_demux_t *d = t->demux;
    g_mutex_lock(&d->lock);
    if (!t->active) {
        g_mutex_unlock(&d->lock);
        return GST_PAD_PROBE_OK;
    }
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        d->disabled = TRUE;
    } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            gst_event_copy_segment(event, &t->segment);
            t->have_segment = TRUE;
            t->covered_until = GST_CLOCK_TIME_NONE;
        } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START ||
                   GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
            t->have_segment = FALSE;
            d->fragment_started = FALSE;
            reset_fragment(d, TRUE);
        } else if (GST_EVENT_TYPE(event) == GST_EVENT_GAP && t->audio) {
            GstClockTime start, duration;
            gst_event_parse_gap(event, &start, &duration);
            if (GST_CLOCK_TIME_IS_VALID(duration) && start <= GST_CLOCK_TIME_NONE - duration - 1) {
                GstClockTime end = to_running_time(t, start + duration);
                if (GST_CLOCK_TIME_IS_VALID(end) &&
                    (!GST_CLOCK_TIME_IS_VALID(t->covered_until) || end > t->covered_until))
                    t->covered_until = end;
            }
        }
    } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        t->fragment_buffers++;
        t->total_buffers++;
        /* Decode time avoids extending an audio gap into the following fragment
         * because of reordered video presentation timestamps. */
        GstClockTime timestamp = t->video && GST_BUFFER_DTS_IS_VALID(buffer) ?
                                 GST_BUFFER_DTS(buffer) : GST_BUFFER_PTS(buffer);
        GstClockTime start = to_running_time(t, timestamp);
        GstClockTime end = start;
        GstClockTime duration = GST_BUFFER_DURATION(buffer);
        if (GST_CLOCK_TIME_IS_VALID(start) && GST_CLOCK_TIME_IS_VALID(duration) &&
            start < GST_CLOCK_TIME_NONE - duration)
            end = start + duration;
        if (t->video && GST_CLOCK_TIME_IS_VALID(start)) {
            if (!GST_CLOCK_TIME_IS_VALID(d->fragment_video_start) || start < d->fragment_video_start)
                d->fragment_video_start = start;
            if (!GST_CLOCK_TIME_IS_VALID(d->fragment_video_end) || end > d->fragment_video_end)
                d->fragment_video_end = end;
        } else if (t->audio && GST_CLOCK_TIME_IS_VALID(end) &&
                   (!GST_CLOCK_TIME_IS_VALID(t->covered_until) || end > t->covered_until)) {
            t->covered_until = end;
        }
    }
    g_mutex_unlock(&d->lock);
    return GST_PAD_PROBE_OK;
}

static void add_pad(GstElement *element, GstPad *pad, gpointer user_data) {
    (void)element;
    gap_demux_t *d = user_data;
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
    gboolean audio = g_str_has_prefix(GST_PAD_NAME(pad), "audio_");
    gboolean video = g_str_has_prefix(GST_PAD_NAME(pad), "video_");
    if (!audio && !video) return;
    g_mutex_lock(&d->lock);
    for (guint i = 0; i < d->tracks->len; i++) {
        gap_track_t *existing = g_ptr_array_index(d->tracks, i);
        if (existing->pad == pad) {
            g_mutex_unlock(&d->lock);
            return;
        }
        if (video && existing->video && existing->active) d->disabled = TRUE;
        if (audio && existing->audio && existing->active) d->disabled = TRUE;
    }
    if (d->tracks->len >= MAX_TRACKS) {
        d->disabled = TRUE;
        g_mutex_unlock(&d->lock);
        return;
    }
    gap_track_t *t = g_new0(gap_track_t, 1);
    t->demux = d;
    t->pad = gst_object_ref(pad);
    t->audio = audio;
    t->video = video;
    t->active = TRUE;
    t->covered_until = GST_CLOCK_TIME_NONE;
    gst_segment_init(&t->segment, GST_FORMAT_UNDEFINED);
    g_ptr_array_add(d->tracks, t);
    g_mutex_unlock(&d->lock);
    t->probe = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
                                GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                                GST_PAD_PROBE_TYPE_EVENT_FLUSH, track_probe, t, NULL);
}

static void remove_pad(GstElement *element, GstPad *pad, gpointer user_data) {
    (void)element;
    gap_demux_t *d = user_data;
    g_mutex_lock(&d->lock);
    for (guint i = 0; i < d->tracks->len; i++) {
        gap_track_t *t = g_ptr_array_index(d->tracks, i);
        if (t->pad == pad) t->active = FALSE;
    }
    d->fragment_started = FALSE;
    reset_fragment(d, TRUE);
    g_mutex_unlock(&d->lock);
    /* Keep observer storage until NULL; another streaming callback may still
     * be returning from the removed pad. Inactive pads cannot generate gaps. */
}

static guint complete_fragment(gap_demux_t *d, pending_gap_t *pending) {
    guint count = 0;
    if (d->disabled || !d->fragment_started || d->fragment_completed ||
        !GST_CLOCK_TIME_IS_VALID(d->fragment_video_start) ||
        !GST_CLOCK_TIME_IS_VALID(d->fragment_video_end) ||
        d->fragment_video_end <= FRAGMENT_OVERLAP) return 0;
    GstClockTime end = d->fragment_video_end - FRAGMENT_OVERLAP;
    for (guint i = 0; i < d->tracks->len; i++) {
        gap_track_t *t = g_ptr_array_index(d->tracks, i);
        if (!t->active || !t->audio || t->fragment_buffers || !t->have_segment ||
            t->segment.format != GST_FORMAT_TIME || t->segment.rate != 1.0 ||
            t->segment.applied_rate != 1.0 || !gst_pad_is_linked(t->pad)) continue;
        GstClockTime start = GST_CLOCK_TIME_IS_VALID(t->covered_until) ?
                             t->covered_until : d->fragment_video_start;
        if (end <= start || end - start > MAX_GAP) continue;
        GstClockTime position = gst_segment_position_from_running_time(
            &t->segment, GST_FORMAT_TIME, start);
        GstClockTime stop = gst_segment_position_from_running_time(
            &t->segment, GST_FORMAT_TIME, end);
        if (!GST_CLOCK_TIME_IS_VALID(position) || !GST_CLOCK_TIME_IS_VALID(stop) || stop <= position)
            continue;
        pending[count++] = (pending_gap_t) {gst_object_ref(t->pad), position, stop - position,
                                          d->fragment_number, t};
        t->covered_until = end;
    }
    return count;
}

/* Walk only top-level ISO-BMFF headers; never retain media or allocate by a box
 * length. A styp beginning at byte zero, or in a preceding buffer's incomplete
 * header, is a safe boundary. The first styp may also follow initialization
 * boxes in the same buffer, because none of those bytes can produce samples.
 * Once a moof or mdat has appeared, a styp after bytes in this buffer is not
 * safe: those bytes have not reached qtdemux when this pre-chain probe runs.
 *
 * Partial headers and coalesced input are handled conservatively. A segment
 * whose boundary shares an input buffer with preceding media is not repaired.
 */
static guint inspect_boxes(gap_demux_t *d, GstBuffer *buffer, pending_gap_t *pending) {
    GstMapInfo map;
    guint count = 0;
    gsize position = 0;
    gboolean header_started_before_buffer = d->box_header_bytes != 0;
    gsize header_start = 0;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return 0;
    while (position < map.size && !d->disabled) {
        if (d->box_remaining) {
            guint64 amount = MIN(d->box_remaining, map.size - position);
            d->box_remaining -= amount;
            position += amount;
            continue;
        }
        if (!d->box_header_bytes) {
            header_start = position;
            header_started_before_buffer = FALSE;
        }
        guint needed = 8;
        if (d->box_header_bytes >= 8 && GST_READ_UINT32_BE(d->box_header) == 1) needed = 16;
        guint amount = MIN((gsize)(needed - d->box_header_bytes), map.size - position);
        memcpy(d->box_header + d->box_header_bytes, map.data + position, amount);
        d->box_header_bytes += amount;
        position += amount;
        if (d->box_header_bytes < needed) break;
        guint64 size = GST_READ_UINT32_BE(d->box_header);
        if (size == 1 && needed == 8) continue;
        if (size == 1) size = GST_READ_UINT64_BE(d->box_header + 8);
        if (size < needed) {
            d->disabled = TRUE; /* Unbounded/invalid box cannot prove a boundary. */
            break;
        }
        if (!memcmp(d->box_header + 4, "styp", 4)) {
            gboolean safe = !d->seen_media || header_started_before_buffer || header_start == 0;
            if (safe) count = complete_fragment(d, pending);
            reset_fragment(d, FALSE);
            d->fragment_started = safe;
            d->fragment_number++;
            if (d->input_reports < 8) {
                d->input_reports++;
                logger_log(d->owner->logger, LOGGER_DEBUG,
                           "Direct playback: stage=gap-fragment boundary=%" G_GUINT64_FORMAT
                           " safe=%d repairs=%u tracks=%u", d->fragment_number,
                           safe, count, d->tracks->len);
            }
        }
        if (!memcmp(d->box_header + 4, "moof", 4) ||
            !memcmp(d->box_header + 4, "mdat", 4)) d->seen_media = TRUE;
        d->box_remaining = size - needed;
        d->box_header_bytes = 0;
    }
    gst_buffer_unmap(buffer, &map);
    return count;
}

static void push_gaps(gap_demux_t *d, pending_gap_t *pending, guint count) {
    /* No helper lock across downstream calls: GAP returns through track_probe,
     * and adaptive demuxer output runs on another streaming thread. */
    for (guint i = 0; i < count; i++) {
        GstEvent *event = gst_event_new_gap(pending[i].start, pending[i].duration);
        gst_event_set_gap_flags(event, GST_GAP_FLAG_MISSING_DATA);
        gboolean accepted = gst_pad_push_event(pending[i].pad, event);
        if (accepted) {
            g_mutex_lock(&d->lock);
            pending[i].track->repaired = TRUE;
            g_mutex_unlock(&d->lock);
        } else {
            g_mutex_lock(&d->lock);
            pending[i].track->covered_until = GST_CLOCK_TIME_NONE;
            pending[i].track->repaired = FALSE;
            g_mutex_unlock(&d->lock);
        }
        hls_gap_repair_t *r = d->owner;
        g_mutex_lock(&r->lock);
        if (r->reports < MAX_REPORTS) {
            r->reports++;
            logger_log(r->logger, LOGGER_INFO, "Direct playback: session=%" G_GUINT64_FORMAT
                       " stage=gap-repair fragment=%" G_GUINT64_FORMAT
                       " start_ms=%" G_GUINT64_FORMAT " duration_ms=%" G_GUINT64_FORMAT
                       " accepted=%d", r->session_id, pending[i].fragment,
                       pending[i].start / GST_MSECOND, pending[i].duration / GST_MSECOND, accepted);
        }
        g_mutex_unlock(&r->lock);
        gst_object_unref(pending[i].pad);
    }
}

static GstPadProbeReturn input_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void)pad;
    gap_demux_t *d = user_data;
    pending_gap_t pending[MAX_TRACKS];
    guint count = 0;
    g_mutex_lock(&d->lock);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        /* A probe precedes the entire list, so we cannot use a boundary inside
         * it as proof that earlier list items have already been parsed. */
        d->disabled = TRUE;
    } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) {
            d->fragment_started = FALSE;
            reset_fragment(d, TRUE);
            d->box_header_bytes = 0;
            d->box_remaining = 0;
            d->seen_media = FALSE;
        }
        count = inspect_boxes(d, buffer, pending);
    } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
            if (!d->box_header_bytes && !d->box_remaining)
                count = complete_fragment(d, pending);
            d->fragment_started = FALSE;
        } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START ||
                   GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP ||
                   GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            d->fragment_started = FALSE;
            reset_fragment(d, TRUE);
            d->box_header_bytes = 0;
            d->box_remaining = 0;
            d->seen_media = FALSE;
        }
    }
    g_mutex_unlock(&d->lock);
    push_gaps(d, pending, count);
    return GST_PAD_PROBE_OK;
}

static gboolean completion_version_supported(guint major, guint minor, guint micro, guint nano,
                                             const gchar *plugin_name, const gchar *plugin_version) {
    return major == 1 && minor == 26 && micro == 2 && nano == 0 &&
           !g_strcmp0(plugin_name, "adaptivedemux2") && !g_strcmp0(plugin_version, "1.26.2");
}

static GstElement *single_hls_parser(GstElement *adaptive) {
    GstElement *parser = NULL;
    GstIterator *iterator = gst_bin_iterate_elements(GST_BIN(adaptive));
    GValue item = G_VALUE_INIT;
    guint count = 0;
    GstIteratorResult result;
    while ((result = gst_iterator_next(iterator, &item)) == GST_ITERATOR_OK) {
        GstElement *element = g_value_get_object(&item);
        if (!strcmp(factory_name(element), "parsebin")) {
            count++;
            if (!parser) parser = gst_object_ref(element);
        }
        g_value_reset(&item);
    }
    if (G_VALUE_TYPE(&item)) g_value_unset(&item);
    gst_iterator_free(iterator);
    /* A concurrent topology change is ambiguous too; wait for a later event. */
    if (count != 1 || result != GST_ITERATOR_DONE) gst_clear_object(&parser);
    return parser;
}

/* This is an explicitly versioned compatibility adapter, not a general claim
 * that a bandwidth notification means a download completed. In GStreamer
 * 1.26.2 there is exactly one notification site, for streams containing video:
 * gstadaptivedemux-stream.c:2675-2711, update_current_bitrate(). Its only caller
 * is advance_fragment():2403-2484, reached after HLS finish_fragment():875-960
 * has drained pending decrypted/typefind/segment data. Header/index downloads
 * return before that call. The notification runs after the object and segment
 * locks are released, without the adaptive track lock held.
 * Source: https://github.com/GStreamer/gstreamer/tree/1.26.2/subprojects/gst-plugins-good/ext/adaptivedemux2
 * Other versions retain only the conservative next-styp/EOS fallback.
 */
static void fragment_download_completed(GObject *object, GParamSpec *property, gpointer data) {
    (void)property;
    completion_watch_t *watch = data;
    hls_gap_repair_t *r = watch->owner;
    /* A demux-level notification has no source stream ID. Require exactly one
     * parser beneath this HLS demuxer, so a different rendition/period or a
     * non-MP4 video stream cannot finalize the wrong MP4 parser. */
    GstElement *parser = single_hls_parser(GST_ELEMENT(object));
    if (!parser) return;
    gap_demux_t *candidate = NULL;
    guint video_contexts = 0;
    pending_gap_t pending[MAX_TRACKS];
    guint count = 0;
    gap_demux_t *contexts[MAX_DEMUXERS];
    g_mutex_lock(&r->lock);
    guint context_count = MIN(r->demuxers->len, MAX_DEMUXERS);
    for (guint i = 0; i < context_count; i++)
        contexts[i] = g_ptr_array_index(r->demuxers, i);
    g_mutex_unlock(&r->lock);
    /* Context storage outlives all streaming callbacks (freed after NULL).
     * Keep GStreamer topology queries outside the owner lock. */
    for (guint i = 0; i < context_count; i++) {
        gap_demux_t *d = contexts[i];
        if (d->adaptive != GST_ELEMENT(object) ||
            !gst_object_has_as_ancestor(GST_OBJECT(d->element), GST_OBJECT(parser))) continue;
        g_mutex_lock(&d->lock);
        gboolean active_video = FALSE;
        for (guint j = 0; j < d->tracks->len; j++) {
            gap_track_t *t = g_ptr_array_index(d->tracks, j);
            if (t->active && t->video && gst_pad_is_linked(t->pad))
                active_video = TRUE;
        }
        if (active_video) {
            video_contexts++;
            candidate = d;
        }
        g_mutex_unlock(&d->lock);
    }
    /* The notification is per adaptive demuxer, not per stream. If overlapping
     * periods/variants make its originating video parser ambiguous, do nothing. */
    if (video_contexts == 1) {
        g_mutex_lock(&candidate->lock);
        if (!candidate->disabled && candidate->fragment_started && !candidate->fragment_completed &&
            !candidate->box_header_bytes && !candidate->box_remaining &&
            GST_CLOCK_TIME_IS_VALID(candidate->fragment_video_start) &&
            GST_CLOCK_TIME_IS_VALID(candidate->fragment_video_end)) {
            count = complete_fragment(candidate, pending);
            candidate->fragment_completed = TRUE;
        }
        g_mutex_unlock(&candidate->lock);
    }
    gst_object_unref(parser);
    if (candidate && video_contexts == 1) push_gaps(candidate, pending, count);
}

/* Owner lock held; all callbacks/storage are retired after pipeline NULL. */
static void watch_completion(hls_gap_repair_t *r, GstElement *adaptive) {
    for (guint i = 0; i < r->completion_watches->len; i++) {
        completion_watch_t *watch = g_ptr_array_index(r->completion_watches, i);
        if (watch->adaptive == adaptive) return;
    }
    if (r->completion_watches->len >= MAX_DEMUXERS) return;
    guint major, minor, micro, nano;
    gst_version(&major, &minor, &micro, &nano);
    GstElementFactory *factory = gst_element_get_factory(adaptive);
    GstPlugin *plugin = factory ? gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory)) : NULL;
    gboolean supported = plugin && completion_version_supported(major, minor, micro, nano,
        gst_plugin_get_name(plugin), gst_plugin_get_version(plugin));
    if (plugin) gst_object_unref(plugin);
    if (!supported) return;
    completion_watch_t *watch = g_new0(completion_watch_t, 1);
    watch->owner = r;
    watch->adaptive = gst_object_ref(adaptive);
    g_ptr_array_add(r->completion_watches, watch);
    watch->notify = g_signal_connect(adaptive, "notify::current-bandwidth",
                                    G_CALLBACK(fragment_download_completed), watch);
    logger_log(r->logger, LOGGER_INFO, "Direct playback: session=%" G_GUINT64_FORMAT
               " stage=gap-completion-hook gst=1.26.2", r->session_id);
}

static void element_added(GstBin *pipeline, GstBin *parent, GstElement *element, gpointer user_data) {
    (void)pipeline;
    (void)parent;
    hls_gap_repair_t *r = user_data;
    if (strcmp(factory_name(element), "qtdemux")) return;
    GstElement *adaptive = find_hls_parent(element);
    gboolean is_hls = adaptive != NULL;
    logger_log(r->logger, LOGGER_DEBUG, "Direct playback: stage=gap-demux hls=%d", is_hls);
    if (!is_hls) return;
    g_mutex_lock(&r->lock);
    if (r->demuxers->len >= MAX_DEMUXERS) {
        g_mutex_unlock(&r->lock);
        gst_object_unref(adaptive);
        return;
    }
    gap_demux_t *d = g_new0(gap_demux_t, 1);
    d->owner = r;
    d->element = gst_object_ref(element);
    d->adaptive = adaptive;
    d->tracks = g_ptr_array_new();
    g_mutex_init(&d->lock);
    reset_fragment(d, TRUE);
    g_ptr_array_add(r->demuxers, d);
    watch_completion(r, adaptive);
    g_mutex_unlock(&r->lock);
    d->pad_added = g_signal_connect(element, "pad-added", G_CALLBACK(add_pad), d);
    d->pad_removed = g_signal_connect(element, "pad-removed", G_CALLBACK(remove_pad), d);
    d->sink = gst_element_get_static_pad(element, "sink");
    d->sink_probe = gst_pad_add_probe(d->sink, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH, input_probe, d, NULL);
}

hls_gap_repair_t *hls_gap_repair_attach(GstElement *pipeline, logger_t *logger, guint64 session_id) {
    if (!GST_IS_BIN(pipeline) || !logger) return NULL;
    hls_gap_repair_t *r = g_new0(hls_gap_repair_t, 1);
    r->pipeline = gst_object_ref(pipeline);
    r->logger = logger;
    r->session_id = session_id;
    r->demuxers = g_ptr_array_new();
    r->completion_watches = g_ptr_array_new();
    g_mutex_init(&r->lock);
    r->element_added = g_signal_connect(pipeline, "deep-element-added", G_CALLBACK(element_added), r);
    return r;
}

gboolean hls_gap_repair_gap_only_audio(hls_gap_repair_t *r) {
    if (!r) return FALSE;
    gboolean gap_only = FALSE;
    g_mutex_lock(&r->lock);
    for (guint i = 0; i < r->demuxers->len && !gap_only; i++) {
        gap_demux_t *d = g_ptr_array_index(r->demuxers, i);
        g_mutex_lock(&d->lock);
        if (!d->disabled) {
            for (guint j = 0; j < d->tracks->len; j++) {
                gap_track_t *t = g_ptr_array_index(d->tracks, j);
                if (t->active && t->audio && t->repaired && !t->total_buffers &&
                    gst_pad_is_linked(t->pad)) gap_only = TRUE;
            }
        }
        g_mutex_unlock(&d->lock);
    }
    g_mutex_unlock(&r->lock);
    return gap_only;
}

void hls_gap_repair_free(hls_gap_repair_t *r) {
    if (!r) return;
    g_signal_handler_disconnect(r->pipeline, r->element_added);
    for (guint i = 0; i < r->completion_watches->len; i++) {
        completion_watch_t *watch = g_ptr_array_index(r->completion_watches, i);
        g_signal_handler_disconnect(watch->adaptive, watch->notify);
        gst_object_unref(watch->adaptive);
        g_free(watch);
    }
    g_ptr_array_free(r->completion_watches, TRUE);
    for (guint i = 0; i < r->demuxers->len; i++) {
        gap_demux_t *d = g_ptr_array_index(r->demuxers, i);
        g_signal_handler_disconnect(d->element, d->pad_added);
        g_signal_handler_disconnect(d->element, d->pad_removed);
        gst_pad_remove_probe(d->sink, d->sink_probe);
        gst_object_unref(d->sink);
        for (guint j = 0; j < d->tracks->len; j++) {
            gap_track_t *t = g_ptr_array_index(d->tracks, j);
            gst_pad_remove_probe(t->pad, t->probe);
            gst_object_unref(t->pad);
            g_free(t);
        }
        g_ptr_array_free(d->tracks, TRUE);
        gst_object_unref(d->element);
        gst_object_unref(d->adaptive);
        g_mutex_clear(&d->lock);
        g_free(d);
    }
    g_ptr_array_free(r->demuxers, TRUE);
    gst_object_unref(r->pipeline);
    g_mutex_clear(&r->lock);
    g_free(r);
}

#else
hls_gap_repair_t *hls_gap_repair_attach(GstElement *pipeline, logger_t *logger, guint64 session_id) {
    (void)pipeline;
    (void)logger;
    (void)session_id;
    return NULL;
}
void hls_gap_repair_free(hls_gap_repair_t *repair) {
    (void)repair;
}
gboolean hls_gap_repair_gap_only_audio(hls_gap_repair_t *repair) {
    (void)repair;
    return FALSE;
}
#endif
