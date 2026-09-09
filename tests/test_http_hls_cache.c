/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the production binary-plist HTTP handlers and FCUP serialization.
 * Only the reverse-channel socket lookup is replaced; requests use a local
 * socketpair, so no network server, phone, or renderer is needed.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define httpd_get_connection_socket_by_type test_reverse_socket
#include "../lib/raop.c"
#undef httpd_get_connection_socket_by_type

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static int reverse_pair[2] = {-1, -1};
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
    unsigned int play_calls;
    unsigned int reset_calls;
    float played_position;
    char played_location[256];
} fixture_t;

int test_reverse_socket(httpd_t *httpd, connection_type_t type, int instance) {
    (void)httpd;
    assert(type == CONNECTION_TYPE_PTTH);
    assert(instance == 1);
    return reverse_pair[0];
}

static void test_log(void *cls, int level, const char *message) {
    (void)cls;
    (void)level;
    printf("%s\n", message);
}

static void played(void *cls, const char *location, const float position) {
    fixture_t *f = cls;
    f->play_calls++;
    f->played_position = position;
    snprintf(f->played_location, sizeof(f->played_location), "%s", location);
}

static void reset_connection(void *cls, int count) {
    (void)count;
    ((fixture_t *)cls)->reset_calls++;
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
    f->raop.callbacks.conn_reset = reset_connection;
    f->conn.raop = &f->raop;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, reverse_pair) == 0);
    assert(fcntl(reverse_pair[1], F_SETFL, O_NONBLOCK) == 0);
}

static void fixture_destroy(fixture_t *f) {
    raop_destroy_airplay_video(&f->raop, -1);
    logger_destroy(f->raop.logger);
    close(reverse_pair[0]);
    close(reverse_pair[1]);
    reverse_pair[0] = reverse_pair[1] = -1;
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

static int invoke(fixture_t *f, raop_handler_t handler, const char *path, plist_t root) {
    char *body = NULL;
    uint32_t body_length = 0;
    plist_to_bin(root, &body, &body_length);
    plist_free(root);
    assert(body && body_length > 0);
    char header[512];
    int header_length = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\nX-Apple-Session-ID: %s\r\n"
        "Content-Type: application/x-apple-binary-plist\r\nContent-Length: %u\r\n\r\n",
        path, session_id, body_length);
    assert(header_length > 0 && header_length < (int)sizeof(header));
    http_request_t *request = http_request_init();
    assert(http_request_add_data(request, header, header_length) == 0);
    assert(http_request_add_data(request, body, body_length) == 0);
    plist_mem_free(body);
    assert(http_request_is_complete(request));
    assert(!http_request_has_error(request));
    http_response_t *response = http_response_create();
    http_response_init(response, "HTTP/1.1", 200, "OK");
    char *response_body = NULL;
    int response_length = 0;
    handler(&f->conn, request, response, &response_body, &response_length);
    http_response_finish(response, response_body, response_length);
    const char *serialized = http_response_get_data(response, &response_length);
    int status = 0;
    assert(sscanf(serialized, "HTTP/1.1 %d", &status) == 1);
    free(response_body);
    http_response_destroy(response);
    http_request_destroy(request);
    return status;
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

static void test_pi4_profile_reaches_download_finalizer(bool enabled) {
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
    play_request(&f);
    expect_request(master_url);
    assert(action_response(&f, master_url, 1, 200, mixed_master) == 200);
    expect_request(media0);
    assert(action_response(&f, media0, 2, 200, media) == 200);
    expect_request(media1);
    assert(action_response(&f, media1, 3, 200, media) == 200);
    expect_request(media2);
    assert(action_response(&f, media2, 4, 200, media) == 200);
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

int main(void) {
    test_partial_cache_never_resumes();
    test_complete_cache_resumes_without_refetch();
    test_stale_responses_do_not_advance_collection();
    test_failed_media_variant_is_removed(404, "not found");
    test_failed_media_variant_is_removed(200, "not a playlist");
    test_failed_media_variant_is_removed(200, NULL);
    test_missing_or_malformed_master_is_rejected();
    test_all_media_variants_failed_is_rejected();
    test_duplicate_uri_requires_current_request_id();
    test_pi4_profile_reaches_download_finalizer(false);
    test_pi4_profile_reaches_download_finalizer(true);
    puts("HTTP HLS cache regression tests passed.");
    return 0;
}
