/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the production binary-plist HTTP handlers and FCUP serialization.
 * Only the reverse-channel socket lookup is replaced; requests use a local
 * socketpair, so no network server, phone, or renderer is needed.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define httpd_get_connection_socket_by_type test_reverse_socket
#define httpd_get_connection_by_type test_reverse_connection
#define httpd_get_connection_socket test_connection_socket
#include "../lib/raop.c"
#undef httpd_get_connection_socket_by_type
#undef httpd_get_connection_by_type
#undef httpd_get_connection_socket
/* Inspect saved handshake state when malformed requests are rejected. */
#include "../lib/fairplay_playfair.c"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static int reverse_pair[2] = {-1, -1};
static raop_conn_t *test_reverse_connections[3];
static int test_reverse_sockets[3];
static unsigned test_reverse_count;
static const char session_id[] = "11111111-2222-3333-4444-555555555555";
static const char playback_uuid[] = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
static const char prefix[] = "mlhls://fixture.invalid/current-video";
static const char master_url[] = "mlhls://fixture.invalid/current-video/master.m3u8";
static const char media0[] = "mlhls://fixture.invalid/current-video/itag/100/mediadata.m3u8";
static const char media1[] = "mlhls://fixture.invalid/current-video/itag/200/mediadata.m3u8";
static const char media2[] = "mlhls://fixture.invalid/current-video/itag/300/mediadata.m3u8";
static const char master[] =
    "#EXTM3U\n#EXT-X-VERSION:3\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=200000\n"
    "mlhls://fixture.invalid/current-video/itag/100/mediadata.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
    "mlhls://fixture.invalid/current-video/itag/200/mediadata.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=800000\n"
    "mlhls://fixture.invalid/current-video/itag/300/mediadata.m3u8\n";
static const char media[] =
    "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:180\n"
    "#EXT-X-PLAYLIST-TYPE:VOD\n#EXTINF:180.0,\n"
    "https://fixture.invalid/generated-segment.ts\n#EXT-X-ENDLIST\n";

typedef struct {
    raop_t raop;
    raop_conn_t conn;
    raop_conn_t reverse_conn;
    unsigned int play_calls;
    unsigned int reset_calls;
    unsigned int request_calls, request_error_calls, rate_calls, scrub_calls, stop_calls, info_calls;
    bool request_direct_http;
    float played_position;
    bool expected_direct_http;
    char played_location[256];
} fixture_t;

int test_reverse_socket(httpd_t *httpd, connection_type_t type, int instance) {
    (void)httpd;
    assert(type == CONNECTION_TYPE_PTTH);
    assert(instance == 1);
    return reverse_pair[0];
}

void *test_reverse_connection(httpd_t *httpd, connection_type_t type, int instance) {
    (void)httpd;
    if (type != CONNECTION_TYPE_PTTH || instance < 1 || (unsigned)instance > test_reverse_count) return NULL;
    return test_reverse_connections[instance - 1];
}

int test_connection_socket(httpd_t *httpd, void *data) {
    (void)httpd;
    for (unsigned i = 0; i < test_reverse_count; i++)
        if (test_reverse_connections[i] == data) return test_reverse_sockets[i];
    return -1;
}

static void test_log(void *cls, int level, const char *message) {
    (void)cls;
    (void)level;
    printf("%s\n", message);
}

static void played(void *cls, const char *location, const float position, bool direct_http) {
    fixture_t *f = cls;
    assert(direct_http == f->expected_direct_http);
    f->play_calls++;
    f->played_position = position;
    snprintf(f->played_location, sizeof(f->played_location), "%s", location);
}

static void reset_connection(void *cls, int count) {
    (void)count;
    ((fixture_t *)cls)->reset_calls++;
}

static void video_requested(void *cls, bool direct_http) {
    fixture_t *f = cls;
    f->request_calls++;
    f->request_direct_http = direct_http;
}

static void request_failed(void *cls) { ((fixture_t *)cls)->request_error_calls++; }
static void rate_changed(void *cls, float rate) { (void)rate; ((fixture_t *)cls)->rate_calls++; }
static void scrub_changed(void *cls, float position) { (void)position; ((fixture_t *)cls)->scrub_calls++; }
static void stopped(void *cls) { ((fixture_t *)cls)->stop_calls++; }
static void playback_info(void *cls, playback_info_t *info) {
    ((fixture_t *)cls)->info_calls++;
    memset(info, 0, sizeof(*info));
    info->duration = 180;
    info->position = 1;
}

static void fixture_init(fixture_t *f) {
    memset(f, 0, sizeof(*f));
    f->raop.current_video = -1;
    f->raop.port = 7000;
    f->raop.lang = "";
    f->raop.logger = logger_init();
    logger_set_callback(f->raop.logger, test_log, NULL);
    logger_set_level(f->raop.logger, LOGGER_INFO);
    f->raop.callbacks.cls = f;
    f->raop.callbacks.on_video_play = played;
    f->raop.callbacks.on_video_request = video_requested;
    f->raop.callbacks.on_video_request_error = request_failed;
    f->raop.callbacks.on_video_rate = rate_changed;
    f->raop.callbacks.on_video_scrub = scrub_changed;
    f->raop.callbacks.on_video_stop = stopped;
    f->raop.callbacks.on_video_acquire_playback_info = playback_info;
    f->raop.callbacks.conn_reset = reset_connection;
    f->conn.raop = &f->raop;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, reverse_pair) == 0);
    assert(fcntl(reverse_pair[1], F_SETFL, O_NONBLOCK) == 0);
    f->reverse_conn.raop = &f->raop;
    f->reverse_conn.client_session_id = (char *)session_id;
    f->reverse_conn.reverse_registration_order = 1;
    test_reverse_count = 1;
    test_reverse_connections[0] = &f->reverse_conn;
    test_reverse_sockets[0] = reverse_pair[0];
}

static void fixture_destroy(fixture_t *f) {
    raop_destroy_airplay_video(&f->raop, -1);
    logger_destroy(f->raop.logger);
    close(reverse_pair[0]);
    close(reverse_pair[1]);
    reverse_pair[0] = reverse_pair[1] = -1;
    test_reverse_count = 0;
}

