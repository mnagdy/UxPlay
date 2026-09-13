/**
 * Copyright (c) 2024 fduncanh
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 */

// it should only start and stop the media_data_store that handles all HLS transactions, without
// otherwise participating in them.  

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>

#include "raop.h"
#include "airplay_video.h"
#include "crypto.h"
#include "compat.h"

typedef enum playlist_type_e {
    NONE,
    VOD,
    EVENT
} playlist_type_t;

struct media_item_s {
    char *uri;
    char *playlist;
    int num;
    int count;
    float duration;
    bool endlist;
    playlist_type_t playlist_type;
    int hls_version;
    int media_sequence;
};

struct airplay_video_s {
    raop_t *raop;
    char *apple_session_id;
    char *playback_uuid;
    char *uri_prefix;
    char *local_uri_prefix;
    char cache_id[33];
    char *playback_location;
    char *language_name;
    char *language_code;
    const char *lang;
    int next_uri;
    int FCUP_RequestID;
    bool cancelled;
    char cache_diagnostics[768];
    float start_position_seconds;
    float resume_position_seconds;
    playback_info_t *playback_info;
    char *master_playlist;
    media_item_t *media_data_store;
    int num_uri;
};

//  initialize airplay_video service.
airplay_video_t *airplay_video_init(raop_t *raop, unsigned short http_port, const char *lang) {
    char uri[] = "http://localhost:";
    char port[6] = { '\0' };
    assert(raop);

    /* calloc guarantees that the 36-character strings apple_session_id and 
       playback_uuid are null-terminated */
    airplay_video_t *airplay_video =  (airplay_video_t *) calloc(1, sizeof(airplay_video_t));

    if (!airplay_video) {
        return NULL;
    }

    airplay_video->lang = lang;
     /* create local_uri_prefix string */
    snprintf(port, sizeof(port), "%u", http_port);
    size_t len = strlen(uri) + strlen(port);
    airplay_video->local_uri_prefix = (char *) calloc (len + 1, sizeof(char));
    strcat(airplay_video->local_uri_prefix, uri);
    strcat(airplay_video->local_uri_prefix, port);

    airplay_video->raop = raop;
    airplay_video->FCUP_RequestID = 0;
    airplay_video->apple_session_id = NULL;
    airplay_video->start_position_seconds = 0.0f;
    airplay_video->playback_uuid = NULL;
    airplay_video->uri_prefix = NULL;
    airplay_video->playback_location = NULL;
    airplay_video->language_code = NULL;
    airplay_video->language_name = NULL;
    airplay_video->media_data_store = NULL;
    airplay_video->master_playlist = NULL;
    airplay_video->num_uri = 0;
    airplay_video->next_uri = 0;
    return airplay_video;
}

bool airplay_video_enable_scoped_cache(airplay_video_t *video) {
    if (!video || !video->local_uri_prefix || video->playback_location || video->master_playlist) return false;
    if (video->cache_id[0]) return true;
    unsigned char random[16];
    if (get_random_bytes(random, sizeof(random)) != 1) return false;
    char id[33];
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); i++) {
        id[2 * i] = hex[random[i] >> 4];
        id[2 * i + 1] = hex[random[i] & 15];
    }
    id[32] = '\0';
    size_t length = strlen(video->local_uri_prefix) + strlen("/cache/") + strlen(id) + 1;
    char *prefix = malloc(length);
    if (!prefix) return false;
    snprintf(prefix, length, "%s/cache/%s", video->local_uri_prefix, id);
    free(video->local_uri_prefix);
    video->local_uri_prefix = prefix;
    memcpy(video->cache_id, id, sizeof(id));
    return true;
}

const char *airplay_video_get_cache_id(const airplay_video_t *video) {
    return video && video->cache_id[0] ? video->cache_id : NULL;
}

// destroy the airplay_video service
void
airplay_video_destroy(airplay_video_t *airplay_video) {
    if (airplay_video->apple_session_id) {
        free(airplay_video->apple_session_id);
    }
    if (airplay_video->playback_uuid) {
        free(airplay_video->playback_uuid);
    }
    if (airplay_video->uri_prefix) {
        free(airplay_video->uri_prefix);
    }
    if (airplay_video->local_uri_prefix) {
        free(airplay_video->local_uri_prefix);
    }
    if (airplay_video->playback_location) {
        free(airplay_video->playback_location);
    }
    if (airplay_video->language_name) {
        free(airplay_video->language_name);
    }
    if (airplay_video->language_code) {
       free(airplay_video->language_code);
    }
    if (airplay_video->media_data_store) {
        destroy_media_data_store(airplay_video);
    }
    if (airplay_video->master_playlist){
        free (airplay_video->master_playlist);
    }
    free (airplay_video);
    airplay_video = NULL;
}

void set_apple_session_id(airplay_video_t *airplay_video, const char * apple_session_id, size_t len) {
    assert(apple_session_id && len == 36);
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, apple_session_id, len);
    if (airplay_video->apple_session_id) {
        free(airplay_video->apple_session_id);
    }
    airplay_video->apple_session_id = str;
    str = NULL;
}

void set_playback_uuid(airplay_video_t *airplay_video, const char *playback_uuid, size_t len) {
    assert(playback_uuid && len == 36);
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, playback_uuid, len);
    if (airplay_video->playback_uuid) {
        free(airplay_video->playback_uuid);
    }
    airplay_video->playback_uuid = str;
    str = NULL;
}

void set_uri_prefix(airplay_video_t *airplay_video, const char *uri_prefix, size_t len) {
    assert(uri_prefix && len );
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, uri_prefix, len);
    if (airplay_video->uri_prefix) {
        free(airplay_video->uri_prefix);
    }
    airplay_video->uri_prefix = str;
    str = NULL;
}

void set_playback_location(airplay_video_t *airplay_video, const char *location, size_t len) {
    assert(location && len );
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, location, len);
    if (airplay_video->playback_location) {
        free(airplay_video->playback_location);
    }
    airplay_video->playback_location = str;
    str = NULL;
}

void set_language_name(airplay_video_t *airplay_video, const char *language_name, size_t len) {
    assert(language_name && len );
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, language_name, len);
    if (airplay_video->language_name) {
        free(airplay_video->language_name);
    }
    airplay_video->language_name = str;
    str = NULL;
}

void set_language_code(airplay_video_t *airplay_video, const char *language_code, size_t len) {
    assert(language_code && len );
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, language_code, len);
    if (airplay_video->language_code) {
        free(airplay_video->language_code);
    }
    airplay_video->language_code = str;
    str = NULL;
}


