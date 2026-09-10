/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Parser safety tests complement real HTTP playback cases. Include the helper
 * implementation so the private incremental parser has no production test API. */
#include "../renderers/hls_gap_repair.c"

#if GST_CHECK_VERSION(1, 22, 0)
static guint feed(gap_demux_t *demux, const guint8 *bytes, gsize length) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, length, NULL);
    gst_buffer_fill(buffer, 0, bytes, length);
    pending_gap_t gaps[MAX_TRACKS];
    guint count = inspect_boxes(demux, buffer, gaps);
    for (guint i = 0; i < count; i++) gst_object_unref(gaps[i].pad);
    gst_buffer_unref(buffer);
    return count;
}

static void reset_parser(gap_demux_t *demux) {
    demux->box_header_bytes = 0;
    demux->box_remaining = 0;
    demux->fragment_started = FALSE;
    demux->fragment_number = 0;
    demux->disabled = FALSE;
    demux->seen_media = FALSE;
    reset_fragment(demux, TRUE);
}

static void test_boundaries(logger_t *logger) {
    hls_gap_repair_t owner = {.logger = logger};
    gap_demux_t demux = {.owner = &owner, .tracks = g_ptr_array_new()};
    const guint8 segment[] = {
        0,0,0,16,'s','t','y','p',0,0,0,0,0,0,0,0,
        0,0,0,16,'m','d','a','t',1,2,3,4,5,6,7,8
    };
    /* hlsdemux2 commonly appends initialization to the first media body. It
     * must not cost an extra live segment of startup: metadata before the first
     * styp cannot contain samples from a preceding media segment. */
    reset_parser(&demux);
    guint8 initialized[24 + sizeof(segment)] = {
        0,0,0,16,'f','t','y','p',0,0,0,0,0,0,0,0,
        0,0,0,8,'m','o','o','v'
    };
    memcpy(initialized + 24, segment, sizeof(segment));
    feed(&demux, initialized, sizeof(initialized));
    g_assert_true(demux.fragment_started);
    g_assert_true(demux.seen_media);
    g_assert_cmpuint(demux.fragment_number, ==, 1);
    /* Every possible split of the short or extended header remains bounded,
     * and a split header does not make a complete old media buffer unsafe. */
    for (guint split = 1; split < 16; split++) {
        reset_parser(&demux);
        feed(&demux, segment, split);
        feed(&demux, segment + split, sizeof(segment) - split);
        g_assert_false(demux.disabled);
        g_assert_true(demux.fragment_started);
        g_assert_cmpuint(demux.fragment_number, ==, 1);
        g_assert_cmpuint(demux.box_remaining, ==, 0);
        g_assert_cmpuint(demux.box_header_bytes, ==, 0);
    }
    const guint8 extended[] = {
        0,0,0,1,'s','t','y','p',0,0,0,0,0,0,0,24,0,0,0,0,0,0,0,0
    };
    for (guint split = 1; split < 24; split++) {
        reset_parser(&demux);
        feed(&demux, extended, split);
        feed(&demux, extended + split, sizeof(extended) - split);
        g_assert_false(demux.disabled);
        g_assert_true(demux.fragment_started);
        g_assert_cmpuint(demux.fragment_number, ==, 1);
        g_assert_cmpuint(demux.box_remaining, ==, 0);
    }
    /* The second styp shares a pre-chain buffer with the last bytes of mdat.
     * It MUST invalidate that segment, even though the boxes are all valid. */
    reset_parser(&demux);
    feed(&demux, segment, 28);
    guint8 coalesced[36];
    memcpy(coalesced, segment + 28, 4);
    memcpy(coalesced + 4, segment, sizeof(segment));
    feed(&demux, coalesced, sizeof(coalesced));
    g_assert_false(demux.disabled);
    g_assert_false(demux.fragment_started);
    g_assert_cmpuint(demux.fragment_number, ==, 2);
    /* A subsequent clean boundary restores evidence collection. */
    feed(&demux, segment, sizeof(segment));
    g_assert_true(demux.fragment_started);
    g_assert_cmpuint(demux.fragment_number, ==, 3);

    /* Two complete segments coalesced in one input buffer are also unsafe. */
    reset_parser(&demux);
    guint8 joined[sizeof(segment) * 2];
    memcpy(joined, segment, sizeof(segment));
    memcpy(joined + sizeof(segment), segment, sizeof(segment));
    feed(&demux, joined, sizeof(joined));
    g_assert_false(demux.fragment_started);

    /* Zero-sized boxes extend to EOF and cannot prove a later boundary;
     * undersized boxes and undersized extended boxes are invalid. */
    const guint8 invalid[][16] = {
        {0,0,0,0,'m','d','a','t'},
        {0,0,0,7,'f','r','e','e'},
        {0,0,0,1,'m','d','a','t',0,0,0,0,0,0,0,15}
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        reset_parser(&demux);
        feed(&demux, invalid[i], sizeof(invalid[i]));
        g_assert_true(demux.disabled);
    }
    /* Large media bodies are skipped by count without allocating that size. */
    reset_parser(&demux);
    const guint8 large[] = {0xff,0xff,0xff,0xff,'m','d','a','t'};
    feed(&demux, large, sizeof(large));
    g_assert_false(demux.disabled);
    g_assert_cmpuint(demux.box_remaining, ==, G_MAXUINT32 - 8u);
    g_ptr_array_free(demux.tracks, TRUE);
}

