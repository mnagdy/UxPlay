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

/* this file is part of raop.c and should not be included in any other file */

#include "airplay_video.h"
#include "fcup_request.h"
#include <math.h>
#include <float.h>

static void
*hls_get_current_video(raop_t *raop) {
    if (raop->current_video < 0) {
        logger_log(raop->logger, LOGGER_ERR, "hls_get_current_video: failed to identify current_playlist");
        return NULL;
    }
    assert(raop->airplay_video[raop->current_video]);
    return (void *) raop->airplay_video[raop->current_video];
}

static int
get_playlist_by_uuid(raop_t *raop, const char *uuid) {
    for (int i = 0 ;i < MAX_AIRPLAY_VIDEO; i++) {
        if (raop->airplay_video[i] && !strcmp(uuid, get_playback_uuid(raop->airplay_video[i]))) {
            return i;
        }
    }
    return -1;
}

/* Player control follows the current AirPlay session, never the TCP
 * connection's original classification. A reused connection may be stale. */
static bool
http_video_control_is_current(raop_conn_t *conn, http_request_t *request,
                              http_response_t *response) {
    raop_t *raop = conn->raop;
    if (!raop->hls_scoped_cache) return true;
    const char *incoming = http_request_get_header(request, "X-Apple-Session-ID");
    airplay_video_t *video = raop->current_video >= 0 && raop->current_video < MAX_AIRPLAY_VIDEO ?
                             raop->airplay_video[raop->current_video] : NULL;
    const char *active = video ? get_apple_session_id(video) : NULL;
    if (!incoming || !active || strcmp(incoming, active)) {
        http_response_init(response, "HTTP/1.1", 409, "Conflict");
        logger_log(raop->logger, LOGGER_DEBUG, "AirPlay video: ignoring stale session control");
        return false;
    }
    return true;
}

