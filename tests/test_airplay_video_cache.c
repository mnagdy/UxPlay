/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/raop.h"
#include "../lib/airplay_video.h"

static const char media[] =
    "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n"
    "#EXTINF:6.0,\nhttps://media.example.invalid/segment.ts\n#EXT-X-ENDLIST\n";

static char *copy_string(const char *s) {
    char *copy = malloc(strlen(s) + 1);
    assert(copy);
    strcpy(copy, s);
    return copy;
}

static void set_location(airplay_video_t *video, const char *location) {
    set_playback_location(video, location, strlen(location));
}

static airplay_video_t *new_cached_video(raop_t *owner, int entries) {
    airplay_video_t *video = airplay_video_init(owner, 34759, NULL);
    assert(video);
    char location[128];
    snprintf(location, sizeof(location), "%s/master.m3u8", get_uri_local_prefix(video));
    set_location(video, location);
    /* The local location exists before the FCUP master response arrives. */
    assert(!airplay_video_is_ready(video));
    const char *prefix = "airplay://media.example.invalid/video";
    set_uri_prefix(video, prefix, strlen(prefix));
    assert(!airplay_video_is_ready(video));
    if (!entries) return video;

    char **uris = calloc((size_t) entries, sizeof(char *));
    assert(uris);
    char master[8192] = "#EXTM3U\n";
    size_t used = strlen(master);
    for (int i = 0; i < entries; i++) {
        char uri[160];
        /* Entry 4 aliases entry 3, as in the interrupted Pi cache. */
        snprintf(uri, sizeof(uri), "%s/itag/%d/mediadata.m3u8", prefix, i == 4 ? 3 : i);
        uris[i] = copy_string(uri);
        int written = snprintf(master + used, sizeof(master) - used,
                               "#EXT-X-STREAM-INF:BANDWIDTH=100000\n%s\n", uri);
        assert(written > 0 && (size_t) written < sizeof(master) - used);
        used += (size_t) written;
    }
    store_master_playlist(video, copy_string(master));
    create_media_data_store(video, uris, entries);
    free(uris); /* The store owns the individual URI strings. */
    assert(!airplay_video_is_ready(video));
    return video;
}

static void store_entry(airplay_video_t *video, int index, const char *body) {
    char *playlist = copy_string(body);
    float duration;
    bool endlist;
    int count = analyze_media_playlist(playlist, &duration, &endlist);
    int result = store_media_playlist(video, playlist, &count, &duration, &endlist, index);
    assert(result == (index == 4 ? 1 : 0));
}

static void test_interrupted_and_complete_cache(raop_t *owner) {
    airplay_video_t *video = new_cached_video(owner, 18);
    for (int i = 0; i < 5; i++) store_entry(video, i, media);
    set_next_media_uri_id(video, 6);
    /* Regression: the Pi had 18 advertised entries, cursor 6, only 0..4 ready. */
    assert(!airplay_video_is_ready(video));
    set_next_media_uri_id(video, 18);
    assert(!airplay_video_is_ready(video)); /* A cursor is not proof of storage. */
    for (int i = 5; i < 18; i++) store_entry(video, i, media);
    assert(airplay_video_is_ready(video)); /* NULL alias slot 4 resolves to 3. */
    set_next_media_uri_id(video, 0);
    assert(airplay_video_is_ready(video));
    airplay_video_destroy(video);
}

static void test_missing_or_invalid_cache(raop_t *owner) {
    assert(!airplay_video_is_ready(NULL));
    airplay_video_t *video = airplay_video_init(owner, 34759, NULL);
    assert(!airplay_video_is_ready(video));
    airplay_video_destroy(video);
    video = new_cached_video(owner, 0);
    assert(!airplay_video_is_ready(video));
    store_master_playlist(video, copy_string("#EXTM3U\n"));
    assert(!airplay_video_is_ready(video)); /* A master without media is incomplete. */
    airplay_video_destroy(video);
    video = new_cached_video(owner, 1);
    store_entry(video, 0, "upstream download failed");
    assert(!airplay_video_is_ready(video));
    airplay_video_destroy(video);
    video = new_cached_video(owner, 1);
    store_entry(video, 0, media);
    store_master_playlist(video, copy_string("not a master playlist"));
    assert(!airplay_video_is_ready(video));
    airplay_video_destroy(video);
}

