/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "http_response.h"
#include "compat.h"

struct http_response_s {
    int complete;
    int disconnect;
    int empty_body_length;
    int length_forbidden;
    int has_content_length;
    int has_content_type;

    char *data;
    int buffer_size;
    int data_length;
};


static void
http_response_add_data(http_response_t *response, const char *data, int datalen)
{
    assert(response);
    assert(data);
    assert(datalen > 0);

    size_t newbufsize = response->buffer_size;
    while (response->data_length + datalen > newbufsize) {
        newbufsize *= 2;
    }
    if (newbufsize != response->buffer_size) {
        response->data = realloc(response->data, newbufsize);
        assert(response->data);
        response->buffer_size = newbufsize;
    }
    memcpy(response->data+response->data_length, data, datalen);
    response->data_length += datalen;
}


http_response_t *
http_response_create()
{
    http_response_t *response =  (http_response_t *) calloc(1, sizeof(http_response_t));
    if (!response) {
        return NULL;
    }
    /* Allocate response data */
    response->buffer_size = 1024;
    response->data = (char *) malloc(response->buffer_size);
    if (!response->data) {
        free(response);
        return NULL;
    }
    return response;
}

void
http_response_init(http_response_t *response, const char *protocol, int code, const char *message)
{
    assert(response);
    response->data_length = 0;    /* can be used to reinitialize a previously-initialized response */
    response->complete = response->disconnect = 0;
    response->has_content_length = response->has_content_type = 0;
    int http = !strncmp(protocol, "HTTP/", 5);
    response->length_forbidden = http && (code < 200 || code == 204 || code == 304);
    response->empty_body_length = http && !response->length_forbidden;
    char codestr[4] = {0};

    assert(code >= 100 && code < 1000);

    /* Convert code into string */
    memset(codestr, 0, sizeof(codestr));
    snprintf(codestr, sizeof(codestr), "%u", code);

    /* Add first line of response to the data array */
    http_response_add_data(response, protocol, strlen(protocol));
    http_response_add_data(response, " ", 1);
    http_response_add_data(response, codestr, strlen(codestr));
    http_response_add_data(response, " ", 1);
    http_response_add_data(response, message, strlen(message));
    http_response_add_data(response, "\r\n", 2);
}

void
http_response_reverse_request_init(http_response_t *request, const char *method, const char *url, const char *protocol)
{
    assert(request);
    request->complete = request->disconnect = 0;
    request->has_content_length = request->has_content_type = 0;
    request->empty_body_length = request->length_forbidden = 0;
    request->data_length = 0;  /* reinitialize a previously-initialized response as a reverse-HTTP (PTTH/1.0) request */

    /* Add first line of response to the data array */
    http_response_add_data(request, method, strlen(method));
    http_response_add_data(request, " ", 1);
    http_response_add_data(request, url, strlen(url));
    http_response_add_data(request, " ", 1);
    http_response_add_data(request, protocol, strlen(protocol));
    http_response_add_data(request, "\r\n", 2);
}

void
http_response_destroy(http_response_t *response)
{
    if (response) {
        free(response->data);
        free(response);
    }
}

void
http_response_add_header(http_response_t *response, const char *name, const char *value)
{
    assert(response);
    assert(name);
    assert(value);

    /* Header names are case insensitive. Do not duplicate explicit lengths. */
    char lower[20];
    size_t length = strlen(name);
    if (length < sizeof(lower)) {
        for (size_t i = 0; i <= length; i++) lower[i] = tolower((unsigned char) name[i]);
        if (!strcmp(lower, "content-length")) response->has_content_length = 1;
        if (!strcmp(lower, "content-type")) response->has_content_type = 1;
    }

    http_response_add_data(response, name, strlen(name));
    http_response_add_data(response, ": ", 2);
    http_response_add_data(response, value, strlen(value));
    http_response_add_data(response, "\r\n", 2);
}

void
http_response_finish(http_response_t *response, const char *data, int datalen)
{
    assert(response);
    assert(datalen==0 || (data && datalen > 0));

    if (data && datalen > 0) {
        const char *hdrname = "Content-Length";
        char hdrvalue[16] = {0};

        memset(hdrvalue, 0, sizeof(hdrvalue));
        snprintf(hdrvalue, sizeof(hdrvalue)-1, "%d", datalen);

        /* Add Content-Length header first */
        if (!response->has_content_length) http_response_add_header(response, hdrname, hdrvalue);
        http_response_add_data(response, "\r\n", 2);

        /* Add data to the end of response */
        http_response_add_data(response, data, datalen);
    } else {
        /* An ordinary HTTP response without a length is delimited by closing
         * the connection. AirPlay keeps control connections open, so even an
         * empty /play, /rate or /stop reply needs a zero length. Upgrades and
         * statuses with no body keep their protocol-defined framing. */
        if (!response->has_content_length && !response->length_forbidden &&
            (response->empty_body_length || response->has_content_type))
            http_response_add_header(response, "Content-Length", "0");
        /* Add extra end of line after headers */
        http_response_add_data(response, "\r\n", 2);
    }
    response->complete = 1;
}

void
http_response_set_disconnect(http_response_t *response, int disconnect)
{
    assert(response);

    response->disconnect = !!disconnect;
}

int
http_response_get_disconnect(http_response_t *response)
{
    assert(response);

    return response->disconnect;
}

const char *
http_response_get_data(http_response_t *response, int *datalen)
{
    assert(response);
    assert(datalen);
    assert(response->complete);

    *datalen = response->data_length;
    return response->data;
}