static bool
http_video_peer_is_loopback(const raop_conn_t *conn) {
    if (!conn->remote) return false;
    if (conn->remotelen == 4) return conn->remote[0] == 127;
    if (conn->remotelen == 16) {
        static const unsigned char loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        static const unsigned char mapped[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
        return !memcmp(conn->remote, loopback, sizeof(loopback)) ||
               (!memcmp(conn->remote, mapped, sizeof(mapped)) && conn->remote[12] == 127);
    }
    return false;
}

static void
http_handler_server_info(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                         char **response_data, int *response_datalen)  {

    raop_t *raop = conn->raop;
    assert(raop->dnssd);
    int hw_addr_raw_len = 0;
    const char *hw_addr_raw = dnssd_get_hw_addr(raop->dnssd, &hw_addr_raw_len);

    char *hw_addr = calloc(1, 3 * hw_addr_raw_len);
    utils_hwaddr_airplay(hw_addr, 3 * hw_addr_raw_len, hw_addr_raw, hw_addr_raw_len);

    plist_t r_node = plist_new_dict();

    /* Keep the legacy HTTP profile: the full RAOP mask made a live client
     * select an unsupported HTTP /fp-setup handshake instead of /play. */
    uint64_t features = UINT64_C(0x27F);
    plist_t features_node = plist_new_uint(features);
    plist_dict_set_item(r_node, "features", features_node);
    logger_log(raop->logger, LOGGER_INFO,
               "AirPlay capabilities: endpoint=server-info profile=legacy-http features=0x%" PRIx64, features);

    plist_t mac_address_node = plist_new_string(hw_addr);
    plist_dict_set_item(r_node, "macAddress", mac_address_node);

    plist_t model_node = plist_new_string(GLOBAL_MODEL);
    plist_dict_set_item(r_node, "model", model_node);

    plist_t os_build_node = plist_new_string("12B435");
    plist_dict_set_item(r_node, "osBuildVersion", os_build_node);

    plist_t protovers_node = plist_new_string("1.0");
    plist_dict_set_item(r_node, "protovers", protovers_node);

    plist_t source_version_node = plist_new_string(GLOBAL_VERSION);
    plist_dict_set_item(r_node, "srcvers", source_version_node);

    plist_t vv_node = plist_new_uint(strtol(AIRPLAY_VV, NULL, 10));
    plist_dict_set_item(r_node, "vv", vv_node);

    plist_t device_id_node = plist_new_string(hw_addr);
    plist_dict_set_item(r_node, "deviceid", device_id_node);

    plist_to_xml(r_node, response_data, (uint32_t *) response_datalen);

    //assert(*response_datalen == strlen(*response_data));

    /* last character (at *response_data[response_datalen - 1]) is  0x0a = '\n'
     * (*response_data[response_datalen] is '\0').
     * apsdk removes the last "\n" by overwriting it with '\0', and reducing response_datalen by 1. 
     * TODO: check if this is necessary  */
    
    plist_free(r_node);
    http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
    free(hw_addr);
}    

static void
http_handler_scrub(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                   char **response_data, int *response_datalen) {
    if (!http_video_control_is_current(conn, request, response)) return;
    raop_t *raop = conn->raop;
    const char *url = http_request_get_url(request);
    const char *data = strstr(url, "?");
    float scrub_position = 0.0f;
    if (data) {
        data++;
        const char *position = strstr(data, "=") + 1;
        char *end = NULL;
        double value = strtod(position, &end);
        if (end && end != position) {
            scrub_position = (float) value;
            logger_log(raop->logger, LOGGER_DEBUG, "http_handler_scrub: got position = %.6f",
                       scrub_position);	  
        }
    }
    logger_log(raop->logger, LOGGER_DEBUG, "**********************SCRUB %f ***********************",scrub_position);
    raop->callbacks.on_video_scrub(raop->callbacks.cls, scrub_position);
}

static void
http_handler_rate(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                  char **response_data, int *response_datalen) {

    if (!http_video_control_is_current(conn, request, response)) return;

    raop_t *raop = conn->raop;
    const char *url = http_request_get_url(request);
    const char *data = strstr(url, "?");
    float rate_value = 0.0f;
    if (data) {
        data++;
        const char *rate = strstr(data, "=") + 1;
        char *end = NULL;
        float value = strtof(rate, &end);
        if (end && end != rate) {
            rate_value =  value;
            logger_log(raop->logger, LOGGER_DEBUG, "http_handler_rate: got rate = %.6f", rate_value);
        }
    }
    raop->callbacks.on_video_rate(raop->callbacks.cls, rate_value);
}

static void
http_handler_stop(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                  char **response_data, int *response_datalen) {

    if (!http_video_control_is_current(conn, request, response)) return;

    raop_t *raop = conn->raop;
    logger_log(raop->logger, LOGGER_INFO, "client HTTP request POST stop");

    raop->callbacks.on_video_stop(raop->callbacks.cls);
}

/* handles PUT /setProperty http requests from Client to Server */

static void
http_handler_set_property(raop_conn_t *conn,
                          http_request_t *request, http_response_t *response,
                          char **response_data, int *response_datalen) {

    raop_t *raop = conn->raop;
    const char *url = http_request_get_url(request);
    const char *property = url + strlen("/setProperty?");
    logger_log(raop->logger, LOGGER_DEBUG, "http_handler_set_property: %s", property);


    /*  actionAtItemEnd:  values:  
                  0: advance (advance to next item, if there is one)
                  1: pause   (pause playing)
                  2: none    (do nothing)             

        reverseEndTime   (only used when rate < 0) time at which reverse playback ends
        forwardEndTime   (only used when rate > 0) time at which reverse playback ends
        selectedMediaArray contains plist with language choice:
    */

    airplay_video_t *airplay_video = (airplay_video_t *) hls_get_current_video(raop);
    assert(airplay_video);
    if (!strcmp(property, "selectedMediaArray")) {
        /* verify that this request contains a binary plist*/
        char *header_str = NULL;
        int request_datalen = 0;
        http_request_get_header_string(request, &header_str);
        bool is_plist = strstr(header_str,"apple-binary-plist");
        free(header_str);
        if (!is_plist) {
            logger_log(raop->logger, LOGGER_DEBUG, "POST /setProperty?selectedMediaArray"
                       "does not provide an apple-binary-plist");
            goto post_error;
        }

        const char *request_data = http_request_get_data(request, &request_datalen);
        plist_t req_root_node = NULL;
        plist_from_bin(request_data, request_datalen, &req_root_node);
        plist_t req_value_node = plist_dict_get_item(req_root_node, "value");

        if (!req_value_node || !PLIST_IS_ARRAY(req_value_node)) {	  
            logger_log(raop->logger, LOGGER_INFO, "POST /setProperty?selectedMediaArray"
                   " did not provide expected plist from client");
            goto post_error;
        }

        int count = plist_array_get_size(req_value_node);
        char *name = NULL;
        char *code = NULL;
        char *language_name = NULL;
        char *language_code = NULL;
        for (int i = 0; i < count; i++) {
            plist_t req_value_array_node = plist_array_get_item(req_value_node,i);
            if (!language_name) {
                plist_t req_value_options_name_node =  plist_dict_get_item(req_value_array_node,"MediaSelectionOptionsName");
                if (PLIST_IS_STRING(req_value_options_name_node)) {
                    plist_get_string_val(req_value_options_name_node, &name);
                    if (name) {
                        language_name = (char *) calloc(strlen(name) + 1, sizeof(char));
                        if (!language_name) {
                            printf("Memory allocation failed\n");
                            exit(1);
                        }
                        memcpy(language_name, name, strlen(name));
                        plist_mem_free(name);
                    }
                }
            }
            if (!language_code) {
                plist_t req_value_options_code_node =  plist_dict_get_item(req_value_array_node,"MediaSelectionOptionsUnicodeLanguageIdentifier");
                if (PLIST_IS_STRING(req_value_options_code_node)) {
                    plist_get_string_val(req_value_options_code_node, &code);
                    if (code) {
                        language_code = (char *) calloc(strlen(code) + 1, sizeof(char));
                        if (!language_code) {
                            printf("Memory allocation failed\n");
                            exit(1);
                        }
                        memcpy(language_code, code, strlen(code));
                        plist_mem_free(code);
                    }
                }
            }
            if (language_code && language_name) {
                break;
            } else {
                plist_free (req_value_array_node);
                continue;
            }
        }
        plist_free (req_root_node);
        if (language_code && language_name) {
            set_language_code(airplay_video, language_code, strlen(language_code));
            set_language_name(airplay_video, language_name, strlen(language_name));
            logger_log(raop->logger, LOGGER_INFO, "stored language from MediaSelectionOptions: %s \"%s\"",
                       get_language_code(airplay_video), get_language_name(airplay_video));
        }
        plist_mem_free(language_name);
        plist_mem_free(language_code);
    } else if (!strcmp(property, "reverseEndTime") ||
        !strcmp(property, "forwardEndTime") ||
        !strcmp(property, "actionAtItemEnd")) {
        logger_log(raop->logger, LOGGER_DEBUG, "property %s is known but unhandled", property);

        plist_t errResponse = plist_new_dict();
        plist_t errCode = plist_new_uint(0);
        plist_dict_set_item(errResponse, "errorCode", errCode);
        plist_to_xml(errResponse, response_data, (uint32_t *) response_datalen);
        plist_free(errResponse);
        http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
    } else {
        logger_log(raop->logger, LOGGER_DEBUG, "property %s is unknown, unhandled", property);      
        goto post_error;
    }
    return;
 post_error:
    http_response_add_header(response, "Content-Length", "0");
}

/* handles GET /getProperty http requests from Client to Server.  (not implemented) */

static void
http_handler_get_property(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                          char **response_data, int *response_datalen) {
    raop_t *raop = conn->raop;
    const char *url = http_request_get_url(request);
    const char *property = url + strlen("getProperty?");
    logger_log(raop->logger, LOGGER_DEBUG, "http_handler_get_property: %s (unhandled)", property);
}

/* this request (for a variant FairPlay decryption)  cannot be handled  by UxPlay */
static void
http_handler_fpsetup2(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen) {
    raop_t *raop = conn->raop;
    logger_log(raop->logger, LOGGER_WARNING, "client HTTP request POST fp-setup2 is unhandled");
    http_response_add_header(response, "Content-Type", "application/x-apple-binary-plist");
    int req_datalen = 0;
    const unsigned char *req_data = (unsigned char *) http_request_get_data(request, &req_datalen);
    if (req_data && req_datalen >= 5) {
        logger_log(raop->logger, LOGGER_ERR, "only FairPlay version 0x03 is implemented, version is 0x%2.2x",
                   req_data[4]);
    } else {
        logger_log(raop->logger, LOGGER_ERR, "Invalid fp-setup2 data length: %d", req_datalen);
    }
    http_response_init(response, "HTTP/1.1", 421, "Misdirected Request");
}

// called by http_handler_playback_info while preparing response to a GET /playback_info request from the client.

typedef struct time_range_s {
    double start;
    double duration;
} time_range_t;

void time_range_to_plist(void *time_ranges, const int n_time_ranges,
		         plist_t time_ranges_node) {
    time_range_t *tr = (time_range_t *) time_ranges;
    for (int i = 0 ; i < n_time_ranges; i++) {
        plist_t time_range_node = plist_new_dict();
        plist_t duration_node = plist_new_real(tr[i].duration);
        plist_dict_set_item(time_range_node, "duration", duration_node);
        plist_t start_node = plist_new_real(tr[i].start);
        plist_dict_set_item(time_range_node, "start", start_node);
        plist_array_append_item(time_ranges_node, time_range_node);
    }
}

// called by http_handler_playback_info while preparing response to a GET /playback_info request from the client.

int create_playback_info_plist_xml(playback_info_t *playback_info, char **plist_xml) {

    plist_t res_root_node = plist_new_dict();

    plist_t duration_node = plist_new_real(playback_info->duration);
    plist_dict_set_item(res_root_node, "duration", duration_node);

    plist_t position_node = plist_new_real(playback_info->position);
    plist_dict_set_item(res_root_node, "position", position_node);

    plist_t rate_node = plist_new_real(playback_info->rate);
    plist_dict_set_item(res_root_node, "rate", rate_node);

    /* should these be int or bool? */
    plist_t ready_to_play_node = plist_new_uint(playback_info->ready_to_play);
    plist_dict_set_item(res_root_node, "readyToPlay", ready_to_play_node);

    plist_t playback_buffer_empty_node = plist_new_uint(playback_info->playback_buffer_empty);
    plist_dict_set_item(res_root_node, "playbackBufferEmpty", playback_buffer_empty_node);

    plist_t playback_buffer_full_node = plist_new_uint(playback_info->playback_buffer_full);
    plist_dict_set_item(res_root_node, "playbackBufferFull", playback_buffer_full_node);

    plist_t playback_likely_to_keep_up_node = plist_new_uint(playback_info->playback_likely_to_keep_up);
    plist_dict_set_item(res_root_node, "playbackLikelyToKeepUp", playback_likely_to_keep_up_node);

    plist_t loaded_time_ranges_node = plist_new_array();
    time_range_to_plist(playback_info->loadedTimeRanges, playback_info->num_loaded_time_ranges,
                        loaded_time_ranges_node);
    plist_dict_set_item(res_root_node, "loadedTimeRanges", loaded_time_ranges_node);

    plist_t seekable_time_ranges_node = plist_new_array();
    time_range_to_plist(playback_info->seekableTimeRanges, playback_info->num_seekable_time_ranges,
                        seekable_time_ranges_node);
    plist_dict_set_item(res_root_node, "seekableTimeRanges", seekable_time_ranges_node);

    int len = 0;
    plist_to_xml(res_root_node, plist_xml, (uint32_t *) &len);
    /* plist_xml is null-terminated, last character is '/n' */

    plist_free(res_root_node);

    return len;
}

/* this handles requests from the Client  for "Playback information" while the Media is playing on the 
   Media Player.  (The Server gets this information by monitoring the Media Player). The Client could use 
   the information to e.g. update  the slider it shows with progress to the player (0%-100%). 
   It does not affect playing of the Media*/

static void
http_handler_playback_info(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                           char **response_data, int *response_datalen)
{
    if (!http_video_control_is_current(conn, request, response)) return;
    raop_t *raop = conn->raop;
    //const char *session_id = http_request_get_header(request, "X-Apple-Session-ID");
    playback_info_t playback_info;

    playback_info.stallcount = 0;
    //playback_info.playback_buffer_empty = false;   // maybe  need to get this from playbin 
    //playback_info.playback_buffer_full = true;
    //ayback_info.ready_to_play = true; // ???;
    //ayback_info.playback_likely_to_keep_up = true;

    raop->callbacks.on_video_acquire_playback_info(raop->callbacks.cls, &playback_info);
    if (playback_info.duration == -1.0) {
        /* video has finished, reset */
        logger_log(raop->logger, LOGGER_DEBUG, "playback_info not available (finishing)");
        //httpd_remove_known_connections(raop->httpd);
        http_response_set_disconnect(response,1);
        raop->callbacks.video_reset(raop->callbacks.cls, RESET_TYPE_HLS_SHUTDOWN);
        return;
    } else if (playback_info.position == -1.0) {
        logger_log(raop->logger, LOGGER_DEBUG, "playback_info not available");
        return;
    }      

    /* A live stream has no finite total duration. Do not invent a negative
     * loaded range by subtracting its advancing position from zero. */
    playback_info.num_loaded_time_ranges = playback_info.duration > playback_info.position ? 1 : 0;
    time_range_t time_ranges_loaded[1];
    time_ranges_loaded[0].start = playback_info.position;
    time_ranges_loaded[0].duration = playback_info.duration - playback_info.position;
    playback_info.loadedTimeRanges = (void *) &time_ranges_loaded;

    playback_info.num_seekable_time_ranges = 1;
    time_range_t time_ranges_seekable[1];
    time_ranges_seekable[0].start = playback_info.seek_start;
    time_ranges_seekable[0].duration = playback_info.seek_duration;
    playback_info.seekableTimeRanges = (void *) &time_ranges_seekable;

    *response_datalen =  create_playback_info_plist_xml(&playback_info, response_data);
    http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
}

/* this handles the POST /reverse request from Client to Server on a AirPlay http channel to "Upgrade" 
   to "PTTH/1.0" Reverse HTTP protocol proposed in 2009 Internet-Draft 

          https://datatracker.ietf.org/doc/id/draft-lentczner-rhttp-00.txt .  

   After the Upgrade the channel becomes a reverse http "AirPlay (reversed)" channel for
   http requests from Server to Client.
  */

static void
http_handler_reverse(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                     char **response_data, int *response_datalen) {

    raop_t *raop = conn->raop;
    if (raop->hls_scoped_cache) {
        const char *session = http_request_get_header(request, "X-Apple-Session-ID");
        if (!session || strlen(session) != 36) {
            http_response_init(response, "HTTP/1.1", 400, "Bad Request");
            return;
        }
        char *copy = strdup(session);
        if (!copy) {
            http_response_init(response, "HTTP/1.1", 503, "Service Unavailable");
            return;
        }
        free(conn->client_session_id);
        conn->client_session_id = copy;
        conn->reverse_registration_order = ++raop->reverse_registration_order;
    }
    /* get http socket for send */
    int socket_fd = httpd_get_connection_socket (raop->httpd, (void *) conn);
    if (socket_fd < 0) {
        logger_log(raop->logger, LOGGER_ERR, "fcup_request failed to retrieve socket_fd from httpd");
        /* shut down connection? */
    }
    
    const char *purpose = http_request_get_header(request, "X-Apple-Purpose");
    const char *connection = http_request_get_header(request, "Connection");
    const char *upgrade = http_request_get_header(request, "Upgrade");
    logger_log(raop->logger, LOGGER_INFO, "client requested reverse connection: %s; purpose: %s  \"%s\"",
               connection, upgrade, purpose);

    httpd_set_connection_type(raop->httpd, (void *) conn, CONNECTION_TYPE_PTTH);
    int type_PTTH = httpd_count_connection_type(raop->httpd, CONNECTION_TYPE_PTTH);

    if (type_PTTH == 1 || raop->hls_scoped_cache) {
        logger_log(raop->logger, LOGGER_DEBUG, "will use socket %d for %s connections", socket_fd, purpose);
        http_response_init(response, "HTTP/1.1", 101, "Switching Protocols");
        http_response_add_header(response, "Connection", "Upgrade");
        http_response_add_header(response, "Upgrade", "PTTH/1.0");
    } else {
        logger_log(raop->logger, LOGGER_ERR, "multiple TPPH connections (%d) are forbidden", type_PTTH );
    }    
}

/* the POST /action request from Client to Server on the AirPlay http channel follows a POST /event "FCUP Request"
 from Server to Client on the reverse http channel, for a HLS playlist (first the Master Playlist, then the Media Playlists
 listed in the Master Playlist.     The POST /action request contains the playlist requested by the Server in
 the preceding "FCUP Request".   The FCUP Request sequence  continues until all Media Playlists have been obtained by the Server */ 

static void
http_handler_action(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                    char **response_data, int *response_datalen) {

    raop_t *raop = conn->raop;
    airplay_video_t *airplay_video = (airplay_video_t *) hls_get_current_video(raop);
    if (!airplay_video) {
        http_response_init(response, "HTTP/1.1", 409, "Conflict");
        return;
    }
    if (!http_video_control_is_current(conn, request, response)) return;
    bool data_is_plist = false;
    bool request_current = false;
    plist_t req_root_node = NULL;
    uint64_t uint_val = 0;
    uint64_t request_id = 0;
    bool have_request_id = false;
    int fcup_response_statuscode = 0;
    char *type = NULL;
    bool logger_debug = (logger_get_level(raop->logger) >= LOGGER_DEBUG);

    const char* session_id = http_request_get_header(request, "X-Apple-Session-ID");
    if (!session_id) {
        logger_log(raop->logger, LOGGER_ERR, "Play request had no X-Apple-Session-ID");
        goto post_action_error;
    }    
    const char *apple_session_id = get_apple_session_id(airplay_video);
    if (strcmp(session_id, apple_session_id)){
        logger_log(raop->logger, LOGGER_ERR, "X-Apple-Session-ID has changed:\n  was:\"%s\"\n  now:\"%s\"",
                   apple_session_id, session_id);
        goto post_action_error;
    }
    request_current = !raop->hls_scoped_cache;

    /* verify that this request contains a binary plist*/
    char *header_str = NULL;
    http_request_get_header_string(request, &header_str);
    logger_log(raop->logger, LOGGER_DEBUG, "request header: %s", header_str);
    data_is_plist = (strstr(header_str,"apple-binary-plist") != NULL);
    free(header_str);
    if (!data_is_plist) {
        logger_log(raop->logger, LOGGER_INFO, "POST /action: did not receive expected plist from client");	
        goto post_action_error;
    }

    /* extract the root_node  plist */
    int request_datalen = 0;
    const char *request_data = http_request_get_data(request, &request_datalen);
    if (request_datalen == 0) {
        logger_log(raop->logger, LOGGER_INFO, "POST /action: did not receive expected plist from client");	
        goto post_action_error;
    }
    plist_from_bin(request_data, request_datalen, &req_root_node);

    /* determine type of data */
    plist_t req_type_node = plist_dict_get_item(req_root_node, "type");
    if (!PLIST_IS_STRING(req_type_node)) {
        goto post_action_error;
    }
    
    /* three possible types are known: 
       playlistRemove
       playlistAdd
       unhandledURLRespone
*/
    plist_get_string_val(req_type_node, &type);
    if (!type) {
        goto post_action_error;
    }
    logger_log(raop->logger, LOGGER_DEBUG, "action type is %s", type);
    /* check that plist structure is as expected*/
    plist_t req_params_node = NULL;
    if (PLIST_IS_DICT (req_root_node)) {
        req_params_node = plist_dict_get_item(req_root_node, "params");
    }
    if (!PLIST_IS_DICT (req_params_node)) {
        goto post_action_error;
    }
    if (!strcmp(type,"playlistRemove")) {
        plist_t req_params_item_node = plist_dict_get_item(req_params_node, "item");
        if (!req_params_item_node || !PLIST_IS_DICT (req_params_item_node)) {
            goto post_action_error;
        }
        plist_t req_params_item_uuid_node = plist_dict_get_item(req_params_item_node, "uuid");
        char* remove_uuid = NULL;
        plist_get_string_val(req_params_item_uuid_node, &remove_uuid);
        assert(remove_uuid);
        int id  =  get_playlist_by_uuid(raop, remove_uuid);
        if (id == raop->current_video) {
            raop->current_video = -1;
            float position = raop->callbacks.on_video_playlist_remove(raop->callbacks.cls);
            /* keep the playlist (until space is needed for another one) in case its playback_uuid is re-requested
               the video will then be restarted at its previous position */
            set_resume_position_seconds(airplay_video, position);
        } else {
            logger_log(raop->logger, LOGGER_WARNING, "playlistRemove uuid %s does not match current_video\n", remove_uuid);
        }
        plist_mem_free (remove_uuid);

    } else if (!strcmp(type, "playlistInsert")) {
        logger_log(raop->logger, LOGGER_INFO, "action type playlistInsert (start playback)");
        plist_t req_params_item_node = plist_dict_get_item(req_params_node, "item");
        if (!req_params_item_node || !PLIST_IS_DICT (req_params_item_node)) {
            goto post_action_error;
        }
        plist_t req_params_item_uuid_node = plist_dict_get_item(req_params_item_node, "uuid");
        char* remove_uuid = NULL;
        plist_get_string_val(req_params_item_uuid_node, &remove_uuid);
        if (remove_uuid) {
            int id  =  get_playlist_by_uuid(raop, remove_uuid);
            if (id >= 0) {
                logger_log(raop->logger, LOGGER_INFO, "playlistInsert uuid %s is stored at airplay_video[%d]", remove_uuid, id);
            } else {
                logger_log(raop->logger, LOGGER_INFO, "playlistInsert uuid %s is not a stored playlist", remove_uuid);
            }
            plist_mem_free(remove_uuid);
            char *plist_xml = NULL;
            uint32_t plist_len = 0;
            plist_to_xml(req_params_item_node, &plist_xml, &plist_len);
            printf("playlistInsert parameter item list is:\n%s", plist_xml);
            plist_mem_free(plist_xml);
        }
        logger_log(raop->logger, LOGGER_ERR, "FIXME: playlistInsert is not yet implemented");

    } else if (!strcmp(type, "unhandledURLResponse")) {   
        /* handling type "unhandledURLResponse" */
        if (airplay_video_is_ready(airplay_video)) {
            /* A delayed duplicate final reply must not restart playback. */
            goto post_action_done;
        }
        uint_val = 0;
        int fcup_response_datalen = 0;

        {
            plist_t req_params_fcup_response_statuscode_node = plist_dict_get_item(req_params_node,
                                                                      "FCUP_Response_StatusCode");
            if (req_params_fcup_response_statuscode_node) {
                plist_get_uint_val(req_params_fcup_response_statuscode_node, &uint_val);
                fcup_response_statuscode = (int) uint_val;
                uint_val = 0;
                logger_log(raop->logger, LOGGER_DEBUG, "FCUP_Response_StatusCode = %d",
                           fcup_response_statuscode);
            }

            plist_t req_params_fcup_response_requestid_node = plist_dict_get_item(req_params_node,
                                                                     "FCUP_Response_RequestID");
            if (PLIST_IS_UINT(req_params_fcup_response_requestid_node)) {
                plist_get_uint_val(req_params_fcup_response_requestid_node, &request_id);
                have_request_id = true;
                logger_log(raop->logger, LOGGER_DEBUG, "FCUP_Response_RequestID =  %" PRIu64, request_id);
            }
        }
        if ((raop->hls_scoped_cache && !have_request_id) ||
            (have_request_id && request_id != (uint64_t) get_current_FCUP_RequestID(airplay_video))) {
            /* Consecutive entries can share a URL. An older failed response
             * must not consume the later request for that same URL. */
            logger_log(raop->logger, LOGGER_DEBUG,
                       "Direct playback: ignoring superseded playlist response ID");
            goto post_action_done;
        }

        plist_t req_params_fcup_response_url_node = plist_dict_get_item(req_params_node, "FCUP_Response_URL");
        if (!PLIST_IS_STRING(req_params_fcup_response_url_node)) {
            goto post_action_error;
        }
        char *fcup_response_url = NULL;
        plist_get_string_val(req_params_fcup_response_url_node, &fcup_response_url);
        if (!fcup_response_url) {
            goto post_action_error;
        }
        logger_log(raop->logger, LOGGER_DEBUG, "FCUP_Response_URL =  %s", fcup_response_url);

        /* Responses may arrive after a video was stopped or replaced. Match
         * the outstanding URL before advancing the sequential download queue;
         * otherwise an old reply can populate a different playlist's slot. */
        int next_uri = get_next_media_uri_id(airplay_video);
        const char *expected_uri = next_uri > 0 ?
            get_media_uri_by_num(airplay_video, next_uri - 1) : NULL;
        const char *prefix = get_uri_prefix(airplay_video);
        bool expected_master = next_uri == 0 && prefix &&
            !strncmp(fcup_response_url, prefix, strlen(prefix)) &&
            !strcmp(fcup_response_url + strlen(prefix), "/master.m3u8");
        if (!expected_master && (!expected_uri || strcmp(expected_uri, fcup_response_url))) {
            logger_log(raop->logger, LOGGER_INFO,
                       "Direct playback: ignoring a playlist response for a superseded request");
            plist_mem_free(fcup_response_url);
            goto post_action_done;
        }
        request_current = true;
        if (fcup_response_statuscode && fcup_response_statuscode != 200) {
            logger_log(raop->logger, LOGGER_WARNING,
                       "Direct playback: %s playlist download failed (HTTP %d)",
                       expected_master ? "master" : "media", fcup_response_statuscode);
            plist_mem_free(fcup_response_url);
            if (expected_master) goto post_action_error;
            goto next_fcup_request;
        }
	
        plist_t req_params_fcup_response_data_node = plist_dict_get_item(req_params_node, "FCUP_Response_Data");
        if (!PLIST_IS_DATA(req_params_fcup_response_data_node)){
            plist_mem_free(fcup_response_url);
            if (expected_master) goto post_action_error;
            logger_log(raop->logger, LOGGER_WARNING,
                       "Direct playback: skipping media playlist with no response data");
            goto next_fcup_request;
        }

        uint_val = 0;
#ifdef PLIST_210
        const char *fcup_response_data = NULL;
        fcup_response_data = plist_get_data_ptr(req_params_fcup_response_data_node, &uint_val);
#else
        char *fcup_response_data = NULL;       
        fcup_response_data = plist_get_data_val(req_params_fcup_response_data_node, &fcup_response_data, &uint_val);
#endif
        fcup_response_datalen = (int) uint_val;
        char *playlist = NULL;
        if (!fcup_response_data) {
            plist_mem_free(fcup_response_url);
            if (expected_master) goto post_action_error;
            logger_log(raop->logger, LOGGER_WARNING,
                       "Direct playback: skipping empty media playlist response");
            goto next_fcup_request;
        } else {
            playlist = (char *) malloc(fcup_response_datalen + 1);
            if (!playlist) {
                printf("Memory allocation failed (playlist)\n");
                exit(1);
            }
            playlist[fcup_response_datalen] = '\0';
            memcpy(playlist, fcup_response_data, fcup_response_datalen);
#ifndef PLIST_210
            plist_mem_free(fcup_response_data);
#endif
        }
        assert(playlist);
        int playlist_len = strlen(playlist);
        if (strncmp(playlist, "#EXTM3U", 7)) {
            logger_log(raop->logger, LOGGER_ERR, "Direct playback: invalid playlist response");
            free(playlist);
            plist_mem_free(fcup_response_url);
            if (expected_master) goto post_action_error;
            goto next_fcup_request;
        }
    
        if (logger_debug) {
            logger_log(raop->logger, LOGGER_DEBUG, "begin FCUP Response data:\n%s\nend FCUP Response data", playlist);
        }

        if (expected_master) {
            /* this is a master playlist */
            const char *uri_prefix = get_uri_prefix(airplay_video);
            char ** uri_list = NULL;
            int num_uri = 0;
            char *uri_local_prefix = get_uri_local_prefix(airplay_video);
            playlist = select_master_playlist_language(airplay_video, playlist);
            playlist_len = strlen(playlist);
            int table_result = create_media_uri_table(uri_prefix, playlist, playlist_len, &uri_list, &num_uri);
            if (table_result || num_uri <= 0) {
                logger_log(raop->logger, LOGGER_ERR,
                           "Direct playback: master playlist contains no supported media routes");
                for (int i = 0; i < num_uri; i++) free(uri_list[i]);
                free(uri_list);
                free(playlist);
                plist_mem_free(fcup_response_url);
                goto post_action_error;
            }
            char *new_master = adjust_master_playlist (playlist, playlist_len,  uri_prefix, uri_local_prefix);
            free(playlist);
            store_master_playlist(airplay_video, new_master);
            create_media_data_store(airplay_video, uri_list, num_uri);
            free (uri_list);
            num_uri =  get_num_media_uri(airplay_video);
            set_next_media_uri_id(airplay_video, 0);
        } else {
            /* this is a media playlist */
            float duration = 0.0f;
            bool endlist = false;
            int count = analyze_media_playlist(playlist, &duration, &endlist);
            int uri_num = get_next_media_uri_id(airplay_video);
            --uri_num;    // (next num is current num + 1)
            int ret = store_media_playlist(airplay_video, playlist, &count, &duration, &endlist, uri_num);
            if (ret < 0) {
                free(playlist);
                plist_mem_free(fcup_response_url);
                goto post_action_error;
            }
            if (ret == 1) {
                logger_log(raop->logger, LOGGER_DEBUG,"media_playlist is a duplicate: do not store");
            } else if (count) {
                logger_log(raop->logger, LOGGER_DEBUG,
                           "\n%s:\nreceived media playlist has %5d chunks, total duration %9.3f secs\n",
                            fcup_response_url, count, duration);
            }
        }

        plist_mem_free(fcup_response_url);

 next_fcup_request:;
        int num_uri = get_num_media_uri(airplay_video);
        int uri_num = get_next_media_uri_id(airplay_video);
        if (uri_num <  num_uri) {
            int sent = fcup_request((void *) conn, get_media_uri_by_num(airplay_video, uri_num),
                                                             apple_session_id,
                                                             get_next_FCUP_RequestID(airplay_video));
            if (sent < 0) goto post_action_error;
            set_next_media_uri_id(airplay_video, ++uri_num);
        } else {
            if (!airplay_video_finalize_cache_profile(airplay_video, raop->hls_pi4)) {
                logger_log(raop->logger, LOGGER_ERR,
                           "Direct playback: no playable variants in the downloaded playlist cache%s",
                           raop->hls_pi4 ? " matching the Raspberry Pi 4 H.264/AAC-LC profile" : "");
                goto post_action_error;
            }
            logger_log(raop->logger, LOGGER_INFO,
                       "Direct playback: playlist cache ready (%d available entries)",
                       get_num_media_uri(airplay_video));
            raop->callbacks.on_video_play(raop->callbacks.cls,
                                                get_playback_location(airplay_video),
                                                get_start_position_seconds(airplay_video), false);
        }


    } else {
        logger_log(raop->logger, LOGGER_INFO, "unknown action type (unhandled)"); 
    }
 post_action_done:;
    plist_mem_free(type);
    plist_free(req_root_node);
    return;

 post_action_error:;
    if (request_current && !airplay_video_is_ready(airplay_video) && raop->callbacks.on_video_request_error)
        raop->callbacks.on_video_request_error(raop->callbacks.cls);
    http_response_init(response, "HTTP/1.1", 400, "Bad Request");
    plist_mem_free(type);
    if (req_root_node)  {
        plist_free(req_root_node);
    }

}

/* The POST /play request from the Client to Server on the AirPlay http channel contains (among other information)
   the "Content Location" that specifies the HLS Playlists for the video to be streamed, as well as the video 
   "start position in seconds".   Once this request is received by the Sever, the Server sends a POST /event
   "FCUP Request" request to the Client on the reverse http channel, to request the HLS Master Playlist */

static void
http_handler_play(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen) {
    raop_t *raop = conn->raop;
    char* playback_location = NULL;
    char* playback_uuid = NULL;
    char* client_proc_name = NULL;
    const char *uri_suffix = NULL;
    bool direct_http = false;
    bool request_announced = false;
    plist_t req_root_node = NULL;
    float start_position_seconds = 0.0f;
    bool data_is_binary_plist = false;
    char supported_hls_proc_names[] = "YouTube;";
    airplay_video_t *airplay_video = NULL;
    
    logger_log(raop->logger, LOGGER_DEBUG, "http_handler_play");

    const char* apple_session_id = http_request_get_header(request, "X-Apple-Session-ID");
    if (!apple_session_id || strlen(apple_session_id) != 36) {
        logger_log(raop->logger, LOGGER_ERR, "Play request had no X-Apple-Session-ID");
        goto play_error;
    }

    int request_datalen = -1;    
    const char *request_data = http_request_get_data(request, &request_datalen);

    if (request_datalen > 0) {
        char *header_str = NULL;
        http_request_get_header_string(request, &header_str);
        logger_log(raop->logger, LOGGER_DEBUG, "request header:\n%s", header_str);
        data_is_binary_plist = (strstr(header_str, "x-apple-binary-plist") != NULL);
        free (header_str);
    }

    if (!data_is_binary_plist) {
         logger_log(raop->logger, LOGGER_ERR, "Play request Content is not binary_plist (unsupported)");
         goto play_error;
    }

    plist_from_bin(request_data, request_datalen, &req_root_node);

    /* Keep the incoming-request timestamp separate from renderer startup.
     * Sender names, request bodies and locations are untrusted/private. */
    if (!PLIST_IS_DICT(req_root_node)) goto play_error;
    char *diagnostic_sender = NULL;
    plist_t diagnostic_sender_node = plist_dict_get_item(req_root_node, "clientProcName");
    if (PLIST_IS_STRING(diagnostic_sender_node)) plist_get_string_val(diagnostic_sender_node, &diagnostic_sender);
    logger_log(raop->logger, LOGGER_INFO, "AirPlay video request: sender=%s",
        diagnostic_sender && !strcmp(diagnostic_sender, "UHF") ? "UHF" :
        diagnostic_sender && !strcmp(diagnostic_sender, "YouTube") ? "YouTube" : "other");
    plist_mem_free(diagnostic_sender);

    plist_t req_uuid_node = plist_dict_get_item(req_root_node, "uuid");
    if (!PLIST_IS_STRING(req_uuid_node)) {
       goto play_error;
    }
    plist_get_string_val(req_uuid_node, &playback_uuid);
    if (!playback_uuid || strlen(playback_uuid) != 36) goto play_error;
    plist_t req_content_location_node = plist_dict_get_item(req_root_node, "Content-Location");
    if (!PLIST_IS_STRING(req_content_location_node)) goto play_error;
    plist_get_string_val(req_content_location_node, &playback_location);
    if (!playback_location || !*playback_location) goto play_error;
    direct_http = !strncmp(playback_location, "http://", 7) || !strncmp(playback_location, "https://", 8);
    uri_suffix = strstr(playback_location, "/master.m3u8");
    if (!direct_http && !uri_suffix) goto play_error;
    plist_t req_start_position_seconds_node = plist_dict_get_item(req_root_node, "Start-Position-Seconds");
    if (req_start_position_seconds_node) {
        double start_position = 0.0;
        if (PLIST_IS_REAL(req_start_position_seconds_node))
            plist_get_real_val(req_start_position_seconds_node, &start_position);
        else if (PLIST_IS_UINT(req_start_position_seconds_node)) {
            uint64_t whole_seconds = 0;
            plist_get_uint_val(req_start_position_seconds_node, &whole_seconds);
            start_position = (double)whole_seconds;
        } else goto play_error;
        if (!isfinite(start_position) || start_position < 0 || start_position > FLT_MAX) goto play_error;
        start_position_seconds = (float)start_position;
    }

    /* A valid request is now accepted, before cache replacement or FCUP work.
     * Invalid traffic cannot create a new on-screen session or fail the old one. */
    if (raop->callbacks.on_video_request) raop->callbacks.on_video_request(raop->callbacks.cls, direct_http);
    request_announced = true;

#if 0
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        printf("old: airplay_video[%d] %p %s %f\n", i, raop->airplay_video[i],
	       get_playback_uuid(raop->airplay_video[i]),
	       get_duration(raop->airplay_video[i]));
    }
    printf("\n");
    printf("new playback_uuid %s\n\n", playback_uuid);
#endif

    int id = -1;
    id = get_playlist_by_uuid(raop, playback_uuid);

    if (id >= 0 && !airplay_video_is_ready(raop->airplay_video[id])) {
        logger_log(raop->logger, LOGGER_INFO,
                   "Direct playback: discarding incomplete playlist cache; fetching it again");
        raop_destroy_airplay_video(raop, id);
        id = -1;
    }

    /* check if playlist is already downloaded and stored (may have been interrupted by advertisements ) */
    if (id >= 0) {
      //printf("====use EXISTING  airplay_video[%d] %p %s %s\n", id, raop->airplay_video[id], playback_uuid, get_playback_uuid(raop->airplay_video[id]));
        plist_mem_free(playback_uuid);
        plist_mem_free(playback_location);
        plist_free(req_root_node);
        raop->current_video = id;
        airplay_video = hls_get_current_video(raop);
        assert(airplay_video);
        set_apple_session_id(airplay_video, apple_session_id, strlen(apple_session_id));
        float resume_pos = get_resume_position_seconds(airplay_video);
        float start_pos = get_start_position_seconds(airplay_video);
	//printf("========= %f ============call on_video_play===== %f ==========\n", start_pos, resume_pos);
        raop->callbacks.on_video_play(raop->callbacks.cls,
                                      get_playback_location(airplay_video),
                                      resume_pos > start_pos ? resume_pos : start_pos, direct_http);
        return;
    }
    
    /* initialize a new playlist (airplay_video structure) */
    /* first delete any short stored playlists (probably advertisements */
    int count = 0;
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        if (raop->airplay_video[i]) {
            float duration = get_duration(raop->airplay_video[i]); 
            if (duration < (float) MIN_STORED_AIRPLAY_VIDEO_DURATION_SECONDS ) { //likely to be an advertisement
                logger_log(raop->logger, LOGGER_INFO,
                          "deleting playlist playback_uuid %s duration (seconds) %f",
                           get_playback_uuid(raop->airplay_video[i]), duration);
                raop_destroy_airplay_video(raop, i);
            } else {
                count++;
            }
        }
    }

    assert (count < MAX_AIRPLAY_VIDEO);
    assert(id == -1);
    /* initialize new airplay_video structure to hold playlist */
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        if (raop->airplay_video[i]) {
            continue;
        }
        id = i;
        break;
    }
    assert(id >= 0);

    raop->current_video = id;
    raop->airplay_video[id] = airplay_video_init(raop, raop->port, raop->lang);
    if (!raop->airplay_video[id]) { raop->current_video = -1; goto play_error; }
    if (raop->hls_scoped_cache && !airplay_video_enable_scoped_cache(raop->airplay_video[id])) {
        raop_destroy_airplay_video(raop, id);
        goto play_error;
    }
    airplay_video = hls_get_current_video(raop);
    assert(airplay_video);
    set_apple_session_id(airplay_video, apple_session_id, strlen(apple_session_id));
    set_playback_uuid(airplay_video, playback_uuid, strlen(playback_uuid));
    plist_mem_free (playback_uuid);
    playback_uuid = NULL;
    count++;

    /* ensure that space will always be available for adding future playlists */

    if (count == MAX_AIRPLAY_VIDEO) {
        int next = (id + 1) % (int) MAX_AIRPLAY_VIDEO;
        logger_log(raop->logger, LOGGER_INFO,
                   "deleting playlist playback_uuid %s duration (seconds) %f",
                   get_playback_uuid(raop->airplay_video[next]),
                   get_duration(raop->airplay_video[next]));
        airplay_video_destroy(raop->airplay_video[next]);
        raop->airplay_video[next] = NULL;
    }