const char *get_apple_session_id(airplay_video_t *airplay_video) {
    if (!airplay_video || !airplay_video->apple_session_id) {
        return NULL;
    }
    return airplay_video->apple_session_id;
}

float get_duration(airplay_video_t *airplay_video) {
    if (!airplay_video || !airplay_video->media_data_store || !airplay_video->media_data_store->duration) {
        return 0.0f;
    }
    return airplay_video->media_data_store->duration;
}

float get_start_position_seconds(airplay_video_t *airplay_video) {
    return airplay_video->start_position_seconds;
}

float get_resume_position_seconds(airplay_video_t *airplay_video) {
    return airplay_video->resume_position_seconds;
}

void set_start_position_seconds(airplay_video_t *airplay_video, float start_position_seconds) {
    airplay_video->start_position_seconds = start_position_seconds;
}

void set_resume_position_seconds(airplay_video_t *airplay_video, float resume_position_seconds) {
    airplay_video->resume_position_seconds = resume_position_seconds;
}

const char *get_playback_uuid(airplay_video_t *airplay_video) {
    return (const char *) (!airplay_video ? NULL : airplay_video->playback_uuid); 
}

const char *get_playback_location(airplay_video_t *airplay_video) {
    return (const char *) (!airplay_video ? NULL : airplay_video->playback_location); 
}

static bool has_playlist_header(const char *playlist) {
    if (!playlist || strncmp(playlist, "#EXTM3U", 7)) {
        return false;
    }
    return playlist[7] == '\n' || playlist[7] == '\r' || playlist[7] == '\0';
}

bool airplay_video_is_ready(const airplay_video_t *airplay_video) {
    if (!airplay_video || !airplay_video->playback_location ||
        !airplay_video->playback_location[0]) {
        return false;
    }

    const char *location = airplay_video->playback_location;
    bool local_master = false;
    if (airplay_video->local_uri_prefix) {
        size_t prefix_len = strlen(airplay_video->local_uri_prefix);
        if (!strncmp(location, airplay_video->local_uri_prefix, prefix_len)) {
            const char *suffix = location + prefix_len;
            size_t master_len = strlen("/master.m3u8");
            local_master = !strncmp(suffix, "/master.m3u8", master_len) &&
                           (suffix[master_len] == '\0' || suffix[master_len] == '?' ||
                            suffix[master_len] == '#');
        }
    }

    /* Ordinary UHF/Safari HTTP URLs bypass FCUP and have no playlist cache.
     * Our local master location is assigned before its cache has been filled. */
    if (!local_master && !airplay_video->uri_prefix &&
        !airplay_video->master_playlist && !airplay_video->media_data_store &&
        airplay_video->num_uri == 0) {
        return !strncmp(location, "http://", 7) || !strncmp(location, "https://", 8);
    }

    if (!has_playlist_header(airplay_video->master_playlist) ||
        !airplay_video->media_data_store || airplay_video->num_uri <= 0) {
        return false;
    }
    for (int i = 0; i < airplay_video->num_uri; i++) {
        const media_item_t *item = &airplay_video->media_data_store[i];
        if (!item->uri || !item->uri[0] || item->num < 0 || item->num >= airplay_video->num_uri) {
            return false;
        }
        /* Duplicate entries point directly at the first stored copy. Match the
         * same one-hop resolution used by get_media_playlist(). */
        const media_item_t *stored = &airplay_video->media_data_store[item->num];
        if (!stored->uri || strcmp(item->uri, stored->uri) ||
            !has_playlist_header(stored->playlist)) {
            return false;
        }
    }
    return true;
}

void airplay_video_cancel_pending(airplay_video_t *video) {
    if (video && !airplay_video_is_ready(video)) video->cancelled = true;
}

bool airplay_video_is_cancelled(const airplay_video_t *video) {
    return video && video->cancelled;
}

const char *get_uri_prefix(airplay_video_t *airplay_video) {
    return (const char *) airplay_video->uri_prefix;
}

const char *get_language_name(airplay_video_t *airplay_video) {
    return (const char *)airplay_video->language_name;
}

const char *get_language_code(airplay_video_t *airplay_video) {
    return (const char *) airplay_video->language_code;
}

char *get_uri_local_prefix(airplay_video_t *airplay_video) {
    return airplay_video->local_uri_prefix;
}

int get_next_FCUP_RequestID(airplay_video_t *airplay_video) {    
    if (airplay_video->cache_id[0])
        airplay_video->FCUP_RequestID = raop_next_scoped_fcup_request_id(airplay_video->raop);
    else airplay_video->FCUP_RequestID++;
    return airplay_video->FCUP_RequestID;
}

int get_current_FCUP_RequestID(const airplay_video_t *airplay_video) {
    return airplay_video->FCUP_RequestID;
}

void  set_next_media_uri_id(airplay_video_t *airplay_video, int num) {
    airplay_video->next_uri = num;
}

int get_next_media_uri_id(airplay_video_t *airplay_video) {
    return airplay_video->next_uri;
}

void store_master_playlist(airplay_video_t *airplay_video, char *master_playlist) {
    if (airplay_video->master_playlist) {
        free (airplay_video->master_playlist);
    }
    airplay_video->master_playlist = master_playlist;
}

/* Attribute parsing is shared with cache topology validation below. */
static char *master_attribute(const char *line, const char *name);

typedef struct {
    const char *start;
    size_t length;
    char *code;
    char *name;
    char *group;
    bool is_default;
    bool keep;
} language_line_t;

static bool preferred_language(const char *code, const char *preference, size_t length) {
    return code && length && !strncmp(code, preference, length) &&
           (!code[length] || code[length] == '-');
}