static void test_gap_only_lifetime(void) {
    hls_gap_repair_t owner = {.demuxers = g_ptr_array_new()};
    gap_demux_t demux = {.owner = &owner, .tracks = g_ptr_array_new()};
    gap_track_t track = {.demux = &demux, .audio = TRUE, .active = TRUE};
    g_mutex_init(&owner.lock);
    g_mutex_init(&demux.lock);
    track.pad = gst_pad_new("audio_0", GST_PAD_SRC);
    GstPad *sink = gst_pad_new("sink", GST_PAD_SINK);
    g_assert_cmpint(gst_pad_link(track.pad, sink), ==, GST_PAD_LINK_OK);
    g_ptr_array_add(owner.demuxers, &demux);
    g_ptr_array_add(demux.tracks, &track);
    g_assert_false(hls_gap_repair_gap_only_audio(&owner));
    track.repaired = TRUE;
    g_assert_true(hls_gap_repair_gap_only_audio(&owner));
    track.total_buffers = 1; /* The first real sample restores normal buffering. */
    g_assert_false(hls_gap_repair_gap_only_audio(&owner));
    track.total_buffers = 0;
    reset_fragment(&demux, TRUE); /* Seek, flush, or discontinuity. */
    g_assert_false(hls_gap_repair_gap_only_audio(&owner));
    track.repaired = TRUE;
    track.active = FALSE; /* Removed pads cannot control a replacement track. */
    g_assert_false(hls_gap_repair_gap_only_audio(&owner));
    track.active = TRUE;
    demux.disabled = TRUE;
    g_assert_false(hls_gap_repair_gap_only_audio(&owner));
    demux.disabled = FALSE;
    gst_pad_unlink(track.pad, sink);
    g_assert_false(hls_gap_repair_gap_only_audio(&owner));
    gst_object_unref(sink);
    gst_object_unref(track.pad);
    g_ptr_array_free(demux.tracks, TRUE);
    g_ptr_array_free(owner.demuxers, TRUE);
    g_mutex_clear(&demux.lock);
    g_mutex_clear(&owner.lock);
}

static void test_completion_version_guard(void) {
    g_assert_true(completion_version_supported(1, 26, 2, 0, "adaptivedemux2", "1.26.2"));
    g_assert_false(completion_version_supported(1, 26, 3, 0, "adaptivedemux2", "1.26.2"));
    g_assert_false(completion_version_supported(1, 26, 2, 1, "adaptivedemux2", "1.26.2"));
    g_assert_false(completion_version_supported(1, 26, 2, 0, "adaptivedemux2", "1.26.3"));
    g_assert_false(completion_version_supported(1, 26, 2, 0, "other", "1.26.2"));
    g_assert_false(completion_version_supported(1, 26, 2, 0, NULL, NULL));
}

