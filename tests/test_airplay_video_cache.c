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
    assert(strstr(airplay_video_get_cache_diagnostics(video), "missing_audio=1"));
    assert(strstr(airplay_video_get_cache_diagnostics(video), "group_absent=1"));
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
    assert(strstr(airplay_video_get_cache_diagnostics(video), "unsupported_codecs=1"));
    assert(strstr(airplay_video_get_cache_diagnostics(video), "retained=0"));
    assert(!strstr(airplay_video_get_cache_diagnostics(video), "://"));
    assert(!airplay_video_is_ready(video));
    assert(!get_master_playlist(video));
    airplay_video_destroy(video);
}

static void test_mpv_cache_selection(raop_t *owner) {
    const char *paths[] = { "/low.m3u8", "/high.m3u8", "/vp9.m3u8", "/low-audio.m3u8", "/high-audio.m3u8" };
    const char *master =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"low\",NAME=\"LC\",URI=\"http://localhost:34759/low-audio.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"high\",NAME=\"LC\",URI=\"http://localhost:34759/high-audio.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100,CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720,AUDIO=\"low\"\n"
        "http://localhost:34759/low.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200,CODECS=\"avc1.64002a,mp4a.40.2\",RESOLUTION=1920x1080,FRAME-RATE=60,AUDIO=\"high\"\n"
        "http://localhost:34759/high.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300,CODECS=\"vp09.00.40.08,mp4a.40.2\",RESOLUTION=1920x1080\n"
        "http://localhost:34759/vp9.m3u8\n";
    /* A failed video OR required audio download must fall back to the lower
     * playable variant, not leave mpv with an unusable high-quality route. */
    for (int missing = -1; missing <= 3; missing++) {
        airplay_video_t *video = cache_fixture(owner, master, paths, 5);
        assert(airplay_video_prepare_cache_profile(video));
        assert(get_num_media_uri(video) == 4);
        assert(get_next_media_uri_id(video) == 0);
        assert(!airplay_video_is_ready(video));
        assert(!strstr(get_master_playlist(video), "vp9"));
        for (int i = 0; i < 4; i++) if (i != missing) store_entry(video, i, media);
        assert(airplay_video_finalize_cache_mpv(video));
        assert(airplay_video_is_ready(video));
        assert(get_num_media_uri(video) == 2);
        const char *filtered = get_master_playlist(video);
        bool high = missing != 1 && missing != 3;
        assert((strstr(filtered, "/high.m3u8") != NULL) == high);
        assert((strstr(filtered, "/high-audio.m3u8") != NULL) == high);
        assert((strstr(filtered, "/low.m3u8") != NULL) == !high);
        assert((strstr(filtered, "/low-audio.m3u8") != NULL) == !high);
        int count;
        float duration;
        assert(get_media_playlist(video, &count, &duration, high ? paths[1] : paths[0]));
        assert(get_media_playlist(video, &count, &duration, high ? paths[4] : paths[3]));
        airplay_video_destroy(video);
    }
    /* Incomplete bandwidth metadata leaves quality choice with the player. */
    const char *bad[] = { "", "BANDWIDTH=0,", "BANDWIDTH=oops,", "BANDWIDTH=4294967296," };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char text[1024];
        snprintf(text, sizeof(text),
            "#EXTM3U\n#EXT-X-STREAM-INF:%sCODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720\n"
            "http://localhost:34759/low.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=200,CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720\n"
            "http://localhost:34759/high.m3u8\n", bad[i]);
        airplay_video_t *video = cache_fixture(owner, text, paths, 2);
        assert(airplay_video_prepare_cache_profile(video));
        store_entry(video, 0, media);
        store_entry(video, 1, media);
        assert(airplay_video_finalize_cache_mpv(video));
        assert(get_num_media_uri(video) == 2);
        airplay_video_destroy(video);
    }
}

