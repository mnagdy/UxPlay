/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "screen_status_renderer.h"
#include "screen_status.h"

#include <gst/gst.h>
#include <string.h>

struct screen_status_renderer_s {
    GMutex mutex;
    logger_t *logger;
    char *sink_factory, *sink_options;
    GstElement *pipeline, *overlay;
    bool visible;
    screen_info_mode_t last_mode;
    char last_text[4096];
};

static void safe_error(screen_status_renderer_t *r, const char *stage) {
    if (r->logger) logger_log(r->logger, LOGGER_WARNING,
        "Status screen: stage=%s output unavailable; another display owner must not start before release", stage);
}

static bool factory_name_valid(const char *name) {
    if (!name || !*name || strlen(name) > 96) return false;
    for (const char *p = name; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-') return false;
    return true;
}

/* Construct exactly one caller-configured video sink. Configuration text is
 * never parsed as a pipeline, so extensions cannot add an audio owner. */
static bool set_sink_options(GstElement *sink, const char *options) {
    if (!options || !*options) return true;
    gchar **argv = NULL;
    gint argc = 0;
    GError *error = NULL;
    if (!g_shell_parse_argv(options, &argc, &argv, &error)) {
        g_clear_error(&error);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < argc && ok; i++) {
        char *equals = strchr(argv[i], '=');
        if (!equals || equals == argv[i]) { ok = false; break; }
        *equals++ = '\0';
        GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(sink), argv[i]);
        if (!pspec || !(pspec->flags & G_PARAM_WRITABLE) || (pspec->flags & G_PARAM_CONSTRUCT_ONLY)) {
            ok = false;
            break;
        }
        GType type = G_PARAM_SPEC_VALUE_TYPE(pspec);
        if (!(type == G_TYPE_STRING || type == G_TYPE_BOOLEAN || type == G_TYPE_INT ||
              type == G_TYPE_UINT || type == G_TYPE_INT64 || type == G_TYPE_UINT64 ||
              type == G_TYPE_LONG || type == G_TYPE_ULONG || type == G_TYPE_FLOAT ||
              type == G_TYPE_DOUBLE || G_TYPE_IS_ENUM(type) || G_TYPE_IS_FLAGS(type))) {
            ok = false;
            break;
        }
        GValue value = G_VALUE_INIT;
        g_value_init(&value, type);
        if (type == G_TYPE_STRING) g_value_set_string(&value, equals);
        else if (!gst_value_deserialize(&value, equals)) ok = false;
        if (ok && g_param_value_validate(pspec, &value)) ok = false;
        if (ok) g_object_set_property(G_OBJECT(sink), argv[i], &value);
        g_value_unset(&value);
    }
    g_strfreev(argv);
    return ok;
}

screen_status_renderer_t *screen_status_renderer_new(logger_t *logger,
                                                     const char *sink,
                                                     const char *sink_options) {
    if (!factory_name_valid(sink) || (sink_options && strlen(sink_options) > 4096)) return NULL;
    screen_status_renderer_t *r = g_new0(screen_status_renderer_t, 1);
    g_mutex_init(&r->mutex);
    r->logger = logger;
    r->sink_factory = g_strdup(sink);
    r->sink_options = g_strdup(sink_options ? sink_options : "");
    return r;
}

/* Caller holds r->mutex. Keep the owner object and pipeline if release cannot
 * be established. Never start a replacement sink on an assumed release. */
static bool hide_locked(screen_status_renderer_t *r) {
    if (!r->pipeline) { r->visible = false; return true; }
    GstStateChangeReturn change = gst_element_set_state(r->pipeline, GST_STATE_NULL);
    GstState current = GST_STATE_VOID_PENDING, pending = GST_STATE_VOID_PENDING;
    GstStateChangeReturn settled = gst_element_get_state(r->pipeline, &current, &pending, GST_SECOND);
    if (change == GST_STATE_CHANGE_FAILURE || settled == GST_STATE_CHANGE_FAILURE ||
        current != GST_STATE_NULL || pending != GST_STATE_VOID_PENDING) {
        safe_error(r, "release");
        return false;
    }
    gst_object_unref(r->pipeline);
    r->pipeline = NULL;
    r->overlay = NULL;
    r->visible = false;
    r->last_text[0] = '\0';
    return true;
}

