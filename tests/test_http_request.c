/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/http_request.h"

static void expect_header(http_request_t *request, const char *name, const char *expected) {
    const char *actual = http_request_get_header(request, name);
    assert(actual && !strcmp(actual, expected));
}

static void add_independent_chunk(http_request_t *request, const char *data, size_t length) {
    assert(length > 0);
    /* No terminator or following request bytes are accessible through this
     * allocation. ASan can detect lookahead past the supplied input span. */
    char *chunk = malloc(length);
    assert(chunk);
    memcpy(chunk, data, length);
    assert(http_request_add_data(request, chunk, (int)length) == 0);
    memset(chunk, 0xa5, length);
    free(chunk);
}

static void test_header_lookup(const char *method, const char *protocol, bool fragmented) {
    char message[1024];
    int header_length = snprintf(message, sizeof(message),
        "%s /fixture %s\r\n"
        "hOsT: Receiver.Example:7000\r\n"
        "X-Apple-Session-Id: AbC-0123-xYz\r\n"
        "aUtHoRiZaTiOn: Digest username=\"MiXeD\", nonce=\"aB9+/=\"\r\n"
        "cSeQ: 00037\r\n"
        "dAcP-Id: AbCdEF0123456789\r\n"
        "aCtIvE-rEmOtE: 009871\r\n"
        "CoNtEnT-TyPe: application/X-Apple-Binary-Plist\r\n"
        "cOnTeNt-LeNgTh: 4\r\n\r\n",
        method, protocol);
    assert(header_length > 0 && header_length + 4 < (int)sizeof(message));
    const char body[] = {'a', 'B', '\0', 'Z'};
    memcpy(message + header_length, body, sizeof(body));
    size_t length = (size_t)header_length + sizeof(body);

    http_request_t *request = http_request_init();
    assert(request && !http_request_is_reverse(request));
    if (fragmented) {
        for (size_t i = 0; i < length; i++) {
            add_independent_chunk(request, message + i, 1);
        }
    } else {
        add_independent_chunk(request, message, length);
    }
    assert(http_request_is_complete(request));
    assert(!http_request_has_error(request));
    assert(!strcmp(http_request_get_method(request), method));
    assert(!strcmp(http_request_get_protocol(request), protocol));
    assert(!strcmp(http_request_get_url(request), "/fixture"));

    expect_header(request, "Host", "Receiver.Example:7000");
    expect_header(request, "HOST", "Receiver.Example:7000");
    expect_header(request, "host", "Receiver.Example:7000");
    expect_header(request, "X-Apple-Session-ID", "AbC-0123-xYz");
    expect_header(request, "x-apple-session-id", "AbC-0123-xYz");
    expect_header(request, "X-Apple-Session-Id", "AbC-0123-xYz");
    expect_header(request, "Authorization", "Digest username=\"MiXeD\", nonce=\"aB9+/=\"");
    expect_header(request, "AUTHORIZATION", "Digest username=\"MiXeD\", nonce=\"aB9+/=\"");
    expect_header(request, "authorization", "Digest username=\"MiXeD\", nonce=\"aB9+/=\"");
    expect_header(request, "CSeq", "00037");
    expect_header(request, "DACP-ID", "AbCdEF0123456789");
    expect_header(request, "Active-Remote", "009871");
    expect_header(request, "Content-Type", "application/X-Apple-Binary-Plist");
    expect_header(request, "Content-Length", "4");
    assert(http_request_get_header(request, "Host") == http_request_get_header(request, "hOsT"));

    const char *missing[] = {
        "", "Missing-Header", "X-Apple-Session", "X-Apple-Session-ID-Extra",
        "X-Apple-Session-ID ", "Authorizatio", "Authorization-Extra", "CSeqX"
    };
    for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); i++) {
        assert(http_request_get_header(request, missing[i]) == NULL);
    }
    int body_length = 0;
    const char *parsed_body = http_request_get_data(request, &body_length);
    assert(body_length == (int)sizeof(body) && !memcmp(parsed_body, body, sizeof(body)));
    char *headers = NULL;
    assert(http_request_get_header_string(request, &headers) > 0);
    assert(strstr(headers, "X-Apple-Session-Id: AbC-0123-xYz\n"));
    assert(strstr(headers, "aUtHoRiZaTiOn: Digest username=\"MiXeD\", nonce=\"aB9+/=\"\n"));
    free(headers);

    /* Reverse-channel handling must still suppress ordinary request fields. */
    http_request_set_reverse(request);
    assert(http_request_is_reverse(request));
    assert(http_request_get_header(request, "X-Apple-Session-ID") == NULL);
    assert(http_request_get_header(request, "x-apple-session-id") == NULL);
    assert(http_request_get_header(request, "AUTHORIZATION") == NULL);
    assert(http_request_get_method(request) == NULL);
    assert(http_request_get_url(request) == NULL);
    assert(http_request_get_protocol(request) == NULL);
    assert(http_request_get_header_string(request, &headers) == 0 && headers == NULL);
    assert(!http_request_has_error(request));
    http_request_destroy(request);
}

