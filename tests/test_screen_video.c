/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the production sink presenter and lifetime hooks without a display.
 * Actual post-overlay buffer pixels are inspected, not just the text property. */
#include "../renderers/video_renderer.c"
#include <gst/base/gstbasesink.h>

typedef struct { GstBaseSink parent; } ScreenVideoSink;
typedef struct { GstBaseSinkClass parent; } ScreenVideoSinkClass;
G_DEFINE_TYPE(ScreenVideoSink, screen_video_sink, GST_TYPE_BASE_SINK)

static guint rendered, text_frames;
static logger_t *test_logger;
static guint opaque_maps;
static gboolean capture_frame;
static GstBuffer *captured_frame;

static GstStaticPadTemplate screen_sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw,format=BGRA"));

static GstFlowReturn screen_sink_render(GstBaseSink *sink, GstBuffer *buffer) {
    (void) sink;
    GstMapInfo map;
    g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_READ));
    guint bright = 0;
    for (gsize i = 0; i + 3 < map.size; i += 4)
        if (map.data[i] > 220 && map.data[i + 1] > 220 && map.data[i + 2] > 220) bright++;
    if (bright > 20) text_frames++;
    rendered++;
    if (capture_frame) {
        if (captured_frame) gst_buffer_unref(captured_frame);
        captured_frame = gst_buffer_ref(buffer);
    }
    gst_buffer_unmap(buffer, &map);
    return GST_FLOW_OK;
}

static void screen_video_sink_class_init(ScreenVideoSinkClass *klass) {
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "Headless screen sink", "Sink/Video",
        "Checks actual post-overlay pixels", "UxPlay tests");
    gst_element_class_add_static_pad_template(element, &screen_sink_template);
    GST_BASE_SINK_CLASS(klass)->render = screen_sink_render;
}

static void screen_video_sink_init(ScreenVideoSink *sink) {
    gst_base_sink_set_sync(GST_BASE_SINK(sink), FALSE);
}

/* A deliberately unmappable opaque allocation proves the presenter rejects
 * hardware-style memory before attempting import, map or software copy. */
typedef struct { GstAllocator parent; } ScreenOpaqueAllocator;
typedef struct { GstAllocatorClass parent; } ScreenOpaqueAllocatorClass;
G_DEFINE_TYPE(ScreenOpaqueAllocator, screen_opaque_allocator, GST_TYPE_ALLOCATOR)

static gpointer opaque_map(GstMemory *memory, gsize size, GstMapFlags flags) {
    (void) memory; (void) size; (void) flags;
    opaque_maps++;
    return NULL;
}
static void opaque_unmap(GstMemory *memory) { (void) memory; }
static void opaque_free(GstAllocator *allocator, GstMemory *memory) {
    (void) allocator;
    g_free(memory);
}
static void screen_opaque_allocator_class_init(ScreenOpaqueAllocatorClass *klass) {
    GST_ALLOCATOR_CLASS(klass)->free = opaque_free;
}
static void screen_opaque_allocator_init(ScreenOpaqueAllocator *allocator) {
    GST_ALLOCATOR(allocator)->mem_type = "UnqualifiedHardware";
    GST_ALLOCATOR(allocator)->mem_map = opaque_map;
    GST_ALLOCATOR(allocator)->mem_unmap = opaque_unmap;
}

static void capture_log(void *cls, int level, const char *text) {
    (void) cls; (void) level;
    g_assert_null(strstr(text, "PRIVATE_SCREEN_FIXTURE"));
}

static void clear_owner(video_renderer_t *owner) {
    if (owner->overlay_pipeline) {
        gst_element_set_state(owner->overlay_pipeline, GST_STATE_NULL);
        gst_object_unref(owner->overlay_pipeline);
    }
    if (owner->overlay) gst_video_overlay_composition_unref(owner->overlay);
    g_mutex_clear(&owner->overlay_lock);
}