/* Assert exactly whether production sent another reverse HTTP request. */
static void expect_request(const char *expected_url) {
    char received[32768];
    size_t length = 0;
    for (;;) {
        ssize_t n = recv(reverse_pair[1], received + length,
                         sizeof(received) - length - 1, 0);
        if (n > 0) {
            length += (size_t)n;
            assert(length < sizeof(received) - 1);
        } else {
            assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            break;
        }
    }
    received[length] = '\0';
    if (expected_url) {
        assert(length > 0);
        assert(strstr(received, "POST /event HTTP/1.1"));
        assert(strstr(received, expected_url));
    } else {
        assert(length == 0);
    }
}

static int invoke_request(fixture_t *f, raop_handler_t handler, const char *method,
                          const char *protocol, const char *path, plist_t root) {
    char *body = NULL;
    uint32_t body_length = 0;
    plist_to_bin(root, &body, &body_length);
    plist_free(root);
    assert(body && body_length > 0);
    char header[512];
    int header_length = snprintf(header, sizeof(header),
        "%s %s %s\r\nX-Apple-Session-ID: %s\r\n"
        "Content-Type: application/x-apple-binary-plist\r\nContent-Length: %u\r\n\r\n",
        method, path, protocol, session_id, body_length);
    assert(header_length > 0 && header_length < (int)sizeof(header));
    http_request_t *request = http_request_init();
    assert(http_request_add_data(request, header, header_length) == 0);
    assert(http_request_add_data(request, body, body_length) == 0);
    plist_mem_free(body);
    assert(http_request_is_complete(request));
    assert(!http_request_has_error(request));
    http_response_t *response = http_response_create();
    http_response_init(response, protocol, 200, "OK");
    char *response_body = NULL;
    int response_length = 0;
    handler(&f->conn, request, response, &response_body, &response_length);
    http_response_finish(response, response_body, response_length);
    const char *serialized = http_response_get_data(response, &response_length);
    int status = 0;
    assert(sscanf(serialized, "%*s %d", &status) == 1);
    free(response_body);
    http_response_destroy(response);
    http_request_destroy(request);
    return status;
}

static int invoke(fixture_t *f, raop_handler_t handler, const char *path, plist_t root) {
    return invoke_request(f, handler, "POST", "HTTP/1.1", path, root);
}

/* When handler is NULL, exercise the dispatch guard before connection
 * classification. Such calls below are intentionally stale and must stop
 * before any live HTTP server/socket state would be needed. */
static int invoke_plain(fixture_t *f, raop_handler_t handler, const char *method,
                        const char *path, const char *session, char *reply, size_t capacity) {
    char header[1024];
    int length = snprintf(header, sizeof(header), "%s %s HTTP/1.1\r\nHost: localhost:7000\r\n%s%s%sContent-Length: 0\r\n\r\n",
        method, path, session ? "X-Apple-Session-ID: " : "", session ? session : "", session ? "\r\n" : "");
    assert(length > 0 && length < (int)sizeof(header));
    http_request_t *request = http_request_init();
    assert(http_request_add_data(request, header, length) == 0);
    assert(http_request_is_complete(request));
    http_response_t *response = NULL;
    char *body = NULL;
    int body_length = 0;
    if (handler) {
        response = http_response_create();
        http_response_init(response, "HTTP/1.1", 200, "OK");
        handler(&f->conn, request, response, &body, &body_length);
        http_response_finish(response, body, body_length);
    } else conn_request(&f->conn, request, &response);
    assert(response);
    const char *serialized = http_response_get_data(response, &length);
    int code = 0;
    assert(sscanf(serialized, "%*s %d", &code) == 1);
    if (reply && capacity) {
        size_t copy = (size_t)length < capacity - 1 ? (size_t)length : capacity - 1;
        memcpy(reply, serialized, copy);
        reply[copy] = '\0';
    }
    free(body);
    http_response_destroy(response);
    http_request_destroy(request);
    return code;
}

static void play_request(fixture_t *f) {
    plist_t root = plist_new_dict();
    plist_dict_set_item(root, "uuid", plist_new_string(playback_uuid));
    plist_dict_set_item(root, "Content-Location", plist_new_string(master_url));
    plist_dict_set_item(root, "Start-Position-Seconds", plist_new_real(0.0));
    plist_dict_set_item(root, "clientProcName", plist_new_string("YouTube"));
    assert(invoke(f, http_handler_play, "/play", root) == 200);
    assert(f->reset_calls == 0);
}

static void test_direct_http_route_metadata(void) {
    const char *locations[] = {"http://fixture.invalid/master.m3u8", "https://fixture.invalid/stream.m3u8"};
    for (unsigned int i = 0; i < sizeof(locations) / sizeof(locations[0]); i++) {
        fixture_t f;
        fixture_init(&f);
        f.expected_direct_http = true;
        plist_t root = plist_new_dict();
        plist_dict_set_item(root, "uuid", plist_new_string(playback_uuid));
        plist_dict_set_item(root, "Content-Location", plist_new_string(locations[i]));
        plist_dict_set_item(root, "Start-Position-Seconds", plist_new_real(3.5));
        plist_dict_set_item(root, "clientProcName", plist_new_string("UHF"));
        assert(invoke(&f, http_handler_play, "/play", root) == 200);
        assert(f.play_calls == 1 && f.played_position == 3.5f);
        assert(!strcmp(f.played_location, locations[i]));
        expect_request(NULL);
        fixture_destroy(&f);
    }
}

static int action_response(fixture_t *f, const char *url, unsigned int id,
                           unsigned int status, const char *playlist) {
    plist_t root = plist_new_dict();
    plist_dict_set_item(root, "type", plist_new_string("unhandledURLResponse"));
    plist_t params = plist_new_dict();
    plist_dict_set_item(params, "FCUP_Response_URL", plist_new_string(url));
    plist_dict_set_item(params, "FCUP_Response_RequestID", plist_new_uint(id));
    plist_dict_set_item(params, "FCUP_Response_StatusCode", plist_new_uint(status));
    if (playlist) {
        plist_dict_set_item(params, "FCUP_Response_Data", plist_new_data(playlist, strlen(playlist)));
    }
    plist_dict_set_item(root, "params", params);
    return invoke(f, http_handler_action, "/action", root);
}