static void test_direct_urls(raop_t *owner) {
    airplay_video_t *video = airplay_video_init(owner, 34759, NULL);
    const char *urls[] = {
        "http://192.0.2.1:8000/stream.m3u8",
        "https://media.example.invalid/master.m3u8",
        "https://media.example.invalid/movie.mp4",
        "http://localhost:8000/master.m3u8"
    };
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        set_location(video, urls[i]);
        assert(airplay_video_is_ready(video));
        assert(airplay_video_finalize_cache(video));
        assert(airplay_video_finalize_cache_profile(video, true));
    }
    set_location(video, "http://localhost:34759/master.m3u8?session=test");
    assert(!airplay_video_is_ready(video));
    set_location(video, "airplay://media.example.invalid/master.m3u8");
    assert(!airplay_video_is_ready(video));
    airplay_video_destroy(video);
}

static airplay_video_t *cache_fixture(raop_t *owner, const char *master,
                                    const char **paths, int count) {
    airplay_video_t *video = new_cached_video(owner, 0);
    store_master_playlist(video, copy_string(master));
    char **uris = calloc((size_t) count, sizeof(char *));
    assert(uris);
    for (int i = 0; i < count; i++) {
        char uri[256];
        snprintf(uri, sizeof(uri), "%s%s", get_uri_prefix(video), paths[i]);
        uris[i] = copy_string(uri);
    }
    create_media_data_store(video, uris, count);
    free(uris);
    return video;
}

static void test_unavailable_quality(raop_t *owner) {
    const char *paths[] = { "/low.m3u8", "/medium.m3u8", "/high.m3u8" };
    const char *master =
        "#EXTM3U\r\n#EXT-X-VERSION:7\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100,CODECS=\"avc1.640028,mp4a.40.2\"\r\n"
        "http://localhost:34759/low.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200\r\nhttp://localhost:34759/medium.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300\r\nhttp://localhost:34759/high.m3u8\r\n"
        "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=200,URI=\"http://localhost:34759/medium.m3u8\"\r\n";
    airplay_video_t *video = cache_fixture(owner, master, paths, 3);
    store_entry(video, 0, media);
    store_entry(video, 2, media);
    assert(!airplay_video_is_ready(video));
    assert(airplay_video_finalize_cache(video));
    assert(airplay_video_is_ready(video));
    assert(get_num_media_uri(video) == 2);
    const char *filtered = get_master_playlist(video);
    assert(strstr(filtered, "#EXT-X-VERSION:7\n"));
    assert(strstr(filtered, "CODECS=\"avc1.640028,mp4a.40.2\""));
    assert(!strstr(filtered, "medium.m3u8"));
    assert(!strstr(filtered, "BANDWIDTH=200"));
    int chunks = 0;
    float duration = 0;
    assert(get_media_playlist(video, &chunks, &duration, "/high.m3u8"));
    assert(!get_media_playlist(video, &chunks, &duration, "/medium.m3u8"));
    assert(!get_media_playlist(video, &chunks, &duration, "high.m3u"));
    assert(airplay_video_finalize_cache(video)); /* Idempotent after compaction. */
    airplay_video_destroy(video);
}

