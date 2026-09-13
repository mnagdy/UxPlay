/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../lib/http_response.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *serialized(http_response_t *r) {
    int size;
    const char *bytes = http_response_get_data(r, &size);
    char *copy = malloc(size + 1);
    assert(copy);
    memcpy(copy, bytes, size);
    copy[size] = 0;
    return copy;
}

int main(int argc, char **argv) {
    http_response_t *r = http_response_create();
    assert(r);
    http_response_init(r, "HTTP/1.1", 200, "OK");
    if (argc == 2) {
        int body = !strcmp(argv[1], "body");
        http_response_finish(r, body ? "OK" : NULL, body ? 2 : 0);
        int size;
        const char *bytes = http_response_get_data(r, &size);
        assert(fwrite(bytes, 1, size, stdout) == (size_t)size);
        http_response_destroy(r);
        return 0;
    }
    http_response_finish(r, NULL, 0);
    char *text = serialized(r);
    assert(!strcmp(text, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"));
    free(text);
    http_response_set_disconnect(r, 1);
    http_response_init(r, "HTTP/1.1", 400, "Bad Request");
    assert(!http_response_get_disconnect(r));
    http_response_add_header(r, "cOnTeNt-LeNgTh", "0");
    http_response_add_header(r, "Content-Type", "application/octet-stream");
    http_response_finish(r, NULL, 0);
    text = serialized(r);
    assert(strstr(text, "cOnTeNt-LeNgTh: 0\r\n"));
    assert(!strstr(text, "Content-Length:"));
    free(text);
    int statuses[] = {101, 204, 304};
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        http_response_init(r, "HTTP/1.1", statuses[i], "Bodyless");
        http_response_add_header(r, "Content-Type", "test/empty");
        http_response_finish(r, NULL, 0);
        text = serialized(r);
        assert(!strstr(text, "Content-Length:"));
        free(text);
    }
    http_response_init(r, "RTSP/1.0", 200, "OK");
    http_response_finish(r, NULL, 0);
    text = serialized(r);
    assert(!strstr(text, "Content-Length:"));
    free(text);
    http_response_reverse_request_init(r, "POST", "/event", "HTTP/1.1");
    http_response_finish(r, "OK", 2);
    text = serialized(r);
    assert(!strcmp(text, "POST /event HTTP/1.1\r\nContent-Length: 2\r\n\r\nOK"));
    free(text);
    http_response_destroy(r);
    puts("HTTP responses: control framing, explicit lengths, upgrade and reverse requests passed.");
    return 0;
}