static void test_request_line_splits(const char *method, const char *protocol) {
    char message[512];
    int length = snprintf(message, sizeof(message),
        "%s /fixture?case=MiXeD %s\r\n"
        "Host: Fixture.Example\r\n"
        "X-Apple-Session-Id: AbC-0123-xYz\r\n"
        "CSeq: 00037\r\n"
        "Content-Length: 4\r\n\r\naB9Z", method, protocol);
    assert(length > 0 && length < (int)sizeof(message));
    size_t first_line = (size_t)(strstr(message, "\r\n") - message) + 2;
    /* Every boundary, including URL/protocol separation, inside each protocol
     * and version token, and between CR and LF, must produce the same request. */
    for (size_t split = 1; split <= first_line; split++) {
        http_request_t *request = http_request_init();
        assert(request);
        add_independent_chunk(request, message, split);
        assert(!http_request_is_complete(request));
        add_independent_chunk(request, message + split, (size_t)length - split);
        assert(http_request_is_complete(request));
        assert(!http_request_has_error(request));
        assert(!strcmp(http_request_get_method(request), method));
        assert(!strcmp(http_request_get_protocol(request), protocol));
        assert(!strcmp(http_request_get_url(request), "/fixture?case=MiXeD"));
        expect_header(request, "host", "Fixture.Example");
        expect_header(request, "X-Apple-Session-ID", "AbC-0123-xYz");
        expect_header(request, "cseq", "00037");
        int body_length = 0;
        const char *body = http_request_get_data(request, &body_length);
        assert(body_length == 4 && !memcmp(body, "aB9Z", 4));
        http_request_set_reverse(request);
        assert(http_request_get_protocol(request) == NULL);
        assert(http_request_get_header(request, "X-Apple-Session-ID") == NULL);
        http_request_destroy(request);
    }
}

int main(void) {
    test_header_lookup("POST", "HTTP/1.0", false);
    test_header_lookup("POST", "HTTP/1.0", true);
    test_header_lookup("POST", "HTTP/1.1", false);
    test_header_lookup("POST", "HTTP/1.1", true);
    test_header_lookup("SET_PARAMETER", "RTSP/1.0", false);
    test_header_lookup("SET_PARAMETER", "RTSP/1.0", true);
    test_header_lookup("SET_PARAMETER", "RTSP/1.1", false);
    test_header_lookup("SET_PARAMETER", "RTSP/1.1", true);
    test_request_line_splits("POST", "HTTP/1.0");
    test_request_line_splits("POST", "HTTP/1.1");
    test_request_line_splits("SET_PARAMETER", "RTSP/1.0");
    test_request_line_splits("SET_PARAMETER", "RTSP/1.1");
    puts("Requests: every HTTP/RTSP request-line split, bytewise input, case-insensitive headers, exact values and reverse handling passed.");
    return 0;
}