static void test_missing_audio_group(raop_t *owner) {
    const char *paths[] = { "/audio-bad.m3u8", "/audio-good.m3u8", "/low.m3u8", "/high.m3u8" };
    const char *master =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:URI=\"http://localhost:34759/audio-bad.m3u8\",TYPE=AUDIO,GROUP-ID=\"missing\",NAME=\"bad\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"available\",NAME=\"good\",URI=\"http://localhost:34759/audio-good.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO=\"missing\"\nhttp://localhost:34759/low.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200,CODECS=\"avc1.640028,mp4a.40.2\",AUDIO=\"available\"\nhttp://localhost:34759/high.m3u8\n";
    airplay_video_t *video = cache_fixture(owner, master, paths, 4);
    for (int i = 1; i < 4; i++) store_entry(video, i, media);
    assert(airplay_video_finalize_cache(video));
    assert(airplay_video_is_ready(video));
    assert(!strstr(get_master_playlist(video), "audio-bad"));
    assert(!strstr(get_master_playlist(video), "low.m3u8"));
    assert(strstr(get_master_playlist(video), "audio-good"));
    assert(strstr(get_master_playlist(video), "high.m3u8"));
    assert(get_num_media_uri(video) == 2);
    airplay_video_destroy(video);
}

static void test_no_playable_variants(raop_t *owner) {
    const char *paths[] = { "/video.m3u8" };
    const char *master = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100\nhttp://localhost:34759/video.m3u8\n";
    airplay_video_t *video = cache_fixture(owner, master, paths, 1);
    assert(!airplay_video_finalize_cache(video));
    assert(!airplay_video_is_ready(video));
    assert(!get_master_playlist(video));
    airplay_video_destroy(video);
    master = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO=\"unknown\"\nhttp://localhost:34759/video.m3u8\n";
    video = cache_fixture(owner, master, paths, 1);
    store_entry(video, 0, media);
    assert(airplay_video_is_ready(video)); /* Bodies exist, topology is invalid. */
    assert(!airplay_video_finalize_cache(video));
    assert(!airplay_video_is_ready(video));
    assert(!get_master_playlist(video));
    airplay_video_destroy(video);
    master = "#EXTM3U\n#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"http://localhost:34759/missing-key\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=100\nhttp://localhost:34759/video.m3u8\n";
    video = cache_fixture(owner, master, paths, 1);
    store_entry(video, 0, media);
    assert(!airplay_video_finalize_cache(video));
    assert(!airplay_video_is_ready(video));
    assert(!get_master_playlist(video));
    airplay_video_destroy(video);
}

static void test_duplicate_after_failed_copy(raop_t *owner) {
    const char *paths[] = { "/video.m3u8", "/video.m3u8" };
    const char *master = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100\nhttp://localhost:34759/video.m3u8\n";
    airplay_video_t *video = cache_fixture(owner, master, paths, 2);
    store_entry(video, 1, media); /* First request failed; the repeat succeeded. */
    assert(!airplay_video_is_ready(video));
    assert(airplay_video_finalize_cache(video));
    assert(airplay_video_is_ready(video));
    assert(get_num_media_uri(video) == 1);
    int chunks = 0;
    float duration = 0;
    assert(get_media_playlist(video, &chunks, &duration, "/video.m3u8"));
    airplay_video_destroy(video);
}