static void test_external_rendition_groups(raop_t *owner) {
    static const struct {
        const char *uri;
        bool keep;
        const char *reason;
    } cases[] = {
        {"https://media.example.invalid/rendition.m3u8?token=PRIVATE%2Fquery&part=1", true, NULL},
        {"http://media.example.invalid/rendition.m3u8?token=PRIVATE", true, NULL},
        {"http://localhost:34759/missing.m3u8?token=PRIVATE", false, "group_local=1"},
        {"http://localhost:34759/cache/obsolete/rendition.m3u8?token=PRIVATE", false, "group_local=1"},
        {"http://LOCALHOST:34759/cache/obsolete/rendition.m3u8", false, "group_local=1"},
        {"http://127.0.0.1:34759/missing.m3u8", false, "group_local=1"},
        {"http://[::1]:34759/missing.m3u8", false, "group_local=1"},
        {"http://[::ffff:127.0.0.1]:34759/missing.m3u8", false, "group_local=1"},
        {"https://", false, "group_malformed=1"},
        {"http://?token=PRIVATE", false, "group_malformed=1"},
        {"https://media.example.invalid:70000/rendition.m3u8", false, "group_malformed=1"},
        {"https://media.example.invalid/rendition bad.m3u8", false, "group_malformed=1"},
        {"https://media.example.invalid/rendition\tbad.m3u8", false, "group_malformed=1"},
        {"airplay://media.example.invalid/missing/rendition.m3u8?token=PRIVATE", false, "group_other=1"},
        {"subtitles/rendition.m3u8?token=PRIVATE", false, "group_relative=1"}
    };
    static const char *types[] = {"AUDIO", "SUBTITLES"};
    const char *paths[] = {"/video.m3u8"};
    for (unsigned type = 0; type < sizeof(types) / sizeof(types[0]); type++) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            for (unsigned finalize_only = 0; finalize_only < 2; finalize_only++) {
                char master[2048];
                snprintf(master, sizeof(master),
                    "#EXTM3U\n"
                    "#EXT-X-MEDIA:TYPE=%s,GROUP-ID=\"PRIVATE-group\",NAME=\"PRIVATE-name\",LANGUAGE=\"en\",URI=\"%s\"\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=100,CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720,%s=\"PRIVATE-group\"\n"
                    "http://localhost:34759/video.m3u8\n", types[type], cases[i].uri, types[type]);
                airplay_video_t *video = cache_fixture(owner, master, paths, 1);
                bool result;
                if (finalize_only) {
                    store_entry(video, 0, media);
                    result = airplay_video_finalize_cache_mpv(video);
                } else result = airplay_video_prepare_cache_profile(video);
                assert(result == cases[i].keep);
                const char *diagnostics = airplay_video_get_cache_diagnostics(video);
                assert(!strstr(diagnostics, "PRIVATE") && !strstr(diagnostics, "://"));
                if (cases[i].keep) {
                    assert(strstr(get_master_playlist(video), cases[i].uri));
                    assert(get_num_media_uri(video) == 1);
                    if (!finalize_only) {
                        store_entry(video, 0, media);
                        assert(airplay_video_finalize_cache_mpv(video));
                    }
                    assert(airplay_video_is_ready(video));
                    assert(strstr(get_master_playlist(video), cases[i].uri));
                } else {
                    assert(strstr(diagnostics, cases[i].reason));
                    assert(strstr(diagnostics, type ? "missing_subtitles=1" : "missing_audio=1"));
                    assert(!airplay_video_is_ready(video));
                }
                airplay_video_destroy(video);
            }
        }
    }
}

static void test_uri_parsing_and_rewriting(void) {
    const char *prefix = "airplay://media.example.invalid/video";
    const char *master =
        "#EXTM3U\r\n#ignored airplay://media.example.invalid/video/comment.m3u8\r\n"
        "#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"airplay://media.example.invalid/video/key\"\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,URI=\"airplay://media.example.invalid/video/audio.m3u8?token=a,b\",GROUP-ID=\"audio\"\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100\r\nairplay://media.example.invalid/video/manifest?format=hls\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200\r\nairplay://media.example.invalid/video/high.m3u8?token=two";
    char **uris = NULL;
    int count = 0;
    assert(!create_media_uri_table(prefix, master, (int)strlen(master), &uris, &count));
    assert(count == 3);
    assert(!strcmp(uris[0], "airplay://media.example.invalid/video/audio.m3u8?token=a,b"));
    assert(!strcmp(uris[1], "airplay://media.example.invalid/video/manifest?format=hls"));
    assert(!strcmp(uris[2], "airplay://media.example.invalid/video/high.m3u8?token=two"));
    for (int i = 0; i < count; i++) free(uris[i]);
    free(uris);
    const char *locals[] = { "http://localhost:1", "http://localhost:1/cache/01234567890123456789012345678901" };
    for (size_t i = 0; i < sizeof(locals) / sizeof(locals[0]); i++) {
        char *rewritten = adjust_master_playlist((char *)master, (int)strlen(master), prefix, (char *)locals[i]);
        assert(rewritten && strstr(rewritten, "#ignored airplay://media.example.invalid/video/comment.m3u8"));
        assert(!strstr(rewritten, "\nairplay://media.example.invalid/video/manifest"));
        assert(strstr(rewritten, "/audio.m3u8?token=a,b"));
        assert(strstr(rewritten, "/manifest?format=hls"));
        free(rewritten);
    }
    char plain[] = "#EXTM3U\n";
    char *unchanged = adjust_master_playlist(plain, (int)strlen(plain), prefix, "http://localhost:1");
    assert(unchanged && !strcmp(unchanged, plain));
    free(unchanged);
    /* The length is authoritative; neither function may read the following
     * byte or depend on input having a C string terminator. */
    char *bounded = malloc(strlen(master));
    assert(bounded);
    memcpy(bounded, master, strlen(master));
    assert(!create_media_uri_table(prefix, bounded, (int)strlen(master), &uris, &count));
    for (int i = 0; i < count; i++) free(uris[i]);
    free(uris);
    free(bounded);
    assert(create_media_uri_table(prefix, plain, (int)strlen(plain), &uris, &count) != 0);
    assert(!uris && count == 0);
}