static airplay_video_t *start_collection(fixture_t *f) {
    play_request(f);
    expect_request(master_url);
    assert(f->play_calls == 0);
    assert(action_response(f, master_url, 1, 200, master) == 200);
    expect_request(media0);
    airplay_video_t *video = hls_get_current_video(&f->raop);
    assert(get_num_media_uri(video) == 3);
    assert(get_next_media_uri_id(video) == 1);
    assert(!strcmp(get_uri_prefix(video), prefix));
    return video;
}

static void test_partial_cache_never_resumes(void) {
    fixture_t f;
    fixture_init(&f);
    airplay_video_t *video = start_collection(&f);
    assert(action_response(&f, media0, 2, 200, media) == 200);
    expect_request(media1);
    assert(get_next_media_uri_id(video) == 2);
    assert(f.play_calls == 0);
    set_resume_position_seconds(video, 165.0f);

    /* Ads or interrupted playlist acquisition can repeat the same UUID while
     * its playback URL exists but later advertised variants are still absent. */
    play_request(&f);
    assert(f.play_calls == 0);
    expect_request(master_url);
    video = hls_get_current_video(&f.raop);
    assert(get_num_media_uri(video) == 0);
    assert(get_next_media_uri_id(video) == 0);
    fixture_destroy(&f);
}

static void test_complete_cache_resumes_without_refetch(void) {
    fixture_t f;
    fixture_init(&f);
    airplay_video_t *video = start_collection(&f);
    assert(action_response(&f, media0, 2, 200, media) == 200);
    expect_request(media1);
    assert(action_response(&f, media1, 3, 200, media) == 200);
    expect_request(media2);
    assert(action_response(&f, media2, 4, 200, media) == 200);
    expect_request(NULL);
    assert(f.play_calls == 1);
    /* Retrying the final FCUP response must not restart already playing video. */
    assert(action_response(&f, media2, 4, 200, media) == 200);
    expect_request(NULL);
    assert(f.play_calls == 1);
    set_resume_position_seconds(video, 165.0f);
    play_request(&f);
    expect_request(NULL);
    assert(f.play_calls == 2);
    assert(f.played_position == 165.0f);
    assert(hls_get_current_video(&f.raop) == video);
    assert(!strcmp(f.played_location, "http://localhost:7000/master.m3u8"));
    fixture_destroy(&f);
}

static void test_stale_responses_do_not_advance_collection(void) {
    fixture_t f;
    fixture_init(&f);
    airplay_video_t *video = start_collection(&f);
    const char *stale = "mlhls://fixture.invalid/previous-video/itag/100/mediadata.m3u8";
    assert(action_response(&f, stale, 2, 200, media) == 200);
    expect_request(NULL);
    assert(get_next_media_uri_id(video) == 1);
    int count = 0;
    float duration = 0;
    assert(!get_media_playlist(video, &count, &duration, "/itag/100/mediadata.m3u8"));
    assert(f.play_calls == 0);

    /* A current-prefix but out-of-order reply is also not the pending slot. */
    assert(action_response(&f, media1, 3, 200, media) == 200);
    expect_request(NULL);
    assert(get_next_media_uri_id(video) == 1);

    assert(action_response(&f, media0, 2, 200, media) == 200);
    expect_request(media1);
    assert(get_next_media_uri_id(video) == 2);
    assert(action_response(&f, media0, 2, 200, media) == 200); /* duplicate */
    expect_request(NULL);
    assert(get_next_media_uri_id(video) == 2);
    assert(!get_media_playlist(video, &count, &duration, "/itag/200/mediadata.m3u8"));
    assert(f.play_calls == 0);
    fixture_destroy(&f);
}

static void test_failed_media_variant_is_removed(unsigned int status, const char *payload) {
    fixture_t f;
    fixture_init(&f);
    airplay_video_t *video = start_collection(&f);
    assert(action_response(&f, media0, 2, 200, media) == 200);
    expect_request(media1);

    /* An unavailable rendition is not a failed video: keep collecting others. */
    assert(action_response(&f, media1, 3, status, payload) == 200);
    expect_request(media2);
    assert(get_next_media_uri_id(video) == 3);
    assert(f.play_calls == 0);
    assert(!airplay_video_is_ready(video));
    int count = 0;
    float duration = 0;
    assert(!get_media_playlist(video, &count, &duration, "/itag/200/mediadata.m3u8"));

    assert(action_response(&f, media2, 4, 200, media) == 200);
    expect_request(NULL);
    assert(f.play_calls == 1);
    assert(airplay_video_is_ready(video));
    const char *available_master = get_master_playlist(video);
    assert(available_master);
    assert(strstr(available_master, "/itag/100/mediadata.m3u8"));
    assert(strstr(available_master, "/itag/300/mediadata.m3u8"));
    assert(!strstr(available_master, "/itag/200/mediadata.m3u8"));
    assert(strstr(available_master, "BANDWIDTH=200000"));
    assert(strstr(available_master, "BANDWIDTH=800000"));
    assert(!strstr(available_master, "BANDWIDTH=400000"));
    assert(get_media_playlist(video, &count, &duration, "/itag/100/mediadata.m3u8"));
    assert(get_media_playlist(video, &count, &duration, "/itag/300/mediadata.m3u8"));
    assert(!get_media_playlist(video, &count, &duration, "/itag/200/mediadata.m3u8"));

    /* A finalized cache remains reusable without resurrecting its bad variant. */
    set_resume_position_seconds(video, 165.0f);
    play_request(&f);
    expect_request(NULL);
    assert(f.play_calls == 2);
    assert(f.played_position == 165.0f);
    assert(!strstr(get_master_playlist(video), "/itag/200/mediadata.m3u8"));
    fixture_destroy(&f);
}

static void test_missing_or_malformed_master_is_rejected(void) {
    fixture_t f;
    fixture_init(&f);
    play_request(&f);
    expect_request(master_url);
    assert(action_response(&f, master_url, 1, 404, "not found") == 400);
    expect_request(NULL);
    assert(f.play_calls == 0);
    airplay_video_t *video = hls_get_current_video(&f.raop);
    assert(get_num_media_uri(video) == 0);
    assert(get_next_media_uri_id(video) == 0);
    assert(!airplay_video_is_ready(video));

    assert(action_response(&f, master_url, 1, 200, "not a playlist") == 400);
    expect_request(NULL);
    assert(f.play_calls == 0);
    assert(!airplay_video_is_ready(video));
    assert(action_response(&f, master_url, 1, 200, "#EXTM3U\n#EXT-X-VERSION:3\n") == 400);
    expect_request(NULL);
    assert(f.play_calls == 0);
    assert(!airplay_video_is_ready(video));
    fixture_destroy(&f);
}