static void test_pixels_and_opaque_guard(void) {
    video_renderer_t owner = {0};
    g_mutex_init(&owner.overlay_lock);
    owner.overlay = screen_overlay_render(&owner, "Paused\nH.264 1920 x 1080\nGStreamer: actual decoder", 1280, 720);
    g_assert_nonnull(owner.overlay);
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_BGRA, 1280, 720);
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, info.size, NULL);
    gst_buffer_memset(buffer, 0, 0, info.size);
    g_assert_true(screen_overlay_blend(&owner, buffer, &info));
    g_assert_true(owner.overlay_known);
    g_assert_true(owner.overlay_supported);
    GstMapInfo map;
    g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_READ));
    guint bright = 0;
    for (gsize i = 0; i + 3 < map.size; i += 4)
        if (map.data[i] > 220 && map.data[i + 1] > 220 && map.data[i + 2] > 220) bright++;
    g_assert_cmpuint(bright, >, 200);
    /* Text exists inside its safe panel; the center/bottom is untouched. */
    g_assert_cmpuint(map.data[(600 * 1280 + 640) * 4], ==, 0);
    gst_buffer_unmap(buffer, &map);
    gst_buffer_unref(buffer);

    GstAllocator *allocator = g_object_new(screen_opaque_allocator_get_type(), NULL);
    GstMemory *memory = g_new0(GstMemory, 1);
    gst_memory_init(memory, 0, allocator, NULL, info.size, 0, 0, info.size);
    buffer = gst_buffer_new();
    gst_buffer_append_memory(buffer, memory);
    g_assert_false(screen_overlay_blend(&owner, buffer, &info));
    g_assert_true(owner.overlay_known);
    g_assert_false(owner.overlay_supported);
    g_assert_cmpuint(opaque_maps, ==, 0);
    g_assert_cmpuint(gst_buffer_n_memory(buffer), ==, 1);
    gst_buffer_unref(buffer);
    gst_object_unref(allocator);
    video_renderer_configure_screen(SCREEN_INFO_STATUS);
    gst_video_overlay_composition_unref(owner.overlay);
    owner.overlay = screen_overlay_render(&owner, "Paused", 1280, 720);
    g_assert_nonnull(owner.overlay);
    GstVideoOverlayRectangle *rectangle = gst_video_overlay_composition_get_rectangle(owner.overlay, 0);
    GstBuffer *panel = gst_video_overlay_rectangle_get_pixels_unscaled_argb(rectangle,
        GST_VIDEO_OVERLAY_FORMAT_FLAG_NONE);
    g_assert_true(gst_buffer_map(panel, &map, GST_MAP_READ));
    guint alpha_offset = G_BYTE_ORDER == G_LITTLE_ENDIAN ? 3 : 0;
    g_assert_cmpuint(map.data[(200 * SCREEN_PANEL_WIDTH + 50) * 4 + alpha_offset], ==, 0);
    gst_buffer_unmap(panel, &map);
    video_renderer_configure_screen(SCREEN_INFO_OFF);
    clear_owner(&owner);
}