static void test_uri_rewrite_preserves_query_and_metadata(void) {
    const char *prefix = "airplay://media.example.invalid/video";
    const char *master =
        "#EXTM3U\r\n"
        "#comment URI=\"airplay://media.example.invalid/video/comment\"\r\n"
        "#EXT-X-SESSION-DATA:DATA-ID=\"airplay://media.example.invalid/video/id\",VALUE=\"airplay://media.example.invalid/video/value\"\r\n"
        "#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"airplay://media.example.invalid/video/key?return=airplay://media.example.invalid/video/private\"\r\n"
        "#EXT-X-CONTENT-STEERING:SERVER-URI=\"airplay://media.example.invalid/video/steering\"\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,URI=\"https://public.example.invalid/audio.m3u8?token=PRIVATE&return=airplay://media.example.invalid/video/callback\"\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100\r\n"
        "airplay://media.example.invalid/video/playlist?return=airplay://media.example.invalid/video/private";
    const char *expected =
        "#EXTM3U\r\n"
        "#comment URI=\"airplay://media.example.invalid/video/comment\"\r\n"
        "#EXT-X-SESSION-DATA:DATA-ID=\"airplay://media.example.invalid/video/id\",VALUE=\"airplay://media.example.invalid/video/value\"\r\n"
        "#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"http://localhost:1/key?return=airplay://media.example.invalid/video/private\"\r\n"
        "#EXT-X-CONTENT-STEERING:SERVER-URI=\"http://localhost:1/steering\"\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,URI=\"https://public.example.invalid/audio.m3u8?token=PRIVATE&return=airplay://media.example.invalid/video/callback\"\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100\r\n"
        "http://localhost:1/playlist?return=airplay://media.example.invalid/video/private";
    /* The helper must obey datalen without requiring a terminating byte. */
    size_t length = strlen(master);
    char *input = malloc(length);
    assert(input);
    memcpy(input, master, length);
    char *rewritten = adjust_master_playlist(input, (int)length, prefix, "http://localhost:1");
    assert(rewritten && !strcmp(rewritten, expected));
    free(rewritten);
    free(input);
}

static void test_language_selection(raop_t *owner) {
    const char *master =
        "#EXTM3U\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,URI=\"/en.m3u8\",LANGUAGE=\"en-US\",NAME=\"English\",GROUP-ID=\"aac\",DEFAULT=YES,YT-EXT-AUDIO-CONTENT-ID=\"en\"\r\n"
        "# comment separating valid audio renditions\r\n"
        "#EXT-X-MEDIA:GROUP-ID=\"aac\",NAME=\"Chinese\",LANGUAGE=\"zh-Hant-TW\",URI=\"/zh.m3u8\",TYPE=AUDIO\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"other\",NAME=\"English\",LANGUAGE=\"en-US\",URI=\"/fallback.m3u8\"\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO=\"aac\"\r\n/video.m3u8";
    airplay_video_t *video = airplay_video_init(owner, 34759, "zh:en");
    char *filtered = select_master_playlist_language(video, copy_string(master));
    assert(filtered && !strstr(filtered, "/en.m3u8"));
    assert(strstr(filtered, "/zh.m3u8"));
    assert(strstr(filtered, "/fallback.m3u8"));
    assert(strstr(filtered, "# comment separating valid audio renditions"));
    assert(strstr(filtered, "#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO=\"aac\"\r\n/video.m3u8"));
    assert(!strcmp(get_language_code(video), "zh-Hant-TW"));
    free(filtered);
    airplay_video_destroy(video);
    /* A single language, no DEFAULT and a long BCP 47 tag are legal. Also
     * tolerate an omitted final newline. The old parser could divide by
     * zero or overrun code[6]. */
    const char *single = "#EXTM3U\n#EXT-X-MEDIA:URI=\"/audio.m3u8\",TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Chinese\",LANGUAGE=\"zh-Hant-TW\",YT-EXT-AUDIO-CONTENT-ID=\"zh\"";
    video = airplay_video_init(owner, 34759, NULL);
    filtered = select_master_playlist_language(video, copy_string(single));
    assert(filtered && !strcmp(filtered, single));
    free(filtered);
    /* Malformed metadata must not crash or consume later playlist lines. */
    const char *malformed = "#EXTM3U\n#EXT-X-MEDIA:URI=\"/audio.m3u8\",LANGUAGE=\"unfinished\n#EXT-X-STREAM-INF:BANDWIDTH=100\n/video.m3u8\n";
    filtered = select_master_playlist_language(video, copy_string(malformed));
    assert(filtered && !strcmp(filtered, malformed));
    free(filtered);
    airplay_video_destroy(video);
}