char *select_master_playlist_language(airplay_video_t *video, char *master) {
    if (!video || !master) return master;
    size_t count = 0;
    for (const char *p = master; *p; ) {
        count++;
        const char *end = strchr(p, '\n');
        p = end ? end + 1 : p + strlen(p);
    }
    language_line_t *lines = calloc(count ? count : 1, sizeof(*lines));
    if (!lines) return master;
    const char *p = master;
    size_t choice = count;
    for (size_t i = 0; i < count; i++) {
        const char *end = strchr(p, '\n');
        lines[i].start = p;
        lines[i].length = end ? (size_t)(end + 1 - p) : strlen(p);
        lines[i].keep = true;
        if (!strncmp(p, "#EXT-X-MEDIA:", 13)) {
            char *text = malloc(lines[i].length + 1);
            if (text) {
                memcpy(text, p, lines[i].length);
                text[lines[i].length] = '\0';
                text[strcspn(text, "\r\n")] = '\0';
                char *type = master_attribute(text, "TYPE");
                if (type && !strcmp(type, "AUDIO")) {
                    lines[i].code = master_attribute(text, "LANGUAGE");
                    lines[i].name = master_attribute(text, "NAME");
                    lines[i].group = master_attribute(text, "GROUP-ID");
                    char *value = master_attribute(text, "DEFAULT");
                    lines[i].is_default = value && !strcmp(value, "YES");
                    free(value);
                }
                free(type);
                free(text);
            }
        }
        /* Missing optional attributes and arbitrary attribute order are valid.
         * Keep unclassified lines intact; never infer slices across newlines. */
        if (lines[i].code && lines[i].code[0] && lines[i].name &&
            lines[i].name[0] && lines[i].group && lines[i].group[0]) {
            if (choice == count || (lines[i].is_default && !lines[choice].is_default)) choice = i;
        }
        p += lines[i].length;
    }
    if (video->language_name) {
        for (size_t i = 0; i < count; i++) {
            if (lines[i].code && lines[i].code[0] && lines[i].group && lines[i].group[0] &&
                lines[i].name && !strcmp(lines[i].name, video->language_name)) { choice = i; break; }
        }
    }
    for (const char *preference = video->lang; preference && *preference; ) {
        const char *end = strchr(preference, ':');
        size_t length = end ? (size_t)(end - preference) : strlen(preference);
        bool found = false;
        for (size_t i = 0; i < count; i++) {
            if (lines[i].name && lines[i].name[0] && lines[i].group && lines[i].group[0] &&
                preferred_language(lines[i].code, preference, length)) {
                choice = i;
                found = true;
                break;
            }
        }
        if (found) break;
        preference = end ? end + 1 : NULL;
    }
    if (choice < count) {
        set_language_name(video, lines[choice].name, strlen(lines[choice].name));
        set_language_code(video, lines[choice].code, strlen(lines[choice].code));
        for (size_t i = 0; i < count; i++) {
            if (!lines[i].code || !lines[i].group || !lines[i].name) continue;
            size_t selected = i;
            /* Each rendition group must retain a usable language even when
             * the requested language is absent from that particular group. */
            for (size_t j = 0; j < count; j++) {
                if (!lines[j].code || !lines[j].name || !lines[j].group ||
                    strcmp(lines[i].group, lines[j].group)) continue;
                if (!strcmp(lines[j].code, lines[choice].code)) { selected = j; break; }
                if (lines[j].is_default || j < selected) selected = j;
            }
            lines[i].keep = !strcmp(lines[i].code, lines[selected].code);
        }
    }
    char *filtered = malloc(strlen(master) + 1);
    size_t written = 0;
    for (size_t i = 0; i < count; i++) {
        if (filtered && lines[i].keep) {
            memcpy(filtered + written, lines[i].start, lines[i].length);
            written += lines[i].length;
        }
        free(lines[i].code);
        free(lines[i].name);
        free(lines[i].group);
    }
    free(lines);
    if (!filtered) return master;
    filtered[written] = '\0';
    free(master);
    return filtered;
}

char *get_master_playlist(airplay_video_t *airplay_video) {
    return  airplay_video->master_playlist;
}

/* media_data_store */

int get_num_media_uri(airplay_video_t *airplay_video) {
    return airplay_video->num_uri;
}

void destroy_media_data_store(airplay_video_t *airplay_video) {
    media_item_t *media_data_store = airplay_video->media_data_store; 
    if (media_data_store) {
        for (int i = 0; i < airplay_video->num_uri ; i ++ ) {
            if (media_data_store[i].uri) {
                free (media_data_store[i].uri);
            }
            if (media_data_store[i].playlist) {
                free (media_data_store[i].playlist);
            }
        }
    }
    free (media_data_store);
    airplay_video->media_data_store = NULL;
    airplay_video->num_uri = 0;
    airplay_video->next_uri = 0;
}

void create_media_data_store(airplay_video_t * airplay_video, char ** uri_list, int num_uri) {  
    destroy_media_data_store(airplay_video);
    media_item_t *media_data_store = calloc(num_uri, sizeof(media_item_t));
    if (!media_data_store) {
        printf("Memory allocation failure (media_data_store)\n");
        exit(1);
    }
    for (int i = 0; i < num_uri; i++) {
        media_data_store[i].uri = uri_list[i];
        media_data_store[i].playlist = NULL;
        media_data_store[i].num = i;
        media_data_store[i].count = 0;
        media_data_store[i].duration = 0;
        media_data_store[i].endlist = false;
        media_data_store[i].playlist_type = NONE;
        media_data_store[i].hls_version = 0;
        media_data_store[i].media_sequence = 0;
    }
    airplay_video->media_data_store = media_data_store;
    airplay_video->num_uri = num_uri;
}


static int parse_media_playlist(media_item_t *media_item) {
    const char *ptr = media_item->playlist;
    char extm3u[] = "#EXTM3U";
    char extinf[] = "#EXTINF:";
    char extx[] = "#EXT-X-";
    char playlist_type[] = "PLAYLIST-TYPE:";
    char version[] = "VERSION:";
    char media_sequence[] = "MEDIA-SEQUENCE:";
    ptr = strstr(ptr, extm3u);
    if (!ptr) {
        return -1;
    }
    ptr++;
    while (ptr) {
        const char *ptr1 = NULL;
        ptr = strstr(ptr, "#EXT");
        if (!ptr || !memcmp(ptr, extinf, strlen(extinf))) {
            break;
        }
        ptr = strstr(ptr, extx);
        if (!ptr) {
            break;
        }
        if ((ptr1 = strstr(ptr, playlist_type))) {
            ptr1 += strlen(playlist_type);
            if (!memcmp(ptr1,"VOD", strlen("VOD"))) {
                media_item->playlist_type = VOD;
            } else if (!memcmp(ptr1,"EVENT", strlen("EVENT"))) {
                media_item->playlist_type = EVENT;
            }
            ptr1 = NULL;
        }
        if ((ptr1 = strstr(ptr, version))) {
            char *endptr = NULL;
            ptr1 += strlen(version);
            media_item->hls_version = (int) strtol(ptr1, &endptr, 10);
            ptr1 = NULL;
        }
        if ((ptr1 = strstr(ptr, media_sequence))) {
            char *endptr = NULL;
            ptr1 += strlen(media_sequence);
            media_item->media_sequence = (int) strtol(ptr1, &endptr, 10);
            ptr1 = NULL;
        }
        ptr += strlen(extx);
    }
    return 0;
}