static playback_diagnostics_snapshot_t run_pipeline(gboolean overlay) {
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc pattern=black num-buffers=6 ! video/x-raw,width=640,height=360 ! "
        "vp8enc deadline=1 ! video/x-vp8,private-info=(string)PRIVATE_SCREEN_FIXTURE ! "
        "decodebin3 ! videoconvert ! video/x-raw,format=BGRA ! screenvideosink name=check_sink", &error);
    g_assert_no_error(error);
    g_assert_nonnull(pipeline);
    video_renderer_t owner = {0};
    g_mutex_init(&owner.overlay_lock);
    if (overlay) {
        owner.overlay = screen_overlay_render(&owner, "Screen mirroring\nVP8 | actual decoder observed", 640, 360);
        g_assert_nonnull(owner.overlay);
    }
    playback_diagnostics_t *diagnostics = playback_diagnostics_attach(pipeline, test_logger, 0, 777);
    playback_diagnostics_enable_screen(diagnostics, overlay ? screen_present_video_buffer : NULL, &owner);
    guint before_rendered = rendered, before_text = text_frames;
    GstBus *bus = gst_element_get_bus(pipeline);
    g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
    gboolean done = FALSE;
    while (!done) {
        GstMessage *message = gst_bus_timed_pop(bus, 5 * GST_SECOND);
        g_assert_nonnull(message);
        playback_diagnostics_message(diagnostics, message);
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *failure = NULL;
            gst_message_parse_error(message, &failure, NULL);
            g_error("Headless screen pipeline failed: %s", failure->message);
        }
        done = GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
        gst_message_unref(message);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    playback_diagnostics_snapshot_t observed;
    playback_diagnostics_get_snapshot(diagnostics, &observed);
    g_assert_cmpuint(rendered - before_rendered, ==, 6);
    g_assert_cmpuint(text_frames - before_text, ==, overlay ? 6 : 0);
    g_assert_cmpuint(observed.video.input_buffers, ==, 6);
    g_assert_true(observed.video.input_buffers_known);
    g_assert_true(observed.video.decoded_buffers_known);
    g_assert_true(observed.video.output_buffers_known);
    g_assert_cmpuint(observed.video.decoded_buffers, ==, 6);
    g_assert_cmpuint(observed.video.output_buffers, ==, 6);
    g_assert_cmpstr(observed.video.codec, ==, "VP8");
    g_assert_cmpuint(observed.video.width, ==, 640);
    g_assert_cmpuint(observed.video.height, ==, 360);
    g_assert_cmpuint(observed.video.output_width, ==, 640);
    g_assert_cmpuint(observed.video.output_height, ==, 360);
    g_assert_cmpstr(observed.video.memory, ==, "SystemMemory");
    g_assert_true(observed.video.hardware_known);
    g_assert_false(observed.video.hardware_active);
    g_assert_cmpint(observed.last_output_at_us, >, 0);
    g_assert_null(strstr((const gchar *) &observed.video.codec, "PRIVATE_SCREEN_FIXTURE"));
    GstElement *audio_decoder = gst_element_factory_make("vorbisdec", NULL);
    g_assert_nonnull(audio_decoder);
    GstMessage *qos = gst_message_new_qos(GST_OBJECT(audio_decoder), FALSE, 0, 0, 0, 0);
    gst_message_set_qos_stats(qos, GST_FORMAT_BUFFERS, 100, 99);
    playback_diagnostics_message(diagnostics, qos);
    gst_message_unref(qos);
    gst_object_unref(audio_decoder);
    playback_diagnostics_get_snapshot(diagnostics, &observed);
    g_assert_false(observed.video.dropped_known); /* audio QoS is not video loss */
    GstElement *video_sink = gst_bin_get_by_name(GST_BIN(pipeline), "check_sink");
    qos = gst_message_new_qos(GST_OBJECT(video_sink), FALSE, 0, 0, 0, 0);
    gst_message_set_qos_stats(qos, GST_FORMAT_BUFFERS, 100, 3);
    playback_diagnostics_message(diagnostics, qos);
    playback_diagnostics_message(diagnostics, qos);
    gst_message_unref(qos);
    gst_object_unref(video_sink);
    playback_diagnostics_get_snapshot(diagnostics, &observed);
    g_assert_true(observed.video.dropped_known);
    g_assert_cmpuint(observed.video.dropped_frames, ==, 3); /* cumulative, not added */

    if (overlay) {
        /* Production refresh must reject an old pipeline's measurements after
         * replacement, including when its final buffers arrive late. */
        screen_status_init(SCREEN_INFO_DEBUG, "Receiver");
        owner.screen_generation = screen_status_begin_session(SCREEN_SESSION_MIRRORING, SCREEN_ROUTE_RTP, "Old sender");
        guint64 replacement = screen_status_begin_session(SCREEN_SESSION_MIRRORING, SCREEN_ROUTE_RTP, "New sender");
        owner.diagnostics = diagnostics;
        renderer = &owner;
        video_renderer_configure_screen(SCREEN_INFO_DEBUG);
        video_renderer_screen_refresh();
        screen_status_snapshot_t snapshot;
        screen_status_get_snapshot(&snapshot);
        g_assert_cmpuint(snapshot.generation, ==, replacement);
        g_assert_cmpuint(snapshot.video.output_buffers, ==, 0);
        g_assert_cmpstr(snapshot.video.decoder, ==, "");
        renderer = NULL;
    }
    playback_diagnostics_free(diagnostics);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    clear_owner(&owner);
    return observed;
}