static void test_condensed_playlists(void) {
    const char *ordinary[] = { media, "#EXTM3U\r\n#EXTINF:6.0,\r\nhttps://media.example.invalid/s.ts\r\n", "#EXTM3U" };
    for (size_t i = 0; i < sizeof(ordinary) / sizeof(ordinary[0]); i++) {
        char *copy = adjust_yt_condensed_playlist(ordinary[i]);
        assert(copy && !strcmp(copy, ordinary[i]));
        free(copy);
    }
    const char *condensed =
        "#EXTM3U\r\n#YT-EXT-CONDENSED-URL:PREFIX=\"yt:seg/\",PARAMS=\"sq,dur\",BASE-URI=\"https://media.example.invalid/v\"\r\n"
        "#EXTINF:6,\r\nyt:seg/0/6\r\n#EXT-X-DISCONTINUITY\r\n#EXTINF:5,\r\nyt:seg/1/5";
    char *expanded = adjust_yt_condensed_playlist(condensed);
    assert(expanded && strstr(expanded, "https://media.example.invalid/v/sq/0/dur/6\r\n#EXT-X-DISCONTINUITY"));
    assert(strstr(expanded, "https://media.example.invalid/v/sq/1/dur/5"));
    free(expanded);
    const char *empty = "#EXTM3U\n#YT-EXT-CONDENSED-URL:BASE-URI=\"https://media.example.invalid/v\",PARAMS=\"\",PREFIX=\"yt:\"\n#EXTINF:6,\nyt:/segment\n#EXT-X-ENDLIST\n";
    expanded = adjust_yt_condensed_playlist(empty);
    assert(expanded && strstr(expanded, "https://media.example.invalid/v/segment\n#EXT-X-ENDLIST"));
    free(expanded);
    const char *invalid[] = {
        "not HLS", "#EXTM3U\n#YT-EXT-CONDENSED-URL:BASE-URI=\"missing-fields\"\n#EXTINF:6,\nyt:0\n",
        "#EXTM3U\n#YT-EXT-CONDENSED-URL:BASE-URI=\"https://example.invalid\",PARAMS=\"sq,dur\",PREFIX=\"yt:\"\n#EXTINF:6,\nyt:0",
        "#EXTM3U\n#YT-EXT-CONDENSED-URL:BASE-URI=\"https://example.invalid\",PARAMS=\"sq\",PREFIX=\"yt:\"\n#EXTINF:6,\nwrong-prefix:0\n"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) assert(!adjust_yt_condensed_playlist(invalid[i]));
    assert(!adjust_yt_condensed_playlist(NULL));
}

static void test_repeated_cache_destruction(raop_t *owner) {
    airplay_video_t *video = new_cached_video(owner, 1);
    store_entry(video, 0, media);
    destroy_media_data_store(video);
    assert(!airplay_video_is_ready(video));
    destroy_media_data_store(video);
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
    test_mpv_cache_selection(owner);
    test_external_rendition_groups(owner);
    test_uri_parsing_and_rewriting();
    test_uri_rewrite_preserves_query_and_metadata();
    test_language_selection(owner);
    test_condensed_playlists();
    test_repeated_cache_destruction(owner);
    free(owner);
    puts("AirPlay video cache tests passed");
    return 0;
}