static void test_all_media_variants_failed_is_rejected(void) {
    fixture_t f;
    fixture_init(&f);
    airplay_video_t *video = start_collection(&f);
    assert(action_response(&f, media0, 2, 404, "not found") == 200);
    expect_request(media1);
    assert(action_response(&f, media1, 3, 200, "not a playlist") == 200);
    expect_request(media2);
    assert(action_response(&f, media2, 4, 404, "not found") == 400);
    expect_request(NULL);
    assert(f.play_calls == 0);
    assert(!airplay_video_is_ready(video));
    fixture_destroy(&f);
}

static void test_duplicate_uri_requires_current_request_id(void) {
    static const char duplicate_master[] =
        "#EXTM3U\n#EXT-X-VERSION:3\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000\n"
        "mlhls://fixture.invalid/current-video/itag/100/mediadata.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
        "mlhls://fixture.invalid/current-video/itag/100/mediadata.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000\n"
        "mlhls://fixture.invalid/current-video/itag/300/mediadata.m3u8\n";
    fixture_t f;
    fixture_init(&f);
    play_request(&f);
    expect_request(master_url);
    assert(action_response(&f, master_url, 1, 200, duplicate_master) == 200);
    expect_request(media0);
    airplay_video_t *video = hls_get_current_video(&f.raop);
    assert(get_num_media_uri(video) == 3);
    assert(get_next_media_uri_id(video) == 1);

    /* Requests 2 and 3 deliberately have the same URL. The first fails, then
     * its delayed duplicate must not be mistaken for the second response. */
    assert(action_response(&f, media0, 2, 404, "not found") == 200);
    expect_request(media0);
    assert(get_next_media_uri_id(video) == 2);
    assert(action_response(&f, media0, 2, 404, "not found") == 200);
    expect_request(NULL);
    assert(get_next_media_uri_id(video) == 2);
    assert(f.play_calls == 0);

    assert(action_response(&f, media0, 3, 200, media) == 200);
    expect_request(media2);
    assert(get_next_media_uri_id(video) == 3);
    assert(f.play_calls == 0);
    assert(action_response(&f, media2, 4, 200, media) == 200);
    expect_request(NULL);
    assert(f.play_calls == 1);
    assert(airplay_video_is_ready(video));
    int count = 0;
    float duration = 0;
    assert(get_media_playlist(video, &count, &duration, "/itag/100/mediadata.m3u8"));
    assert(get_media_playlist(video, &count, &duration, "/itag/300/mediadata.m3u8"));
    fixture_destroy(&f);
}

static void test_pi4_profile_reaches_download_finalizer(bool enabled, bool mpv) {
    static const char mixed_master[] =
        "#EXTM3U\n#EXT-X-VERSION:3\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,CODECS=\"avc1.64001f,mp4a.40.2\",RESOLUTION=1280x720\n"
        "mlhls://fixture.invalid/current-video/itag/100/mediadata.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=400000,CODECS=\"vp09.00.10.08,mp4a.40.2\",RESOLUTION=1280x720\n"
        "mlhls://fixture.invalid/current-video/itag/200/mediadata.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS=\"avc1.640033,mp4a.40.2\",RESOLUTION=3840x2160\n"
        "mlhls://fixture.invalid/current-video/itag/300/mediadata.m3u8\n";
    fixture_t f;
    fixture_init(&f);
    assert(raop_set_plist(&f.raop, "hls_pi4", enabled) == 0);
    assert(raop_set_plist(&f.raop, "hls_mpv", mpv) == 0);
    play_request(&f);
    expect_request(master_url);
    assert(action_response(&f, master_url, 1, 200, mixed_master) == 200);
    expect_request(media0);
    assert(action_response(&f, media0, 2, 200, media) == 200);
    if (!(enabled && mpv)) {
        expect_request(media1);
        assert(action_response(&f, media1, 3, 200, media) == 200);
        expect_request(media2);
        assert(action_response(&f, media2, 4, 200, media) == 200);
    }
    expect_request(NULL);
    assert(f.play_calls == 1);
    airplay_video_t *video = hls_get_current_video(&f.raop);
    assert(airplay_video_is_ready(video));
    assert(get_num_media_uri(video) == (enabled ? 1 : 3));
    const char *filtered = get_master_playlist(video);
    assert(strstr(filtered, "/itag/100/mediadata.m3u8"));
    assert((strstr(filtered, "vp09") != NULL) == !enabled);
    assert((strstr(filtered, "3840x2160") != NULL) == !enabled);
    play_request(&f);
    expect_request(NULL);
    assert(f.play_calls == 2);
    fixture_destroy(&f);
}

static void test_mpv_scoped_quality_selection(bool high_available) {
    const char *master =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,CODECS=\"avc1.64001f,mp4a.40.2\",RESOLUTION=1280x720\n"
        "mlhls://fixture.invalid/current-video/itag/100/mediadata.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS=\"avc1.64002a,mp4a.40.2\",RESOLUTION=1920x1080,FRAME-RATE=60\n"
        "mlhls://fixture.invalid/current-video/itag/200/mediadata.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=900000,CODECS=\"vp09.00.40.08,mp4a.40.2\",RESOLUTION=1920x1080\n"
        "mlhls://fixture.invalid/current-video/itag/300/mediadata.m3u8\n";
    fixture_t f;
    fixture_init(&f);
    f.raop.hls_pi4 = f.raop.hls_mpv = f.raop.hls_scoped_cache = true;
    unsigned char local[4] = {127, 0, 0, 1};
    f.conn.remote = local;
    f.conn.remotelen = sizeof(local);
    play_request(&f);
    expect_request(master_url);
    assert(action_response(&f, master_url, 1, 200, master) == 200);
    expect_request(media0);
    assert(action_response(&f, media0, 2, 200, media) == 200);
    expect_request(media1);
    assert(action_response(&f, media1, 3, high_available ? 200 : 404, high_available ? media : "missing") == 200);
    expect_request(NULL);
    assert(f.play_calls == 1);
    airplay_video_t *video = hls_get_current_video(&f.raop);
    assert(airplay_video_is_ready(video));
    assert(get_num_media_uri(video) == 1);
    const char *filtered = get_master_playlist(video);
    assert((strstr(filtered, "/itag/200/") != NULL) == high_available);
    assert((strstr(filtered, "/itag/100/") != NULL) == !high_available);
    assert(!strstr(filtered, "/itag/300/"));
    char path[192], reply[8192];
    snprintf(path, sizeof(path), "/cache/%s/itag/%d/mediadata.m3u8",
             airplay_video_get_cache_id(video), high_available ? 200 : 100);
    assert(invoke_plain(&f, http_handler_hls, "GET", path, NULL, reply, sizeof(reply)) == 200);
    assert(strstr(reply, "generated-segment.ts"));
    play_request(&f);
    expect_request(NULL);
    assert(f.play_calls == 2);
    fixture_destroy(&f);
}