int store_media_playlist(airplay_video_t *airplay_video, char * media_playlist, int *count, float *duration, bool *endlist, int num) {
    media_item_t *media_data_store = airplay_video->media_data_store;
    if ( num < 0 ||  num >= airplay_video->num_uri) {
        return -1;
    } else if (media_data_store[num].playlist) {
        return -2;
    }
    /* dont store duplicate media paylists */
    for (int i = 0; i < num ; i++) {
        if (strcmp(media_data_store[i].uri, media_data_store[num].uri) == 0) {
            /* An earlier request for this URI may have failed. A later copy
             * must still be stored, rather than dereferencing its NULL body. */
            if (!media_data_store[i].playlist ||
                strcmp(media_data_store[i].playlist, media_playlist)) {
                continue;
            }
            media_data_store[num].num = i;
            free (media_playlist);
            return 1;
        }
    }
    media_item_t *media_item = &media_data_store[num];
    media_item->playlist = media_playlist;
    media_item->count = *count;
    media_item->duration = *duration;
    media_item->endlist = *endlist;
    parse_media_playlist(media_item);
    return 0;
}

static bool media_uri_matches(const airplay_video_t *video, const char *stored,
                              const char *requested) {
    if (!stored || !requested) return false;
    if (!strcmp(stored, requested)) return true;
    if (!video->uri_prefix || !video->local_uri_prefix) return false;
    size_t original_len = strlen(video->uri_prefix);
    if (strncmp(stored, video->uri_prefix, original_len)) return false;
    const char *path = stored + original_len;
    if (path[0] != '/') return false;
    size_t local_len = strlen(video->local_uri_prefix);
    if (!strncmp(requested, video->local_uri_prefix, local_len)) {
        requested += local_len;
    }
    if (!strcmp(path, requested)) return true;
    return path[0] == '/' && requested[0] != '/' && !strcmp(path + 1, requested);
}

static int media_reference_index(const airplay_video_t *video, const char *uri, bool cached) {
    if (!video || !video->media_data_store) return -1;
    for (int i = 0; i < video->num_uri; i++) {
        const media_item_t *entry = &video->media_data_store[i];
        if (!media_uri_matches(video, entry->uri, uri) ||
            entry->num < 0 || entry->num >= video->num_uri) continue;
        const media_item_t *stored = &video->media_data_store[entry->num];
        if (stored->uri && !strcmp(entry->uri, stored->uri) &&
            (!cached || has_playlist_header(stored->playlist))) return entry->num;
    }
    return -1;
}

static int available_media_index(const airplay_video_t *video, const char *uri) {
    return media_reference_index(video, uri, true);
}

/* HLS attribute values may be quoted and contain commas (notably CODECS).
 * Return the bounded, borrowed span of one named value. The wrapper below
 * copies it for callers that need a null-terminated string. */
static const char *master_attribute_span(const char *line, size_t length,
                                        const char *name, size_t *value_length) {
    const char *end = line + length;
    const char *p = memchr(line, ':', length);
    if (!p) return NULL;
    p++;
    while (p < end) {
        while (p < end && (*p == ',' || *p == ' ' || *p == '\t')) p++;
        const char *key = p;
        while (p < end && *p != '=' && *p != ',') p++;
        if (p == end || *p != '=') return NULL;
        size_t key_len = (size_t) (p - key);
        const char *value = ++p;
        bool quoted = p < end && *p == '"';
        if (quoted) value = ++p;
        while (p < end && (quoted ? *p != '"' : *p != ',')) p++;
        if (quoted && p == end) return NULL;
        size_t len = (size_t) (p - value);
        if (key_len == strlen(name) && !memcmp(key, name, key_len)) {
            *value_length = len;
            return value;
        }
        if (quoted) p++;
        if (p < end && *p != ',') return NULL;
    }
    return NULL;
}

static char *master_attribute(const char *line, const char *name) {
    size_t length = 0;
    const char *value = master_attribute_span(line, strlen(line), name, &length);
    if (!value) return NULL;
    char *copy = calloc(length + 1, 1);
    if (copy) memcpy(copy, value, length);
    return copy;
}

typedef struct {
    char *text;
    bool keep;
    int media_index;
    char *group;
    char *type;
    unsigned unavailable_route;
} master_line_t;

static bool available_group(master_line_t *lines, int count,
                            const char *type, const char *group) {
    for (int i = 0; i < count; i++) {
        if (lines[i].keep && lines[i].group && lines[i].type &&
            !strcmp(lines[i].group, group) && !strcmp(lines[i].type, type)) return true;
    }
    return false;
}

static bool bounded_unsigned(const char **cursor, unsigned limit, unsigned *result) {
    const char *p = *cursor;
    unsigned value = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned) (*p++ - '0');
        if (value > limit / 10 || (value == limit / 10 && digit > limit % 10)) return false;
        value = value * 10 + digit;
    }
    *result = value;
    *cursor = p;
    return true;
}

static bool pi4_resolution_supported(const char *resolution) {
    unsigned width, height;
    const char *p = resolution;
    return p && bounded_unsigned(&p, 1920, &width) && width > 0 && *p++ == 'x' &&
           bounded_unsigned(&p, 1080, &height) && height > 0 && *p == '\0';
}

static bool pi4_frame_rate_supported(const char *rate) {
    /* Parse HLS decimal syntax without locale dependence or float rounding at
     * the 60fps boundary. Missing FRAME-RATE is allowed by the HLS format. */
    if (!rate) return true;
    unsigned whole;
    if (!bounded_unsigned(&rate, 60, &whole)) return false;
    bool fraction_nonzero = false;
    if (*rate == '.') {
        rate++;
        if (*rate < '0' || *rate > '9') return false;
        while (*rate >= '0' && *rate <= '9') {
            if (*rate++ != '0') fraction_nonzero = true;
        }
    }
    return !*rate && (whole || fraction_nonzero) && !(whole == 60 && fraction_nonzero);
}