static void test_idle_output_handover(void) {
    screen_status_init(SCREEN_INFO_STATUS, "Receiver");
    screen_status_begin_session(SCREEN_SESSION_MIRRORING, SCREEN_ROUTE_RTP, NULL);
    video_renderer_configure_screen(SCREEN_INFO_STATUS);
    videoflip_t flips[2] = {NONE, NONE};
    video_renderer_init(test_logger, "Headless receiver", flips, "h264parse", "", "avdec_h264",
        "videoconvert", "screenvideosink", "", false, false, true, false, 3, NULL);
    video_renderer_start();
    g_assert_true(video_renderer_suspend_output());
    for (int i = 0; i < n_renderers; i++) {
        if (!renderer_type[i]) continue;
        GstState state;
        gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 0);
        g_assert_cmpint(state, ==, GST_STATE_NULL);
    }
    g_assert_cmpint(video_renderer_choose_codec(false, false), ==, 0);
    g_assert_cmpuint(video_renderer_listen(NULL, 1), ==, 0);
    g_assert_false(screen_output_suspended);
    g_assert_nonnull(renderer);
    g_assert_cmpuint(renderer->screen_generation, >, 0);
    g_assert_true(video_renderer_suspend_output());
    video_renderer_destroy();
    video_renderer_configure_screen(SCREEN_INFO_OFF);
}

static void save_frame_png(GstBuffer *buffer, const gchar *path) {
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=source format=time ! video/x-raw,format=BGRA,width=1920,height=1080,framerate=1/1 ! "
        "videoconvert ! pngenc ! filesink name=target", &error);
    g_assert_no_error(error);
    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    GstElement *target = gst_bin_get_by_name(GST_BIN(pipeline), "target");
    g_object_set(target, "location", path, NULL);
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(source), gst_buffer_ref(buffer)), ==, GST_FLOW_OK);
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    g_assert_nonnull(message);
    g_assert_cmpint(GST_MESSAGE_TYPE(message), ==, GST_MESSAGE_EOS);
    gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(target);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
}