/* Bound lines before handing text to textoverlay. Its default wrap width is
 * the entire video width, which can clip when xpad is then added. Count wide
 * Unicode characters conservatively and retain every supplied field. */
static gchar *wrap_text(const char *text, unsigned columns) {
    GString *wrapped = g_string_new(NULL);
    unsigned used = 0;
    const char *p = text;
    while (*p) {
        if (*p == '\n') { g_string_append_c(wrapped, '\n'); used = 0; p++; continue; }
        if (*p == ' ') { p++; continue; }
        const char *end = p;
        unsigned width = 0;
        while (*end && *end != ' ' && *end != '\n') {
            gunichar ch = g_utf8_get_char(end);
            width += g_unichar_iswide(ch) ? 2 : 1;
            end = g_utf8_next_char(end);
        }
        if (used && used + 1 + width > columns) { g_string_append_c(wrapped, '\n'); used = 0; }
        if (used) { g_string_append_c(wrapped, ' '); used++; }
        while (p < end) {
            gunichar ch = g_utf8_get_char(p);
            unsigned cells = g_unichar_iswide(ch) ? 2 : 1;
            if (used && used + cells > columns) { g_string_append_c(wrapped, '\n'); used = 0; }
            const char *next = g_utf8_next_char(p);
            g_string_append_len(wrapped, p, next - p);
            used += cells;
            p = next;
        }
    }
    return g_string_free(wrapped, FALSE);
}

static void set_text_locked(screen_status_renderer_t *r, const char *text, screen_info_mode_t mode) {
    if (!r->overlay || (!strcmp(r->last_text, text) && r->last_mode == mode)) return;
    /* textoverlay's text property accepts Pango markup. The model produces
     * plain text, so this renderer, not the model, performs markup escaping. */
    gchar *wrapped = wrap_text(text, mode == SCREEN_INFO_DEBUG ? 60 : 32);
    gchar *escaped = g_markup_escape_text(wrapped, -1);
    /* Absolute pixel sizes avoid textoverlay's screen-size multiplication.
     * 20px diagnostics / 32px status fit the fixed 1280x720 idle surface. */
    g_object_set(r->overlay, "font-desc", mode == SCREEN_INFO_DEBUG ? "Sans 20px" : "Sans 32px",
                 "auto-resize", FALSE, "text", escaped, NULL);
    g_free(wrapped);
    g_free(escaped);
    g_strlcpy(r->last_text, text, sizeof(r->last_text));
    r->last_mode = mode;
}