static void test_completion_correlation(void) {
    /* No pipeline is started: these NULL-state bins/pads exercise routing and
     * ambiguity, without network traffic or audio/video decoding. */
    GstElement *adaptive = gst_bin_new("test-adaptive");
    GstElement *parser = gst_element_factory_make("parsebin", NULL);
    g_assert_nonnull(parser);
    gst_bin_add(GST_BIN(adaptive), parser);
    hls_gap_repair_t owner = {.demuxers = g_ptr_array_new()};
    completion_watch_t watch = {.owner = &owner, .adaptive = adaptive};
    gap_demux_t demuxes[2] = {0};
    gap_track_t tracks[2] = {0};
    g_mutex_init(&owner.lock);
    for (guint i = 0; i < 2; i++) {
        gap_demux_t *d = &demuxes[i];
        GstElement *source = gst_element_factory_make("identity", NULL);
        GstElement *sink = gst_element_factory_make("fakesink", NULL);
        gst_bin_add_many(GST_BIN(parser), source, sink, NULL);
        g_assert_true(gst_element_link(source, sink));
        d->owner = &owner;
        d->element = source;
        d->adaptive = adaptive;
        d->tracks = g_ptr_array_new();
        d->fragment_started = TRUE;
        d->fragment_video_start = 0;
        d->fragment_video_end = 2 * GST_SECOND;
        g_mutex_init(&d->lock);
        tracks[i].pad = gst_element_get_static_pad(source, "src");
        tracks[i].active = TRUE;
        tracks[i].video = TRUE;
        g_ptr_array_add(d->tracks, &tracks[i]);
        g_ptr_array_add(owner.demuxers, d);
    }
    /* A notification from already completed B must not finalize A, which can
     * be between video-only and later audio moofs in its unfinished body. */
    demuxes[1].fragment_completed = TRUE;
    fragment_download_completed(G_OBJECT(adaptive), NULL, &watch);
    g_assert_false(demuxes[0].fragment_completed);
    tracks[1].active = FALSE;
    fragment_download_completed(G_OBJECT(adaptive), NULL, &watch);
    g_assert_true(demuxes[0].fragment_completed);
    /* Truncated/in-progress MP4 boxes cannot be finalized even for one source. */
    demuxes[0].fragment_completed = FALSE;
    demuxes[0].box_remaining = 5;
    fragment_download_completed(G_OBJECT(adaptive), NULL, &watch);
    g_assert_false(demuxes[0].fragment_completed);
    demuxes[0].box_remaining = 0;
    /* Any second HLS parser is ambiguous, including a non-MP4 video parser. */
    GstElement *other_parser = gst_element_factory_make("parsebin", NULL);
    gst_bin_add(GST_BIN(adaptive), other_parser);
    fragment_download_completed(G_OBJECT(adaptive), NULL, &watch);
    g_assert_false(demuxes[0].fragment_completed);
    gst_bin_remove(GST_BIN(adaptive), other_parser);
    /* A detached old parser cannot be matched to the active HLS demuxer. */
    gst_object_ref(parser);
    gst_bin_remove(GST_BIN(adaptive), parser);
    fragment_download_completed(G_OBJECT(adaptive), NULL, &watch);
    g_assert_false(demuxes[0].fragment_completed);
    for (guint i = 0; i < 2; i++) {
        gst_object_unref(tracks[i].pad);
        g_ptr_array_free(demuxes[i].tracks, TRUE);
        g_mutex_clear(&demuxes[i].lock);
    }
    g_ptr_array_free(owner.demuxers, TRUE);
    g_mutex_clear(&owner.lock);
    gst_object_unref(parser);
    gst_object_unref(adaptive);
}
#endif

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
#if GST_CHECK_VERSION(1, 22, 0)
    logger_t *logger = logger_init();
    logger_set_level(logger, LOGGER_ERR);
    test_boundaries(logger);
    test_gap_only_lifetime();
    test_completion_version_guard();
    test_completion_correlation();
    logger_destroy(logger);
#endif
    gst_deinit();
    return 0;
}