typedef struct {
    char text[4096];
    size_t length;
} lifecycle_log_t;

static void lifecycle_log(void *cls, int level, const char *message) {
    lifecycle_log_t *capture = cls;
    (void)level;
    assert(!strstr(message, "PRIVATE_TEARDOWN_FIXTURE"));
    if (!strstr(message, "RAOP RTP audio trace ")) return;
    /* logger_log serializes callbacks; reads below occur after the RTP join. */
    size_t length = strlen(message);
    assert(capture->length + length + 2 <= sizeof(capture->text));
    memcpy(capture->text + capture->length, message, length);
    capture->length += length;
    capture->text[capture->length++] = '\n';
    capture->text[capture->length] = '\0';
}

static plist_t audio_lifecycle_request(bool typed) {
    plist_t root = plist_new_dict();
    plist_dict_set_item(root, "privateMetadata", plist_new_string("PRIVATE_TEARDOWN_FIXTURE"));
    if (typed) {
        plist_t streams = plist_new_array();
        plist_t stream = plist_new_dict();
        plist_dict_set_item(stream, "type", plist_new_uint(96));
        plist_dict_set_item(stream, "ct", plist_new_uint(2));
        plist_dict_set_item(stream, "controlPort", plist_new_uint(0));
        plist_array_append_item(streams, stream);
        plist_dict_set_item(root, "streams", streams);
    }
    return root;
}

static void test_audio_teardown_lifecycle(void) {
    /* Exercise real RTP start/join/restart through production plist handlers.
     * No audio packets or timing traffic are sent; the deadline bounds joins. */
    alarm(10);
    fixture_t f;
    fixture_init(&f);
    lifecycle_log_t capture = {{0}, 0};
    logger_set_callback(f.raop.logger, lifecycle_log, &capture);
    httpd_callbacks_t http_callbacks = {0};
    f.raop.httpd = httpd_init(f.raop.logger, &http_callbacks, 0);
    assert(f.raop.httpd);
    timing_protocol_t timing = NTP;
    f.conn.raop_ntp = raop_ntp_init(f.raop.logger, &f.raop.callbacks, "127.0.0.1", 4, 0, &timing);
    assert(f.conn.raop_ntp);
    unsigned char aeskey[16] = {0}, aesiv[16] = {0};
    f.conn.raop_rtp = raop_rtp_init(f.raop.logger, &f.raop.callbacks, f.conn.raop_ntp,
                                     "127.0.0.1", 4, aeskey, aesiv);
    assert(f.conn.raop_rtp);
    raop_rtp_t *original_rtp = f.conn.raop_rtp;
    assert(invoke_request(&f, raop_handler_setup, "SETUP", "RTSP/1.0", "/fixture",
                          audio_lifecycle_request(true)) == 200);
    assert(raop_rtp_is_running(original_rtp));
    uint64_t generation = raop_rtp_get_trace_generation(original_rtp);
    assert(generation > 0);

    assert(invoke_request(&f, raop_handler_teardown, "TEARDOWN", "RTSP/1.0", "/fixture",
                          audio_lifecycle_request(true)) == 200);
    assert(f.conn.raop_rtp == original_rtp);
    assert(!raop_rtp_is_running(original_rtp));
    assert(raop_rtp_get_trace_generation(original_rtp) == generation);
    assert(strstr(capture.text, "event=teardown-request audio=1 video=0 rtp_present=1"));
    assert(strstr(capture.text, "event=stop-request reason=sender-teardown-audio"));
    assert(strstr(capture.text, "event=stopped "));
    memset(&capture, 0, sizeof(capture));

    /* A typed audio teardown retains the object for a later audio SETUP. */
    assert(invoke_request(&f, raop_handler_setup, "SETUP", "RTSP/1.0", "/fixture",
                          audio_lifecycle_request(true)) == 200);
    assert(f.conn.raop_rtp == original_rtp);
    assert(raop_rtp_is_running(original_rtp));
    assert(raop_rtp_get_trace_generation(original_rtp) > generation);
    assert(invoke_request(&f, raop_handler_teardown, "TEARDOWN", "RTSP/1.0", "/fixture",
                          audio_lifecycle_request(false)) == 200);
    assert(f.conn.raop_rtp == NULL);
    assert(strstr(capture.text, "event=teardown-request audio=0 video=0 rtp_present=1"));
    assert(strstr(capture.text, "event=stop-request reason=sender-teardown-session"));
    assert(strstr(capture.text, "event=stopped "));
    assert(!strstr(capture.text, "reason=sender-teardown-audio"));

    raop_ntp_destroy(f.conn.raop_ntp);
    f.conn.raop_ntp = NULL;
    httpd_destroy(f.raop.httpd);
    f.raop.httpd = NULL;
    fixture_destroy(&f);
    alarm(0);
    puts("RAOP teardown: typed audio stop/restart and full session destruction passed.");
}

