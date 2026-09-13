/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <stddef.h>
#include "../lib/httpd.h"
#include <assert.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

/* Inject real readiness-loop failures without closing descriptors beneath
 * a running worker or depending on signal scheduling. */
static pthread_mutex_t fault_mutex = PTHREAD_MUTEX_INITIALIZER;
static int pending_select_error;
static int observed_select_error;
static int pending_accept_error;
static int fault_select(int nfds, fd_set *reads, fd_set *writes, fd_set *errors,
                        struct timeval *timeout) {
    pthread_mutex_lock(&fault_mutex);
    int error = pending_select_error;
    pending_select_error = 0;
    if (error) observed_select_error = error;
    pthread_mutex_unlock(&fault_mutex);
    if (error) {
        errno = error;
        return -1;
    }
    return select(nfds, reads, writes, errors, timeout);
}
static int fault_accept(int fd, struct sockaddr *address, socklen_t *length) {
    pthread_mutex_lock(&fault_mutex);
    int error = pending_accept_error;
    pending_accept_error = 0;
    pthread_mutex_unlock(&fault_mutex);
    if (error) {
        errno = error;
        return -1;
    }
    return accept(fd, address, length);
}
#define select fault_select
#define accept fault_accept
#include "../lib/httpd.c"
#undef accept
#undef select

static void inject_select_error(int error) {
    pthread_mutex_lock(&fault_mutex);
    observed_select_error = 0;
    pending_select_error = error;
    pthread_mutex_unlock(&fault_mutex);
    for (int attempt = 0; attempt < 2000; attempt++) {
        pthread_mutex_lock(&fault_mutex);
        int observed = observed_select_error;
        pthread_mutex_unlock(&fault_mutex);
        if (observed == error) return;
        usleep(1000);
    }
    assert(!"server did not reach injected select failure");
}

static void wait_for_worker_exit(httpd_t *server) {
    for (int attempt = 0; attempt < 2000; attempt++) {
        MUTEX_LOCK(server->run_mutex);
        int running = server->running;
        MUTEX_UNLOCK(server->run_mutex);
        if (!running) return;
        usleep(1000);
    }
    assert(!"server worker did not exit");
}

static void *new_connection(void *opaque, unsigned char *local, int llen,
                            unsigned char *remote, int rlen, unsigned int zone) {
    (void)opaque; (void)local; (void)llen; (void)remote; (void)rlen; (void)zone;
    return malloc(1);
}
static void log_message(void *opaque, int level, const char *message) {
    (void)opaque; (void)level; (void)message;
}
static void request(void *opaque, http_request_t *req, http_response_t **response) {
    (void)opaque;
    assert(!strcmp(http_request_get_url(req), "/probe"));
    *response = http_response_create();
    http_response_init(*response, "HTTP/1.1", 200, "OK");
    http_response_finish(*response, "ok", 2);
}
static int connect_to(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    return fd;
}
static void expect_ok(int fd) {
    char reply[512] = {0};
    size_t used = 0;
    for (;;) {
        fd_set reads;
        FD_ZERO(&reads); FD_SET(fd, &reads);
        struct timeval timeout = {2, 0};
        assert(select(fd + 1, &reads, NULL, NULL, &timeout) == 1);
        assert(used < sizeof(reply) - 1);
        ssize_t got = recv(fd, reply + used, sizeof(reply) - 1 - used, 0);
        assert(got > 0);
        used += (size_t)got;
        reply[used] = '\0';
        char *body = strstr(reply, "\r\n\r\n");
        if (body && used >= (size_t)(body + 4 - reply) + 2) break;
    }
    assert(strstr(reply, "HTTP/1.1 200 OK"));
}
int main(void) {
    alarm(14);
    signal(SIGPIPE, SIG_IGN);
    logger_t *logger = logger_init();
    logger_set_callback(logger, log_message, NULL);
    logger_set_level(logger, LOGGER_DEBUG);
    httpd_callbacks_t callbacks = {0};
    callbacks.conn_init = new_connection;
    callbacks.conn_request = request;
    callbacks.conn_destroy = free;
    httpd_t *server = httpd_init(logger, &callbacks, 0);
    unsigned short port = 0;
    assert(httpd_start(server, &port) == 1);
    int slow = connect_to(port);
    assert(send(slow, "G", 1, 0) == 1);
    usleep(50000);
    int normal = connect_to(port);
    const char message[] = "GET /probe HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    assert(send(normal, message, sizeof(message) - 1, 0) == sizeof(message) - 1);
    expect_ok(normal); /* Another client works while the first has one byte. */
    assert(send(slow, message + 1, 3, 0) == 3);
    usleep(20000);
    assert(send(slow, message + 4, sizeof(message) - 5, 0) == sizeof(message) - 5);
    expect_ok(slow);
    assert(send(slow, message, sizeof(message) - 1, 0) == sizeof(message) - 1);
    expect_ok(slow); /* Prefix state resets on keepalive requests. */
    int reverse = connect_to(port);
    char response[1024];
    memset(response, ' ', sizeof(response));
    memcpy(response, "HTTP/1.1 200 OK\r\n\r\n", 19);
    assert(send(reverse, response, sizeof(response), 0) == sizeof(response));
    usleep(50000); /* Exercise full-buffer reverse logging under ASan. */
    close(reverse); close(normal); close(slow);

    /* An unexpected worker exit must be reported honestly, then joined so
     * the owner can restart this same listener object. */
    for (int attempt = 0; attempt < 3; attempt++) {
        int old_fd4 = server->server_fd4, old_fd6 = server->server_fd6;
        inject_select_error(EBADF);
        wait_for_worker_exit(server);
        assert(!httpd_is_running(server));
        httpd_stop(server);
        assert(server->joined);
        assert(!httpd_is_running(server));
        errno = 0;
        assert(fcntl(old_fd4, F_GETFD) == -1 && errno == EBADF);
        if (old_fd6 != -1) {
            errno = 0;
            assert(fcntl(old_fd6, F_GETFD) == -1 && errno == EBADF);
        }
        httpd_stop(server); /* Repeated owner cleanup is harmless. */
        port = 0;
        assert(httpd_start(server, &port) == 1);
        int retry = connect_to(port);
        assert(send(retry, message, sizeof(message) - 1, 0) == sizeof(message) - 1);
        expect_ok(retry);
        close(retry);
    }

    /* A signal interruption does not represent a failed listening socket. */
    inject_select_error(EINTR);
    int after_signal = connect_to(port);
    assert(send(after_signal, message, sizeof(message) - 1, 0) == sizeof(message) - 1);
    expect_ok(after_signal);
    close(after_signal);

    const int transient_accept_errors[] = {EINTR, EAGAIN};
    for (size_t i = 0; i < sizeof(transient_accept_errors) / sizeof(transient_accept_errors[0]); i++) {
        pthread_mutex_lock(&fault_mutex);
        pending_accept_error = transient_accept_errors[i];
        pthread_mutex_unlock(&fault_mutex);
        int retry = connect_to(port);
        assert(send(retry, message, sizeof(message) - 1, 0) == sizeof(message) - 1);
        expect_ok(retry);
        close(retry);
        pthread_mutex_lock(&fault_mutex);
        assert(!pending_accept_error);
        pthread_mutex_unlock(&fault_mutex);
    }
    httpd_destroy(server);
    logger_destroy(logger);
    alarm(0);
    return 0;
}