static void test_full_debug_panel(const gchar *png_path) {
    screen_status_init(SCREEN_INFO_DEBUG, "Receiver layout fixture");
    video_renderer_configure_screen(SCREEN_INFO_DEBUG);
    guint64 generation = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO, SCREEN_ROUTE_DIRECT_HTTP,
        "Synthetic layout fixture - no live media");
    screen_status_video_t video = {0};
    g_strlcpy(video.backend, "GStreamer", sizeof(video.backend));
    g_strlcpy(video.codec, "HEVC", sizeof(video.codec));
    g_strlcpy(video.profile, "main-10", sizeof(video.profile));
    g_strlcpy(video.decode_policy, "software", sizeof(video.decode_policy));
    g_strlcpy(video.decoder, "avdec_h265", sizeof(video.decoder));
    g_strlcpy(video.memory, "SystemMemory", sizeof(video.memory));
    g_strlcpy(video.overlay_reason, "SystemMemory CPU blend", sizeof(video.overlay_reason));
    video.width = 3840; video.height = 2160; video.fps_num = 60000; video.fps_den = 1001; video.bit_depth = 10;
    video.hardware_known = TRUE; video.hardware_active = FALSE;
    video.output_width = 1920; video.output_height = 1080;
    video.output_buffers_known = TRUE; video.output_buffers = 12345;
    video.input_buffers_known = video.decoded_buffers_known = TRUE;
    video.dropped_known = TRUE; video.dropped_frames = 2;
    video.overlay_known = video.overlay_supported = TRUE;
    screen_status_set_video(generation, &video);
    screen_status_audio_t audio = {0};
    g_strlcpy(audio.codec, "AAC", sizeof(audio.codec));
    g_strlcpy(audio.output, "alsasink", sizeof(audio.output));
    audio.sample_rate = 48000; audio.channels = 2;
    audio.input_buffers_known = audio.decoded_buffers_known = audio.output_buffers_known = TRUE;
    audio.output_buffers = 98765; audio.volume_known = TRUE; audio.volume = 0.65;
    screen_status_set_audio(generation, &audio);
    screen_status_progress_t progress = {0};
    progress.position_known = progress.duration_known = progress.buffer_known = TRUE;
    progress.position_seconds = 156.7; progress.duration_seconds = 3600; progress.buffer_percent = 67;
    screen_status_set_progress(generation, &progress);
    screen_status_event(generation, SCREEN_EVENT_OUTPUT_PROGRESS);
    screen_status_snapshot_t snapshot;
    screen_status_get_snapshot(&snapshot);
    gchar text[2048];
    screen_status_format(&snapshot, text, sizeof(text), true);
    g_assert_nonnull(strstr(text, "Sender:"));
    video_renderer_t owner = {0};
    g_mutex_init(&owner.overlay_lock);
    owner.overlay = screen_overlay_render(&owner, text, 1920, 1080);
    g_assert_nonnull(owner.overlay);
    gboolean auto_resize = TRUE;
    guint text_width = 0, text_height = 0;
    gint text_x = 0, text_y = 0;
    g_object_get(owner.overlay_text, "auto-resize", &auto_resize,
        "text-width", &text_width, "text-height", &text_height, "text-x", &text_x, "text-y", &text_y, NULL);
    g_assert_false(auto_resize);
    g_assert_cmpuint(text_width, >, 500);
    g_assert_cmpuint(text_height, >, 100);
    g_assert_cmpuint(text_width + MAX(text_x, 0), <=, SCREEN_PANEL_WIDTH);
    g_assert_cmpuint(text_height + MAX(text_y, 0), <=, SCREEN_PANEL_HEIGHT);
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc pattern=smpte num-buffers=2 ! video/x-raw,format=BGRA,width=1920,height=1080 ! screenvideosink", &error);
    g_assert_no_error(error);
    playback_diagnostics_t *diagnostics = playback_diagnostics_attach(pipeline, test_logger, 0, generation);
    playback_diagnostics_enable_screen(diagnostics, screen_present_video_buffer, &owner);
    capture_frame = TRUE;
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    g_assert_nonnull(message);
    g_assert_cmpint(GST_MESSAGE_TYPE(message), ==, GST_MESSAGE_EOS);
    gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    capture_frame = FALSE;
    g_assert_nonnull(captured_frame);
    if (png_path) save_frame_png(captured_frame, png_path);
    gst_buffer_unref(captured_frame); captured_frame = NULL;
    playback_diagnostics_free(diagnostics);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    clear_owner(&owner);
    video_renderer_configure_screen(SCREEN_INFO_OFF);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    g_assert_true(gst_element_register(NULL, "screenvideosink", GST_RANK_NONE, screen_video_sink_get_type()));
    test_logger = logger_init();
    logger = test_logger;
    logger_set_callback(test_logger, capture_log, NULL);
    logger_set_level(test_logger, LOGGER_INFO);
    test_pixels_and_opaque_guard();
    playback_diagnostics_snapshot_t baseline = run_pipeline(FALSE);
    playback_diagnostics_snapshot_t overlay = run_pipeline(TRUE);
    g_assert_cmpstr(baseline.video.decoder, ==, overlay.video.decoder);
    g_assert_cmpstr(baseline.video.memory, ==, overlay.video.memory);
    g_assert_cmpuint(baseline.video.output_width, ==, overlay.video.output_width);
    test_idle_output_handover();
    test_full_debug_panel(argc == 3 && !strcmp(argv[1], "--debug-png") ? argv[2] : NULL);
    logger_destroy(test_logger);
    gst_deinit();
    return 0;
}