static void test_server_info_preserves_legacy_http_features(void) {
    fixture_t f;
    fixture_init(&f);
    int error = 0;
    const char address[6] = {2, 0, 0, 0, 0, 1};
    f.raop.dnssd = dnssd_init("fixture", 7, address, sizeof(address), &error, 0);
    assert(f.raop.dnssd && error == DNSSD_ERROR_NOERROR);
    for (int i = 0; i < 3; i++) {
        if (i) {
            dnssd_set_airplay_features(f.raop.dnssd, 3, 0);
            dnssd_set_airplay_features(f.raop.dnssd, 42, i == 1);
        }
        http_response_t *response = http_response_create();
        http_response_init(response, "HTTP/1.1", 200, "OK");
        char *body = NULL;
        int length = 0;
        http_handler_server_info(&f.conn, NULL, response, &body, &length);
        assert(body && length > 0);
        plist_t root = NULL;
        plist_from_xml(body, length, &root);
        assert(PLIST_IS_DICT(root));
        plist_t value = plist_dict_get_item(root, "features");
        assert(PLIST_IS_UINT(value));
        uint64_t features = 0;
        plist_get_uint_val(value, &features);
        /* The modern RAOP mask makes iOS select HTTP /fp-setup, which this
         * receiver does not implement. Preserve its working legacy HLS path
         * even when RAOP/mirroring features include upper-word bits. */
        assert(features == UINT64_C(0x27F));
        assert(!(features & (UINT64_C(1) << 42)));
        char *model = NULL;
        plist_get_string_val(plist_dict_get_item(root, "model"), &model);
        assert(model && !strcmp(model, GLOBAL_MODEL));
        plist_mem_free(model);
        plist_free(root);
        free(body);
        http_response_destroy(response);
    }
    dnssd_destroy(f.raop.dnssd);
    fixture_destroy(&f);
}

static void assert_handshake_unchanged(fairplay_t *fp, const unsigned char handshake[164]) {
    assert(fp->keymsglen == 164);
    assert(!memcmp(fp->keymsg, handshake, 164));
}

static void test_fairplay_setup_bounds(void) {
    fairplay_t *fp = fairplay_init(NULL);
    assert(fp);
    unsigned char setup[16] = {0}, response[142];
    setup[4] = 3;
    /* Golden hashes lock all existing response bytes for each supported mode. */
    const uint64_t expected[] = {UINT64_C(0x3d8e4e02084b2887), UINT64_C(0xc7ba827810db9176),
                                 UINT64_C(0xe5b4faf447ecd35f), UINT64_C(0xf6928fb6c9c9cd28)};
    unsigned char handshake[164], handshake_response[32];
    for (unsigned int i = 0; i < sizeof(handshake); i++) handshake[i] = (unsigned char)i;
    handshake[4] = 3;
    for (unsigned int mode = 0; mode < 4; mode++) {
        assert(!fairplay_handshake(fp, handshake, handshake_response));
        setup[14] = mode;
        assert(!fairplay_setup(fp, setup, response));
        assert(fp->keymsglen == 0);
        uint64_t hash = UINT64_C(14695981039346656037);
        for (unsigned int i = 0; i < sizeof(response); i++)
            hash = (hash ^ response[i]) * UINT64_C(1099511628211);
        assert(hash == expected[mode]);
    }
    assert(!fairplay_handshake(fp, handshake, handshake_response));
    const unsigned char header[] = {'F', 'P', 'L', 'Y', 3, 1, 4, 0, 0, 0, 0, 20};
    assert(!memcmp(handshake_response, header, sizeof(header)));
    assert(!memcmp(handshake_response + sizeof(header), handshake + 144, 20));
    assert_handshake_unchanged(fp, handshake);
    for (unsigned int mode = 4; mode <= 255; mode++) {
        setup[14] = mode;
        memset(response, 0xa5, sizeof(response));
        assert(fairplay_setup(fp, setup, response) == -1);
        for (unsigned int i = 0; i < sizeof(response); i++) assert(response[i] == 0xa5);
        assert_handshake_unchanged(fp, handshake);
    }
    /* Unsupported versions still reject without overwriting output or state. */
    setup[14] = 0;
    setup[4] = 2;
    assert(fairplay_setup(fp, setup, response) == -1);
    for (unsigned int i = 0; i < sizeof(response); i++) assert(response[i] == 0xa5);
    unsigned char unsupported[164];
    memcpy(unsupported, handshake, sizeof(unsupported));
    unsupported[4] = 2;
    memset(handshake_response, 0xa5, sizeof(handshake_response));
    assert(fairplay_handshake(fp, unsupported, handshake_response) == -1);
    for (unsigned int i = 0; i < sizeof(handshake_response); i++) assert(handshake_response[i] == 0xa5);
    assert_handshake_unchanged(fp, handshake);
    fairplay_destroy(fp);
}

static void invoke_rejected_fairplay(fixture_t *f, raop_handler_t handler, const char *protocol,
                                     const char *path, const unsigned char *body, int length,
                                     int expected_status) {
    char header[256];
    int header_length = snprintf(header, sizeof(header),
        "POST %s %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n",
        path, protocol, length);
    assert(header_length > 0 && header_length < (int)sizeof(header));
    http_request_t *request = http_request_init();
    assert(!http_request_add_data(request, header, header_length));
    if (length) assert(!http_request_add_data(request, (const char *)body, length));
    assert(http_request_is_complete(request) && !http_request_has_error(request));
    http_response_t *response = http_response_create();
    http_response_init(response, protocol, 200, "OK");
    char *response_body = NULL;
    int response_length = 0;
    handler(&f->conn, request, response, &response_body, &response_length);
    assert(!response_body && response_length == 0);
    http_response_finish(response, response_body, response_length);
    const char *serialized = http_response_get_data(response, &response_length);
    int status = 0;
    assert(sscanf(serialized, "%*s %d", &status) == 1 && status == expected_status);
    http_response_destroy(response);
    http_request_destroy(request);
}

static void test_fairplay_request_lengths(void) {
    fixture_t f;
    fixture_init(&f);
    f.conn.fairplay = fairplay_init(f.raop.logger);
    assert(f.conn.fairplay);
    unsigned char body[165] = {0}, response[32];
    body[4] = 3;
    assert(!fairplay_handshake(f.conn.fairplay, body, response));
    const int malformed[] = {0, 1, 2, 3, 4, 5, 15, 17, 163, 165};
    for (unsigned int i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        /* Existing RTSP setup length validation must continue guarding its
         * fixed-size FairPlay calls before they can read request fields. */
        invoke_rejected_fairplay(&f, raop_handler_fpsetup, "RTSP/1.0", "/fp-setup",
                                body, malformed[i], 200);
        assert_handshake_unchanged(f.conn.fairplay, body);
    }
    const int unsupported_http[] = {0, 1, 2, 3, 4, 5, 16, 164};
    for (unsigned int i = 0; i < sizeof(unsupported_http) / sizeof(unsupported_http[0]); i++) {
        invoke_rejected_fairplay(&f, http_handler_fpsetup2, "HTTP/1.1", "/fp-setup2",
                                body, unsupported_http[i], 421);
        assert_handshake_unchanged(f.conn.fairplay, body);
    }
    body[14] = 255;
    invoke_rejected_fairplay(&f, raop_handler_fpsetup, "RTSP/1.0", "/fp-setup", body, 16, 200);
    body[14] = 0;
    assert_handshake_unchanged(f.conn.fairplay, body);
    assert(f.play_calls == 0 && f.reset_calls == 0 && f.raop.current_video == -1);
    expect_request(NULL);
    fairplay_destroy(f.conn.fairplay);
    fixture_destroy(&f);
}