static bool create_locked(screen_status_renderer_t *r, const char *text, screen_info_mode_t mode) {
    GstElement *pipeline = gst_pipeline_new("receiver-status");
    GstElement *source = gst_element_factory_make("videotestsrc", "status-background");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "status-caps");
    GstElement *overlay = gst_element_factory_make("textoverlay", "status-text");
    GstElement *converter = gst_element_factory_make("videoconvert", "status-converter");
    GstElement *sink = gst_element_factory_make(r->sink_factory, "status-video-output");
    if (!pipeline || !source || !capsfilter || !overlay || !converter || !sink) {
        if (source) gst_object_unref(source);
        if (capsfilter) gst_object_unref(capsfilter);
        if (overlay) gst_object_unref(overlay);
        if (converter) gst_object_unref(converter);
        if (sink) gst_object_unref(sink);
        if (pipeline) gst_object_unref(pipeline);
        safe_error(r, "plugins");
        return false;
    }
    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, overlay, converter, sink, NULL);
    /* Refuse an audio sink even if its factory name came from configuration. */
    GstElementFactory *factory = gst_element_get_factory(sink);
    bool video_sink = factory && gst_element_factory_list_is_type(factory,
                       GST_ELEMENT_FACTORY_TYPE_SINK | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO);
    bool test_sink = !strcmp(r->sink_factory, "fakesink");
    if ((!video_sink && !test_sink) || !set_sink_options(sink, r->sink_options)) {
        gst_object_unref(pipeline);
        safe_error(r, "sink-options");
        return false;
    }
    g_object_set(source, "is-live", TRUE, NULL);
    gst_util_set_object_arg(G_OBJECT(source), "pattern", "black");
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, 1280,
        "height", G_TYPE_INT, 720, "framerate", GST_TYPE_FRACTION, 1, 1, NULL);
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);
    g_object_set(overlay, "shaded-background", FALSE,
        "draw-shadow", FALSE, "draw-outline", FALSE, "xpad", 64, "ypad", 48, NULL);
    gst_util_set_object_arg(G_OBJECT(overlay), "halignment", "left");
    gst_util_set_object_arg(G_OBJECT(overlay), "line-alignment", "left");
    gst_util_set_object_arg(G_OBJECT(overlay), "valignment", "top");
    gst_util_set_object_arg(G_OBJECT(overlay), "wrap-mode", "none");
    if (!gst_element_link_many(source, capsfilter, overlay, converter, sink, NULL)) {
        gst_object_unref(pipeline);
        safe_error(r, "link");
        return false;
    }
    r->pipeline = pipeline;
    r->overlay = overlay; /* Borrowed from pipeline. */
    set_text_locked(r, text, mode);
    GstStateChangeReturn change = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstState current = GST_STATE_VOID_PENDING;
    GstStateChangeReturn settled = gst_element_get_state(pipeline, &current, NULL, GST_SECOND);
    if (change == GST_STATE_CHANGE_FAILURE || settled == GST_STATE_CHANGE_FAILURE || current != GST_STATE_PLAYING) {
        safe_error(r, "start");
        hide_locked(r);
        return false;
    }
    r->visible = true;
    return true;
}

bool screen_status_renderer_show(screen_status_renderer_t *r) {
    if (!r) return false;
    screen_status_snapshot_t snapshot;
    screen_status_get_snapshot(&snapshot);
    char text[4096];
    screen_status_format(&snapshot, text, sizeof(text), false);
    g_mutex_lock(&r->mutex);
    bool ok;
    if (snapshot.mode == SCREEN_INFO_OFF) ok = hide_locked(r);
    else if (r->pipeline && !r->visible) ok = false; /* Unreleased prior output. */
    else if (r->visible) { set_text_locked(r, text, snapshot.mode); ok = true; }
    else ok = create_locked(r, text, snapshot.mode);
    g_mutex_unlock(&r->mutex);
    return ok;
}

bool screen_status_renderer_tick(screen_status_renderer_t *r) {
    if (!r) return false;
    screen_status_snapshot_t snapshot;
    screen_status_get_snapshot(&snapshot);
    char text[4096];
    screen_status_format(&snapshot, text, sizeof(text), false);
    g_mutex_lock(&r->mutex);
    bool ok = true;
    if (snapshot.mode == SCREEN_INFO_OFF) ok = hide_locked(r);
    else if (r->visible) {
        GstBus *bus = gst_element_get_bus(r->pipeline);
        GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        if (message) {
            /* GStreamer errors can contain private upstream strings. */
            gst_message_unref(message);
            safe_error(r, "render");
            hide_locked(r);
            ok = false;
        } else set_text_locked(r, text, snapshot.mode);
        gst_object_unref(bus);
    }
    g_mutex_unlock(&r->mutex);
    return ok;
}

bool screen_status_renderer_hide(screen_status_renderer_t *r) {
    if (!r) return true;
    g_mutex_lock(&r->mutex);
    bool ok = hide_locked(r);
    g_mutex_unlock(&r->mutex);
    return ok;
}

bool screen_status_renderer_is_visible(screen_status_renderer_t *r) {
    if (!r) return false;
    g_mutex_lock(&r->mutex);
    bool visible = r->visible;
    g_mutex_unlock(&r->mutex);
    return visible;
}

bool screen_status_renderer_free(screen_status_renderer_t *r) {
    if (!r) return true;
    if (!screen_status_renderer_hide(r)) return false;
    g_mutex_clear(&r->mutex);
    g_free(r->sink_factory);
    g_free(r->sink_options);
    g_free(r);
    return true;
}