static bool h264_codec(const char *codec) {
    if (!strcmp(codec, "avc1") || !strcmp(codec, "avc3")) return true;
    if (strlen(codec) != 11 || (strncmp(codec, "avc1.", 5) && strncmp(codec, "avc3.", 5))) return false;
    for (int i = 5; i < 11; i++) {
        char c = codec[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

typedef struct {
    int variants, codec, resolution, frame_rate, routes, groups;
    int missing_group_type[4];
    int group_absent, group_http, group_local, group_relative, group_other, group_malformed;
} cache_rejections_t;

/* Report only fixed categories and counts. Group names, languages and URIs
 * belong to the sender and must not be copied into ordinary diagnostics. */
static void count_unavailable_group(master_line_t *lines, int count,
                                    const char *type, const char *name,
                                    cache_rejections_t *rejected, size_t type_index) {
    rejected->missing_group_type[type_index]++;
    bool declared = false;
    unsigned routes = 0;
    for (int i = 0; i < count; i++) {
        if (lines[i].group && lines[i].type && !strcmp(lines[i].group, name) &&
            !strcmp(lines[i].type, type)) {
            declared = true;
            routes |= lines[i].unavailable_route;
        }
    }
    if (!declared) rejected->group_absent++;
    if (routes & 1) rejected->group_http++;
    if (routes & 2) rejected->group_local++;
    if (routes & 4) rejected->group_relative++;
    if (routes & 8) rejected->group_other++;
    if (routes & 16) rejected->group_malformed++;
}

static bool ascii_name_equal(const char *name, size_t length, const char *expected) {
    if (length != strlen(expected)) return false;
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != expected[i]) return false;
    }
    return true;
}

typedef struct {
    unsigned port;
    bool loopback;
} rendition_origin_t;

/* Validate the authority without changing the URI sent to the player. This
 * also recognizes common loopback spellings of our private cache origin. */
static bool rendition_http_origin(const char *uri, rendition_origin_t *origin) {
    const char *colon = strchr(uri, ':');
    if (!colon || (size_t)(colon - uri) > 5) return false;
    bool https = ascii_name_equal(uri, (size_t)(colon - uri), "https");
    if (!https && !ascii_name_equal(uri, (size_t)(colon - uri), "http")) return false;
    if (strncmp(colon, "://", 3)) return false;
    for (const unsigned char *p = (const unsigned char *)uri; *p; p++)
        if (*p <= 32 || *p == 127 || *p == '\\') return false;
    const char *host = colon + 3;
    const char *end = host + strcspn(host, "/?#");
    for (const char *p = host; p < end; p++) if (*p == '@') host = p + 1;
    if (host == end) return false;
    const char *port = end;
    size_t host_length;
    bool ipv6 = host[0] == '[';
    if (ipv6) {
        host++;
        const char *close = memchr(host, ']', (size_t)(end - host));
        if (!close || close == host || (close + 1 < end && close[1] != ':')) return false;
        host_length = (size_t)(close - host);
        if (close + 1 < end) port = close + 2;
    } else {
        const char *separator = memchr(host, ':', (size_t)(end - host));
        host_length = (size_t)((separator ? separator : end) - host);
        if (separator) port = separator + 1;
        for (size_t i = 0; i < host_length; i++) {
            unsigned char c = host[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')) return false;
        }
    }
    if (!host_length || host_length >= 256 || (port == end && end[-1] == ':')) return false;
    origin->port = https ? 443 : 80;
    if (port != end) {
        const char *cursor = port;
        if (!bounded_unsigned(&cursor, 65535, &origin->port) || cursor != end || !origin->port) return false;
    }
    char address[256];
    memcpy(address, host, host_length);
    address[host_length] = '\0';
    unsigned char bytes[16];
    origin->loopback = false;
    if (ipv6) {
        if (inet_pton(AF_INET6, address, bytes) != 1) return false;
        static const unsigned char loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        static const unsigned char mapped[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
        origin->loopback = !memcmp(bytes, loopback, 16) ||
                           (!memcmp(bytes, mapped, 12) && bytes[12] == 127);
    } else {
        if (host_length && address[host_length - 1] == '.') address[--host_length] = '\0';
        origin->loopback = ascii_name_equal(address, host_length, "localhost") ||
                           (inet_pton(AF_INET, address, bytes) == 1 && bytes[0] == 127);
    }
    return true;
}

static unsigned unavailable_rendition_route(const airplay_video_t *video, const char *uri) {
    const char *colon = strchr(uri, ':');
    if (colon && (ascii_name_equal(uri, (size_t)(colon - uri), "http") ||
                  ascii_name_equal(uri, (size_t)(colon - uri), "https"))) {
        rendition_origin_t origin, receiver;
        if (!rendition_http_origin(uri, &origin)) return 16;
        /* The local prefix is constructed by init and optionally gains a
         * cache path. Validate it here rather than assuming its scheme/size. */
        if (!video->local_uri_prefix || !rendition_http_origin(video->local_uri_prefix, &receiver)) return 16;
        return origin.loopback && origin.port == receiver.port ? 2 : 1;
    }
    /* A URI without a scheme may be relative to the original master. */
    if (!colon || colon >= uri + strcspn(uri, "/?#")) return 4;
    return 8;
}

static bool pi4_variant_supported(const char *line, cache_rejections_t *rejections) {
    char *codecs = master_attribute(line, "CODECS");
    char *resolution = master_attribute(line, "RESOLUTION");
    char *rate = master_attribute(line, "FRAME-RATE");
    bool resolution_ok = pi4_resolution_supported(resolution);
    bool rate_ok = pi4_frame_rate_supported(rate) && (rate || !strstr(line, "FRAME-RATE="));
    bool supported = codecs != NULL;
    unsigned video_codecs = 0, audio_codecs = 0;
    for (char *p = codecs; supported && p; ) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        if (h264_codec(p)) video_codecs++;
        else if (!strcmp(p, "mp4a.40.2")) audio_codecs++;
        else supported = false;
        p = comma ? comma + 1 : NULL;
    }
    free(codecs);
    free(resolution);
    free(rate);
    bool codecs_ok = supported && video_codecs == 1 && audio_codecs == 1;
    if (!codecs_ok) rejections->codec++;
    if (!resolution_ok) rejections->resolution++;
    if (!rate_ok) rejections->frame_rate++;
    return codecs_ok && resolution_ok && rate_ok;
}

bool airplay_video_finalize_cache(airplay_video_t *video) {
    return airplay_video_finalize_cache_profile(video, false);
}

static bool filter_cache_profile(airplay_video_t *video, bool pi4, bool single, bool cached) {
    if (!video) return false;
    snprintf(video->cache_diagnostics, sizeof(video->cache_diagnostics),
             "phase=%s reason=invalid-master-or-routes", cached ? "finalize" : "prepare");
    if (!video->master_playlist && !video->media_data_store &&
        airplay_video_is_ready(video)) return true; /* Direct HTTP playback. */
    if (!has_playlist_header(video->master_playlist) ||
        !video->media_data_store || video->num_uri <= 0) {
        free(video->master_playlist);
        video->master_playlist = NULL;
        return false;
    }

    size_t length = strlen(video->master_playlist);
    int count = 1;
    for (const char *p = video->master_playlist; *p; p++) if (*p == '\n') count++;
    master_line_t *lines = calloc((size_t) count, sizeof(*lines));
    char *text = malloc(length + 1);
    char *filtered = calloc(length + 2, 1);
    bool *used = calloc((size_t) video->num_uri, sizeof(*used));
    if (!lines || !text || !filtered || !used) {
        free(lines); free(text); free(filtered); free(used);
        free(video->master_playlist);
        video->master_playlist = NULL;
        return false;
    }
    memcpy(text, video->master_playlist, length + 1);
    char *next = text;
    bool missing_local_reference = false;
    for (int i = 0; i < count; i++) {
        lines[i].text = next;
        lines[i].media_index = -1;
        char *end = strchr(next, '\n');
        if (end) { *end = '\0'; next = end + 1; }
        size_t len = strlen(lines[i].text);
        if (len && lines[i].text[len - 1] == '\r') lines[i].text[len - 1] = '\0';
        lines[i].keep = lines[i].text[0] == '#';
        if (!strncmp(lines[i].text, "#EXT-X-MEDIA:", 13) ||
            !strncmp(lines[i].text, "#EXT-X-I-FRAME-STREAM-INF:", 26)) {
            char *uri = master_attribute(lines[i].text, "URI");
            if (uri) {
                lines[i].media_index = media_reference_index(video, uri, cached);
                lines[i].keep = lines[i].media_index >= 0;
                if (!lines[i].keep) {
                    lines[i].unavailable_route = unavailable_rendition_route(video, uri);
                    /* FCUP downloads only the sender's relay URLs. Ordinary
                     * HTTP(S) audio/subtitle renditions remain fetchable by
                     * the player and do not need a local cache entry. */
                    if (!strncmp(lines[i].text, "#EXT-X-MEDIA:", 13) &&
                        lines[i].unavailable_route == 1) lines[i].keep = true;
                }
                free(uri);
            } else if (strstr(lines[i].text, "URI=") ||
                       !strncmp(lines[i].text, "#EXT-X-I-FRAME-STREAM-INF:", 26)) {
                lines[i].keep = false; /* Malformed or missing required URI. */
                lines[i].unavailable_route = 16;
            }
            if (!strncmp(lines[i].text, "#EXT-X-MEDIA:", 13)) {
                lines[i].group = master_attribute(lines[i].text, "GROUP-ID");
                lines[i].type = master_attribute(lines[i].text, "TYPE");
            }
        } else if (lines[i].text[0] == '#' && video->local_uri_prefix) {
            /* Unknown tags may carry required resources (for example a
             * session key). Do not silently remove their missing local URI. */
            char *uri = master_attribute(lines[i].text, "URI");
            if (!uri) uri = master_attribute(lines[i].text, "SERVER-URI");
            size_t prefix_len = strlen(video->local_uri_prefix);
            if (uri && !strncmp(uri, video->local_uri_prefix, prefix_len) && uri[prefix_len] == '/') {
                lines[i].media_index = media_reference_index(video, uri, cached);
                if (lines[i].media_index < 0) missing_local_reference = true;
            }
            free(uri);
        }
    }

    int variants = 0;
    cache_rejections_t rejected = {0};
    for (int i = 0; i < count; i++) {
        if (strncmp(lines[i].text, "#EXT-X-STREAM-INF:", 18)) continue;
        lines[i].keep = false;
        rejected.variants++;
        if (pi4 && !pi4_variant_supported(lines[i].text, &rejected)) continue;
        int uri_line = i + 1;
        while (uri_line < count && !lines[uri_line].text[0]) uri_line++;
        if (uri_line >= count || lines[uri_line].text[0] == '#') { rejected.routes++; continue; }
        int index = media_reference_index(video, lines[uri_line].text, cached);
        if (index < 0) { rejected.routes++; continue; }
        static const char *groups[] = { "AUDIO", "VIDEO", "SUBTITLES", "CLOSED-CAPTIONS" };
        bool playable = true;
        for (size_t group = 0; group < sizeof(groups) / sizeof(groups[0]); group++) {
            char *name = master_attribute(lines[i].text, groups[group]);
            if (name && strcmp(name, "NONE") && !available_group(lines, count, groups[group], name)) {
                playable = false;
                count_unavailable_group(lines, count, groups[group], name, &rejected, group);
            }
            free(name);
        }
        if (!playable) { rejected.groups++; continue; }
        lines[i].keep = true;
        lines[uri_line].keep = true;
        lines[uri_line].media_index = index;
        variants++;
    }

    if (single && variants > 1) {
        /* Match mpv's default hls-bitrate=max, after unavailable variants and
         * their dependencies have been removed. Without valid bandwidths,
         * leave selection to the player instead of guessing quality. */
        int selected = -1;
        unsigned highest = 0;
        bool ranked = true;
        for (int i = 0; i < count; i++) {
            if (!lines[i].keep || strncmp(lines[i].text, "#EXT-X-STREAM-INF:", 18)) continue;
            char *bandwidth = master_attribute(lines[i].text, "BANDWIDTH");
            const char *cursor = bandwidth;
            unsigned value;
            bool valid = cursor && bounded_unsigned(&cursor, UINT_MAX, &value) && !*cursor && value > 0;
            if (!valid) ranked = false;
            else if (selected < 0 || value > highest) { selected = i; highest = value; }
            free(bandwidth);
        }
        if (ranked && selected >= 0) {
            for (int i = 0; i < count; i++) {
                if (i == selected || !lines[i].keep || strncmp(lines[i].text, "#EXT-X-STREAM-INF:", 18)) continue;
                lines[i].keep = false;
                int uri_line = i + 1;
                while (uri_line < count && !lines[uri_line].text[0]) uri_line++;
                if (uri_line < count) lines[uri_line].keep = false;
            }
            variants = 1;
        }
    }

    if (pi4) {
        /* Do not expose unused HE-AAC/other audio groups from variants removed
         * above. Retain all available tracks within an actively used group. */
        for (int i = 0; i < count; i++) {
            if (!lines[i].keep || !lines[i].type || (!single && strcmp(lines[i].type, "AUDIO"))) continue;
            bool referenced = false;
            for (int j = 0; j < count && !referenced; j++) {
                if (!lines[j].keep || strncmp(lines[j].text, "#EXT-X-STREAM-INF:", 18)) continue;
                char *group = master_attribute(lines[j].text, lines[i].type);
                referenced = group && lines[i].group && !strcmp(group, lines[i].group);
                free(group);
            }
            if (!referenced) lines[i].keep = false;
        }
    }

    snprintf(video->cache_diagnostics, sizeof(video->cache_diagnostics),
             "phase=%s variants=%d retained=%d unsupported_codecs=%d unsupported_resolution=%d "
             "unsupported_frame_rate=%d unavailable_routes=%d unavailable_groups=%d missing_local_resource=%d "
             "missing_audio=%d missing_video=%d missing_subtitles=%d missing_captions=%d "
             "group_absent=%d group_http=%d group_local=%d group_relative=%d group_other=%d group_malformed=%d",
             cached ? "finalize" : "prepare", rejected.variants, variants, rejected.codec,
             rejected.resolution, rejected.frame_rate, rejected.routes, rejected.groups, missing_local_reference,
             rejected.missing_group_type[0], rejected.missing_group_type[1],
             rejected.missing_group_type[2], rejected.missing_group_type[3], rejected.group_absent,
             rejected.group_http, rejected.group_local, rejected.group_relative, rejected.group_other,
             rejected.group_malformed);
    size_t written = 0;
    for (int i = 0; i < count; i++) {
        if (lines[i].keep) {
            size_t len = strlen(lines[i].text);
            memcpy(filtered + written, lines[i].text, len);
            written += len;
            filtered[written++] = '\n';
            if (lines[i].media_index >= 0) used[lines[i].media_index] = true;
        }
        free(lines[i].group);
        free(lines[i].type);
    }
    free(lines);
    free(text);
    int retained = 0;
    for (int i = 0; i < video->num_uri; i++) if (used[i]) retained++;
    media_item_t *compact = variants && retained && !missing_local_reference ?
                           calloc((size_t) retained, sizeof(*compact)) : NULL;
    if (!compact) {
        free(filtered); free(used);
        free(video->master_playlist);
        video->master_playlist = NULL;
        return false;
    }
    int target = 0;
    for (int i = 0; i < video->num_uri; i++) {
        if (used[i]) {
            compact[target] = video->media_data_store[i];
            compact[target].num = target;
            target++;
        } else {
            free(video->media_data_store[i].uri);
            free(video->media_data_store[i].playlist);
        }
    }
    free(used);
    free(video->media_data_store);
    video->media_data_store = compact;
    video->num_uri = retained;
    video->next_uri = cached ? retained : 0;
    free(video->master_playlist);
    video->master_playlist = filtered;
    return !cached || airplay_video_is_ready(video);
}

const char *airplay_video_get_cache_diagnostics(const airplay_video_t *video) {
    return video && video->cache_diagnostics[0] ? video->cache_diagnostics : "reason=unavailable";
}

bool airplay_video_finalize_cache_profile(airplay_video_t *video, bool pi4) {
    return filter_cache_profile(video, pi4, false, true);
}

bool airplay_video_prepare_cache_profile(airplay_video_t *video) {
    /* Keep every compatible candidate until the downloads establish which
     * qualities and audio groups are actually available. */
    return filter_cache_profile(video, true, false, false);
}

bool airplay_video_finalize_cache_mpv(airplay_video_t *video) {
    return filter_cache_profile(video, true, true, true);
}

char * get_media_playlist(airplay_video_t *airplay_video, int *count, float *duration, const char *uri) {
    int index = available_media_index(airplay_video, uri);
    if (index < 0) return NULL;
    const media_item_t *entry = &airplay_video->media_data_store[index];
    *count = entry->count;
    *duration = entry->duration;
    return entry->playlist;
}

char * get_media_uri_by_num(airplay_video_t *airplay_video, int num) {
    media_item_t * media_data_store = airplay_video->media_data_store;
    if (num >= 0 && num < airplay_video->num_uri) {
        return  media_data_store[num].uri;
    }
    return NULL;
}

int analyze_media_playlist(char *playlist, float *duration, bool *endlist) {
    float next;
    int count = 0;
    char *ptr = strstr(playlist, "#EXTINF:");
    *duration = 0.0f;
    *endlist = false;
    char *end = NULL;
    while (ptr != NULL) {
        ptr += strlen("#EXTINF:");
        next = strtof(ptr, &end);
        *duration += next;
        count++;
        ptr = strstr(end, "#EXTINF:");
    }
    if (end) {
        *endlist = (strstr(end, "#EXT-X-ENDLIST"));
    }
    return count;
}

/* Extract complete URI values, including query strings and extensionless
 * routes. HLS does not require a .m3u8 suffix. Only playlist-bearing tags and
 * URI lines belong in the FCUP playlist queue; keys and comments do not. */
int create_media_uri_table(const char *url_prefix, const char *master_playlist_data,
                           int datalen, char ***media_uri_table, int *num_uri) {
    if (!media_uri_table || !num_uri) return -1;
    *media_uri_table = NULL;
    *num_uri = 0;
    if (!url_prefix || !*url_prefix || !master_playlist_data || datalen <= 0 ||
        memchr(master_playlist_data, '\0', (size_t)datalen)) return -1;
    char *text = malloc((size_t)datalen + 1);
    if (!text) return -1;
    memcpy(text, master_playlist_data, (size_t)datalen);
    text[datalen] = '\0';
    char **table = NULL;
    size_t prefix_length = strlen(url_prefix);
    int count = 0;
    for (char *line = text; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        line[strcspn(line, "\r")] = '\0';
        char *uri = NULL;
        if (line[0] && line[0] != '#') {
            uri = malloc(strlen(line) + 1);
            if (uri) strcpy(uri, line);
            else goto error;
        } else if (!strncmp(line, "#EXT-X-MEDIA:", 13) ||
                   !strncmp(line, "#EXT-X-I-FRAME-STREAM-INF:", 26)) {
            uri = master_attribute(line, "URI");
        }
        if (uri && !strncmp(uri, url_prefix, prefix_length) && uri[prefix_length] == '/') {
            char **grown = realloc(table, ((size_t)count + 1) * sizeof(*table));
            if (!grown) { free(uri); goto error; }
            table = grown;
            table[count++] = uri;
        } else free(uri);
        line = next;
    }
    free(text);
    if (!count) { free(table); return -1; }
    *num_uri = count;
    *media_uri_table = table;
    return 0;
 error:
    for (int i = 0; i < count; i++) free(table[i]);
    free(table);
    free(text);
    return -1;
}

static bool playlist_append(char **text, size_t *length, size_t *capacity,
                            const char *data, size_t count);

/* Rewrite only actual URI starts. A relay-prefix string inside another URL's
 * query, an unrelated attribute, or a comment is data, not a cache route. */
char *adjust_master_playlist(char *data, int datalen,
                             const char *prefix, char *local_prefix) {
    if (!data || datalen < 0 || !prefix || !*prefix || !local_prefix ||
        memchr(data, '\0', (size_t)datalen)) return NULL;
    size_t source_length = (size_t)datalen;
    size_t prefix_length = strlen(prefix), local_length = strlen(local_prefix);
    char *result = NULL;
    size_t written = 0, capacity = 0;
    for (const char *line = data; line < data + source_length; ) {
        const char *newline = memchr(line, '\n', (size_t)(data + source_length - line));
        const char *end = newline ? newline + 1 : data + source_length;
        size_t length = (size_t)((newline ? newline : end) - line);
        if (length && line[length - 1] == '\r') length--;
        const char *values[2] = {NULL, NULL};
        size_t lengths[2] = {0, 0};
        if (length && line[0] != '#') {
            values[0] = line;
            lengths[0] = length;
        } else if (length >= 4 && !memcmp(line, "#EXT", 4)) {
            values[0] = master_attribute_span(line, length, "URI", &lengths[0]);
            values[1] = master_attribute_span(line, length, "SERVER-URI", &lengths[1]);
            if (values[1] && (!values[0] || values[1] < values[0])) {
                const char *swap = values[0]; values[0] = values[1]; values[1] = swap;
                size_t swap_length = lengths[0]; lengths[0] = lengths[1]; lengths[1] = swap_length;
            }
        }
        const char *cursor = line;
        for (unsigned i = 0; i < 2; i++) {
            if (!values[i] || lengths[i] <= prefix_length ||
                memcmp(values[i], prefix, prefix_length) || values[i][prefix_length] != '/') continue;
            if (!playlist_append(&result, &written, &capacity, cursor, (size_t)(values[i] - cursor)) ||
                !playlist_append(&result, &written, &capacity, local_prefix, local_length)) goto error;
            cursor = values[i] + prefix_length;
        }
        if (!playlist_append(&result, &written, &capacity, cursor, (size_t)(end - cursor))) goto error;
        line = end;
    }
    if (written > INT_MAX) goto error;
    if (!result) result = calloc(1, 1);
    return result;
 error:
    free(result);
    return NULL;
}

/* Append an exact slice with overflow checks. Condensed URLs are untrusted
 * sender input, so malformed fields must fail the resource, not the receiver. */
static bool playlist_append(char **text, size_t *length, size_t *capacity,
                            const char *data, size_t count) {
    if (count > SIZE_MAX - *length - 1) return false;
    size_t needed = *length + count + 1;
    if (needed > *capacity) {
        size_t grown = *capacity ? *capacity : 256;
        while (grown < needed) {
            if (grown > SIZE_MAX / 2) { grown = needed; break; }
            grown *= 2;
        }
        char *replacement = realloc(*text, grown);
        if (!replacement) return false;
        *text = replacement;
        *capacity = grown;
    }
    memcpy(*text + *length, data, count);
    *length += count;
    (*text)[*length] = '\0';
    return true;
}

char *adjust_yt_condensed_playlist(const char *media_playlist) {
    if (!has_playlist_header(media_playlist)) return NULL;
    const char *header = strstr(media_playlist, "#YT-EXT-CONDENSED-URL:");
    if (!header) {
        size_t length = strlen(media_playlist);
        char *copy = malloc(length + 1);
        if (copy) memcpy(copy, media_playlist, length + 1);
        return copy;
    }
    if (header != media_playlist && header[-1] != '\n') return NULL;
    size_t header_length = strcspn(header, "\r\n");
    char *header_line = malloc(header_length + 1);
    if (!header_line) return NULL;
    memcpy(header_line, header, header_length);
    header_line[header_length] = '\0';
    char *base = master_attribute(header_line, "BASE-URI");
    char *params = master_attribute(header_line, "PARAMS");
    char *prefix = master_attribute(header_line, "PREFIX");
    free(header_line);
    char *result = NULL;
    size_t length = 0, capacity = 0;
    if (!base || !*base || !params || !prefix || !*prefix) goto error;
    size_t prefix_length = strlen(prefix);
    for (const char *line = media_playlist; *line; ) {
        const char *end = strchr(line, '\n');
        const char *line_end = end ? end : line + strlen(line);
        const char *content_end = line_end;
        if (content_end > line && content_end[-1] == '\r') content_end--;
        size_t line_length = (size_t)(content_end - line);
        if (line_length && line[0] != '#') {
            if (line_length < prefix_length || memcmp(line, prefix, prefix_length)) goto error;
            const char *value = line + prefix_length;
            if (!playlist_append(&result, &length, &capacity, base, strlen(base))) goto error;
            if (!*params) {
                if (!playlist_append(&result, &length, &capacity, value, (size_t)(content_end - value))) goto error;
            } else {
                for (const char *param = params; ; ) {
                    const char *comma = strchr(param, ',');
                    size_t param_length = comma ? (size_t)(comma - param) : strlen(param);
                    const char *value_end = comma ? memchr(value, '/', (size_t)(content_end - value)) : content_end;
                    if (!param_length || !value_end || value_end == value) goto error;
                    if (!playlist_append(&result, &length, &capacity, "/", 1) ||
                        !playlist_append(&result, &length, &capacity, param, param_length) ||
                        !playlist_append(&result, &length, &capacity, "/", 1) ||
                        !playlist_append(&result, &length, &capacity, value, (size_t)(value_end - value))) goto error;
                    if (!comma) break;
                    param = comma + 1;
                    value = value_end + 1;
                }
            }
            if (!playlist_append(&result, &length, &capacity, content_end,
                                 (size_t)((end ? end + 1 : line_end) - content_end))) goto error;
        } else if (!playlist_append(&result, &length, &capacity, line,
                                    (size_t)((end ? end + 1 : line_end) - line))) goto error;
        line = end ? end + 1 : line_end;
    }
    free(base); free(params); free(prefix);
    return result;
 error:
    free(base); free(params); free(prefix); free(result);
    return NULL;
}