static void test_scoped_cache_lifetime_and_loopback(void) {
    fixture_t f;
    fixture_init(&f);
    assert(raop_set_plist(&f.raop, "hls_scoped_cache", 1) == 0);
    unsigned char local[4] = {127, 0, 0, 1};
    f.conn.remote = local;
    f.conn.remotelen = sizeof(local);
    airplay_video_t *old = start_collection(&f);
    assert(f.request_calls == 1 && !f.request_direct_http);
    const char *old_id = airplay_video_get_cache_id(old);
    assert(old_id && strlen(old_id) == 32);
    char old_path[128], old_media_path[192], reply[8192];
    snprintf(old_path, sizeof(old_path), "/cache/%s/master.m3u8", old_id);
    snprintf(old_media_path, sizeof(old_media_path), "/cache/%s/itag/100/mediadata.m3u8", old_id);
    assert(strstr(get_master_playlist(old), old_id));
    assert(action_response(&f, media0, 2, 200, media) == 200);
    expect_request(media1);
    assert(action_response(&f, media1, 3, 200, media) == 200);
    expect_request(media2);
    assert(action_response(&f, media2, 4, 200, media) == 200);
    expect_request(NULL);
    assert(strstr(f.played_location, old_path));
    assert(invoke_plain(&f, http_handler_hls, "GET", old_path, NULL, reply, sizeof(reply)) == 200);
    assert(strstr(reply, old_id));
    assert(invoke_plain(&f, http_handler_hls, "GET", old_media_path, NULL, reply, sizeof(reply)) == 200);
    assert(strstr(reply, "generated-segment.ts"));
    play_request(&f); /* Cached resume preserves this cache's URL namespace. */
    expect_request(NULL);
    assert(strstr(f.played_location, old_path));
    int old_slot = f.raop.current_video;

    /* A different current_video must never change what the old URL returns. */
    int new_slot = (old_slot + 1) % MAX_AIRPLAY_VIDEO;
    airplay_video_t *replacement = airplay_video_init(&f.raop, 7000, "");
    assert(replacement && airplay_video_enable_scoped_cache(replacement));
    assert(strcmp(airplay_video_get_cache_id(replacement), old_id));
    store_master_playlist(replacement, strdup("#EXTM3U\n#replacement-cache\n"));
    f.raop.airplay_video[new_slot] = replacement;
    f.raop.current_video = new_slot;
    assert(invoke_plain(&f, http_handler_hls, "GET", old_path, NULL, reply, sizeof(reply)) == 200);
    assert(!strstr(reply, "replacement-cache"));
    assert(strstr(reply, old_id));
    assert(invoke_plain(&f, http_handler_hls, "GET", "/master.m3u8", NULL, reply, sizeof(reply)) == 404);
    assert(invoke_plain(&f, http_handler_hls, "GET", "/cache/../../master.m3u8", NULL, reply, sizeof(reply)) == 404);
    assert(invoke_plain(&f, http_handler_hls, "POST", old_path, NULL, reply, sizeof(reply)) == 405);
    unsigned char remote[4] = {192, 168, 1, 2};
    f.conn.remote = remote;
    assert(invoke_plain(&f, http_handler_hls, "GET", old_path, NULL, reply, sizeof(reply)) == 403);
    unsigned char local6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    f.conn.remote = local6;
    f.conn.remotelen = sizeof(local6);
    assert(invoke_plain(&f, http_handler_hls, "GET", old_path, NULL, reply, sizeof(reply)) == 200);
    raop_destroy_airplay_video(&f.raop, old_slot);
    assert(invoke_plain(&f, http_handler_hls, "GET", old_path, NULL, reply, sizeof(reply)) == 404);
    assert(!strstr(reply, "replacement-cache"));
    fixture_destroy(&f);
}

static void test_scoped_controls_reject_stale_session(void) {
    fixture_t f;
    fixture_init(&f);
    f.raop.hls_support = true;
    assert(raop_set_plist(&f.raop, "hls_scoped_cache", 1) == 0);
    f.expected_direct_http = true;
    plist_t root = plist_new_dict();
    plist_dict_set_item(root, "uuid", plist_new_string(playback_uuid));
    plist_dict_set_item(root, "Content-Location", plist_new_string("http://fixture.invalid/video.mp4"));
    assert(invoke(&f, http_handler_play, "/play", root) == 200);
    assert(f.request_calls == 1 && f.request_direct_http);
    const char stale[] = "99999999-2222-3333-4444-555555555555";
    const struct { raop_handler_t handler; const char *method; const char *path; } controls[] = {
        {http_handler_rate, "POST", "/rate?value=0"},
        {http_handler_scrub, "POST", "/scrub?position=2"},
        {http_handler_stop, "POST", "/stop"},
        {http_handler_playback_info, "GET", "/playback-info"}
    };
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        assert(invoke_plain(&f, controls[i].handler, controls[i].method, controls[i].path, stale, NULL, 0) == 409);
        assert(invoke_plain(&f, controls[i].handler, controls[i].method, controls[i].path, NULL, NULL, 0) == 409);
        assert(invoke_plain(&f, NULL, controls[i].method, controls[i].path, stale, NULL, 0) == 409);
        assert(invoke_plain(&f, controls[i].handler, controls[i].method, controls[i].path, session_id, NULL, 0) == 200);
    }
    assert(f.rate_calls == 1 && f.scrub_calls == 1 && f.stop_calls == 1 && f.info_calls == 1);
    assert(f.conn.connection_type == CONNECTION_TYPE_UNKNOWN);
    assert(f.request_error_calls == 0);
    assert(invoke_plain(&f, http_handler_action, "POST", "/action", stale, NULL, 0) == 409);
    assert(f.request_error_calls == 0);
    fixture_destroy(&f);
}