#if 0    
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        printf("new: airplay_video[%d] %p %s %f\n", i, raop->airplay_video[i],
	       get_playback_uuid(raop->airplay_video[i]),
	       get_duration(raop->airplay_video[i]));
    }
#endif
	   
    set_start_position_seconds(airplay_video, (float) start_position_seconds);

    /* we now also support playing video from direct Content-Location sources  (such as Safari on iOS/macOS via airplay button)) */ 
    if (!strncmp(playback_location, "http://", strlen("http://")) ||
        !strncmp(playback_location, "https://", strlen("https://"))) {
        logger_log(raop->logger, LOGGER_INFO, "AirPlay video request: route=direct-http");
        set_playback_location(airplay_video, playback_location, strlen(playback_location));
        raop->callbacks.on_video_play(raop->callbacks.cls,
                                      get_playback_location(airplay_video),
                                      start_position_seconds, true);
    } else if (uri_suffix) {
        logger_log(raop->logger, LOGGER_INFO, "AirPlay video request: route=playlist-cache");
        plist_t req_client_proc_name_node = plist_dict_get_item(req_root_node, "clientProcName");
        if (req_client_proc_name_node) {
            plist_get_string_val(req_client_proc_name_node, &client_proc_name);
            if (!strstr(supported_hls_proc_names, client_proc_name)){
                logger_log(raop->logger, LOGGER_WARNING, "Unsupported m3u8 HLS streaming format: clientProcName %s not found in supported list: %s",
                           client_proc_name, supported_hls_proc_names);
            }
            plist_mem_free(client_proc_name);
        }
        size_t len = strlen(get_uri_local_prefix(airplay_video)) + strlen(uri_suffix);
        char *location = (char *) calloc(len + 1, sizeof(char));
        if (!location) {
            printf("Memory allocation failed (location)\n");
            exit(1);
        }
        strcat(location, get_uri_local_prefix(airplay_video));
        strcat(location, uri_suffix);
        set_playback_location(airplay_video, location, strlen(location));
        free(location);
        char *uri_prefix = (char *) calloc(strlen(playback_location) + 1, sizeof(char));
        if (!playback_location) {
            printf("Memeory allocation failed (playback_location)\n");
            exit(1);
        }
        strcat(uri_prefix, playback_location);
        char *end = strstr(uri_prefix, "/master.m3u8");
        *end = '\0';
        set_uri_prefix(airplay_video, uri_prefix, strlen(uri_prefix));
        free (uri_prefix);
        set_next_media_uri_id(airplay_video, 0);
        if (fcup_request((void *) conn, playback_location, apple_session_id,
                         get_next_FCUP_RequestID(airplay_video)) < 0) goto play_error;
    } else {
        logger_log(raop->logger, LOGGER_ERR, "Content-Location has unsupported form");
        goto play_error;
    }

    plist_mem_free(playback_location);

    if (req_root_node) {
        plist_free(req_root_node);
    }
    return;

 play_error:;
    if (request_announced && raop->callbacks.on_video_request_error)
        raop->callbacks.on_video_request_error(raop->callbacks.cls);
    plist_mem_free(playback_uuid);
    plist_mem_free(playback_location);
    if (req_root_node) {
        plist_free(req_root_node);
    }
    logger_log(raop->logger, LOGGER_ERR, "Could not find valid Plist Data for POST/play request, Unhandled");
    http_response_init(response, "HTTP/1.1", 400, "Bad Request");
    http_response_set_disconnect(response, 1);
    raop->callbacks.conn_reset(raop->callbacks.cls, 2);
}