static void test_pi4_profile(raop_t *owner) {
    static const struct {
        const char *attributes;
        bool keep;
    } variants[] = {
        { "CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=25,AUDIO=\"lc\"", true },
        { "CODECS=\"avc3.64002A,mp4a.40.2\",RESOLUTION=1920x1080,FRAME-RATE=60,AUDIO=\"lc\"", true },
        { "CODECS=\"vp09.00.40.08,mp4a.40.2\",RESOLUTION=1920x1080,FRAME-RATE=30,AUDIO=\"lc\"", false },
        { "CODECS=\"hvc1.1.6.L120.B0,mp4a.40.2\",RESOLUTION=1920x1080,AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.640032,mp4a.40.2\",RESOLUTION=3840x2160,AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.64002a,mp4a.40.2\",RESOLUTION=1920x1080,FRAME-RATE=60.000000000000001,AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.4d401f,mp4a.40.5\",RESOLUTION=1280x720,AUDIO=\"he\"", false },
        { "CODECS=\"avc1.4d401f,mp4a.40.2\",AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720,AUDIO=\"lc\"", true },
        { "CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=0x1080,AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=-1,AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.4d401f,mp4a.40.20\",RESOLUTION=1280x720,AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=NaN,AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.invalid,mp4a.40.2\",RESOLUTION=1280x720,AUDIO=\"lc\"", false },
        { "RESOLUTION=1280x720,AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=59.94,AUDIO=\"lc\"", true },
        { "CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=61,AUDIO=\"lc\"", false },
        { "CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=\"unfinished", false }
    };
    enum { count = sizeof(variants) / sizeof(variants[0]), entries = count + 2 };
    char master[8192] =
        "#EXTM3U\n#EXT-X-VERSION:7\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"lc\",NAME=\"LC\",URI=\"http://localhost:34759/audio-lc.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"he\",NAME=\"HE\",URI=\"http://localhost:34759/audio-he.m3u8\"\n";
    char paths[entries][48];
    const char *path_pointers[entries];
    size_t used = strlen(master);
    for (int i = 0; i < count; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/variant-%d.m3u8", i);
        path_pointers[i] = paths[i];
        int written = snprintf(master + used, sizeof(master) - used,
                               "#EXT-X-STREAM-INF:BANDWIDTH=%d,%s\nhttp://localhost:34759%s\n",
                               1000 + i, variants[i].attributes, paths[i]);
        assert(written > 0 && (size_t) written < sizeof(master) - used);
        used += (size_t) written;
    }
    strcpy(paths[count], "/audio-lc.m3u8");
    strcpy(paths[count + 1], "/audio-he.m3u8");
    path_pointers[count] = paths[count];
    path_pointers[count + 1] = paths[count + 1];
    for (int pi4 = 0; pi4 <= 1; pi4++) {
        airplay_video_t *video = cache_fixture(owner, master, path_pointers, entries);
        for (int i = 0; i < entries; i++) {
            char *body = copy_string(media);
            int chunks = 1;
            float duration = 6;
            bool endlist = true;
            assert(store_media_playlist(video, body, &chunks, &duration, &endlist, i) == 0);
        }
        assert(pi4 ? airplay_video_finalize_cache_profile(video, true) : airplay_video_finalize_cache(video));
        assert(airplay_video_is_ready(video));
        const char *filtered = get_master_playlist(video);
        assert(strstr(filtered, "#EXT-X-VERSION:7"));
        for (int i = 0; i < count; i++) {
            assert((strstr(filtered, paths[i]) != NULL) == (!pi4 || variants[i].keep));
        }
        assert(strstr(filtered, "/audio-lc.m3u8"));
        assert((strstr(filtered, "/audio-he.m3u8") != NULL) == !pi4);
        assert(get_num_media_uri(video) == (pi4 ? 5 : entries));
        airplay_video_destroy(video);
    }

    const char *unsupported[] = { "/vp9.m3u8" };
    airplay_video_t *video = cache_fixture(owner,
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100,CODECS=\"vp09.00.40.08,mp4a.40.2\",RESOLUTION=1280x720\n"
        "http://localhost:34759/vp9.m3u8\n", unsupported, 1);
    store_entry(video, 0, media);
    assert(!airplay_video_finalize_cache_profile(video, true));
    assert(!airplay_video_is_ready(video));
    assert(!get_master_playlist(video));
    airplay_video_destroy(video);
}

int main(void) {
    /* The cache keeps an opaque owner pointer; these API operations never
     * access server state, so no listener or network service is required. */
    raop_t *owner = (raop_t *) calloc(1, 1);
    assert(owner);
    test_interrupted_and_complete_cache(owner);
    test_missing_or_invalid_cache(owner);
    test_direct_urls(owner);
    test_unavailable_quality(owner);
    test_missing_audio_group(owner);
    test_no_playable_variants(owner);
    test_duplicate_after_failed_copy(owner);
    test_pi4_profile(owner);
    free(owner);
    puts("AirPlay video cache tests passed");
    return 0;
}