static void test_early_request_error_callback(void) {
    fixture_t f;
    fixture_init(&f);
    f.raop.hls_scoped_cache = true;
    play_request(&f);
    assert(f.request_calls == 1 && f.play_calls == 0 && f.request_error_calls == 0);
    expect_request(master_url);
    assert(action_response(&f, master_url, 1, 404, NULL) == 400);
    assert(f.request_error_calls == 1 && f.play_calls == 0);
    airplay_video_t *current = hls_get_current_video(&f.raop);
    plist_t invalid = plist_new_dict();
    plist_dict_set_item(invalid, "uuid", plist_new_string("bad-uuid"));
    plist_dict_set_item(invalid, "Content-Location", plist_new_string(master_url));
    assert(invoke(&f, http_handler_play, "/play", invalid) == 400);
    assert(f.request_calls == 1 && f.request_error_calls == 1);
    assert(hls_get_current_video(&f.raop) == current);
    fixture_destroy(&f);
}

static void test_scoped_fcup_ids_survive_replacement(void) {
    fixture_t f;
    fixture_init(&f);
    f.raop.hls_scoped_cache = true;
    airplay_video_t *old = start_collection(&f);
    assert(get_current_FCUP_RequestID(old) == 2);
    play_request(&f); /* Same URL/session, but the incomplete cache is replaced. */
    expect_request(master_url);
    airplay_video_t *active = hls_get_current_video(&f.raop);
    assert(get_current_FCUP_RequestID(active) == 3);
    assert(action_response(&f, master_url, 1, 404, NULL) == 200);
    expect_request(NULL);
    assert(f.request_error_calls == 0 && get_num_media_uri(active) == 0);
    plist_t root = plist_new_dict(), params = plist_new_dict();
    plist_dict_set_item(root, "type", plist_new_string("unhandledURLResponse"));
    plist_dict_set_item(params, "FCUP_Response_URL", plist_new_string(master_url));
    plist_dict_set_item(params, "FCUP_Response_StatusCode", plist_new_uint(404));
    plist_dict_set_item(root, "params", params); /* Missing correlation ID. */
    assert(invoke(&f, http_handler_action, "/action", root) == 200);
    expect_request(NULL);
    assert(f.request_error_calls == 0 && get_num_media_uri(active) == 0);
    assert(action_response(&f, master_url, 3, 200, master) == 200);
    expect_request(media0);
    assert(get_current_FCUP_RequestID(active) == 4);
    assert(action_response(&f, media0, 2, 404, NULL) == 200);
    expect_request(NULL);
    assert(get_next_media_uri_id(active) == 1 && f.request_error_calls == 0);
    fixture_destroy(&f);

    fixture_init(&f);
    f.raop.hls_scoped_cache = true;
    f.raop.scoped_fcup_request_id = INT_MAX;
    root = plist_new_dict();
    plist_dict_set_item(root, "uuid", plist_new_string(playback_uuid));
    plist_dict_set_item(root, "Content-Location", plist_new_string(master_url));
    assert(invoke(&f, http_handler_play, "/play", root) == 400);
    expect_request(NULL);
    assert(f.request_calls == 1 && f.request_error_calls == 1);
    fixture_destroy(&f);
}

static void test_scoped_reverse_socket_follows_session(void) {
    fixture_t f;
    fixture_init(&f);
    f.raop.hls_scoped_cache = true;
    int other_pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, other_pair) == 0);
    assert(fcntl(other_pair[1], F_SETFL, O_NONBLOCK) == 0);
    raop_conn_t other = {0};
    other.client_session_id = "99999999-2222-3333-4444-555555555555";
    other.reverse_registration_order = 2;
    test_reverse_count = 2;
    test_reverse_connections[0] = &other;
    test_reverse_sockets[0] = other_pair[0];
    test_reverse_connections[1] = &f.reverse_conn;
    test_reverse_sockets[1] = reverse_pair[0];
    assert(fcup_request(&f.conn, master_url, session_id, 1) == 0);
    expect_request(master_url);
    char received[8192];
    assert(recv(other_pair[1], received, sizeof(received), 0) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    /* Reconnecting with the same session chooses its newest registration,
     * regardless of where the HTTP server reused a connection-table slot. */
    other.client_session_id = (char *)session_id;
    assert(fcup_request(&f.conn, master_url, session_id, 2) == 0);
    expect_request(NULL);
    int length = recv(other_pair[1], received, sizeof(received) - 1, 0);
    assert(length > 0);
    received[length] = '\0';
    assert(strstr(received, master_url));
    assert(fcup_request(&f.conn, master_url, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 3) == -1);
    expect_request(NULL);
    close(other_pair[0]);
    close(other_pair[1]);
    fixture_destroy(&f);
}

int main(void) {
    test_scoped_cache_lifetime_and_loopback();
    test_scoped_controls_reject_stale_session();
    test_early_request_error_callback();
    test_scoped_fcup_ids_survive_replacement();
    test_scoped_reverse_socket_follows_session();
    test_fairplay_setup_bounds();
    test_fairplay_request_lengths();
    test_server_info_preserves_legacy_http_features();
    test_audio_teardown_lifecycle();
    test_direct_http_route_metadata();
    test_partial_cache_never_resumes();
    test_complete_cache_resumes_without_refetch();
    test_stale_responses_do_not_advance_collection();
    test_failed_media_variant_is_removed(404, "not found");
    test_failed_media_variant_is_removed(200, "not a playlist");
    test_failed_media_variant_is_removed(200, NULL);
    test_missing_or_malformed_master_is_rejected();
    test_all_media_variants_failed_is_rejected();
    test_duplicate_uri_requires_current_request_id();
    test_pi4_profile_reaches_download_finalizer(false, false);
    test_pi4_profile_reaches_download_finalizer(true, false);
    test_pi4_profile_reaches_download_finalizer(false, true);
    test_pi4_profile_reaches_download_finalizer(true, true);
    test_mpv_scoped_quality_selection(false);
    test_mpv_scoped_quality_selection(true);
    puts("HTTP HLS cache regression tests passed.");
    return 0;
}