/* the HLS handler handles http requests GET /[uri] on the HLS channel from the media player to the Server, asking for
   (adjusted) copies of Playlists: first the Master Playlist  (adjusted to change the uri prefix to
   "http://localhost:[port]/.......m3u8"), then the Media Playlists that the media player wishes to use.  
   If the client supplied Media playlists with the "YT-EXT-CONDENSED-URI" header, these must be adjusted into
   the standard uncondensed form before sending with the response.    The uri in the request is  the uri for the
   Media Playlist, taken from the Master Playlist, with the uri prefix removed.  
*/ 

static void
http_handler_hls(raop_conn_t *conn,  http_request_t *request, http_response_t *response,
                 char **response_data, int *response_datalen) {
    raop_t *raop = conn->raop;
    if (!raop->hls_scoped_cache && raop->current_video == -1) {
        logger_log(raop->logger, LOGGER_ERR,"airplay_video playlist  not found");
        http_response_init(response, "HTTP/1.1", 404, "Not Found");
        return;
    }
    const char *method = http_request_get_method(request);
    if (!method || strcmp(method, "GET")) {
        http_response_init(response, "HTTP/1.1", 405, "Method Not Allowed");
        return;
    }
    const char *url = http_request_get_url(request);
    airplay_video_t *airplay_video = NULL;
    if (raop->hls_scoped_cache) {
        if (!http_video_peer_is_loopback(conn)) {
            http_response_init(response, "HTTP/1.1", 403, "Forbidden");
            return;
        }
        /* Paths are literal, cache-lifetime names. No decoding, traversal or
         * fallback to current_video is permitted in this mode. */
        if (!url || strncmp(url, "/cache/", 7) || strlen(url) < 40 || url[39] != '/') {
            http_response_init(response, "HTTP/1.1", 404, "Not Found");
            return;
        }
        for (unsigned i = 7; i < 39; i++) {
            if (!((url[i] >= '0' && url[i] <= '9') || (url[i] >= 'a' && url[i] <= 'f'))) {
                http_response_init(response, "HTTP/1.1", 404, "Not Found");
                return;
            }
        }
        for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
            const char *id = airplay_video_get_cache_id(raop->airplay_video[i]);
            if (id && !strncmp(id, url + 7, 32)) { airplay_video = raop->airplay_video[i]; break; }
        }
        if (!airplay_video) {
            http_response_init(response, "HTTP/1.1", 404, "Not Found");
            return;
        }
        url += 39; /* Existing helpers take /master.m3u8 or a rendition path. */
    } else airplay_video = (airplay_video_t *) hls_get_current_video(raop);
    if (!airplay_video) {
        http_response_init(response, "HTTP/1.1", 404, "Not Found");
        return;
    }
    const char* upgrade = http_request_get_header(request, "Upgrade");
    if (upgrade) {
        //don't accept Upgrade: h2c request ?
        logger_log(raop->logger, LOGGER_INFO, "HLS cache: upgrade request declined");
        return;
    }
    if (!strcmp(url, "/master.m3u8")){
        char * master_playlist  = get_master_playlist(airplay_video);
        if (master_playlist) {
            size_t len = strlen(master_playlist);
            char * data = (char *) malloc(len + 1);
            if (!data) {
                printf("Memory allocation failed (data)\n");
                exit(1);
            }
            memcpy(data, master_playlist, len);
            data[len] = '\0';
            *response_data = data;
            *response_datalen = (int ) len;
        } else {
            logger_log(raop->logger, LOGGER_ERR,"HLS cache: requested master playlist not found");
        }

    } else {
        int chunks = 0;
        float duration = 0.0f;
        char *media_playlist = get_media_playlist(airplay_video, &chunks, &duration, url);
        if (media_playlist) {
            char *data  = adjust_yt_condensed_playlist(media_playlist);
            *response_data = data;
            *response_datalen = strlen(data);
            logger_log(raop->logger, LOGGER_INFO,
                       "HLS cache: media playlist served; chunks=%d duration=%.3f", chunks, duration);
        } else {
            logger_log(raop->logger, LOGGER_ERR,"HLS cache: requested media playlist not found");
        }
	    
    }

    http_response_add_header(response, "Access-Control-Allow-Headers", "Content-type");
    http_response_add_header(response, "Access-Control-Allow-Origin", "*");
    const char *date = NULL;
    date = gmt_time_string();
    http_response_add_header(response, "Date", date);
    if (*response_datalen > 0) {
        http_response_add_header(response, "Content-Type", "application/x-mpegURL; charset=utf-8");
    } else if (*response_datalen == 0) {
        http_response_init(response, "HTTP/1.1", 404, "Not Found");
    }
}
