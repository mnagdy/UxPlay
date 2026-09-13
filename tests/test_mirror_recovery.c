/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
/* Exercise the private worker lifecycle without a renderer or iPhone. */
#include "../lib/raop_rtp_mirror.c"

static THREAD_RETVAL exited_worker(void *arg) {
    raop_rtp_mirror_t *mirror = arg;
    MUTEX_LOCK(mirror->run_mutex);
    mirror->running = 0;
    MUTEX_UNLOCK(mirror->run_mutex);
    return 0;
}

int main(void) {
    raop_rtp_mirror_t mirror;
    memset(&mirror, 0, sizeof(mirror));
    mirror.logger = logger_init();
    logger_set_level(mirror.logger, LOGGER_ERR);
    mirror.remote_saddr.ss_family = AF_INET;
    MUTEX_CREATE(mirror.run_mutex);
    for (int attempt = 0; attempt < 20; attempt++) {
        mirror.mirror_data_sock = socket(AF_INET, SOCK_STREAM, 0);
        assert(mirror.mirror_data_sock >= 0);
        int old_fd = mirror.mirror_data_sock;
        mirror.running = 1;
        mirror.joined = 0;
        THREAD_CREATE(mirror.thread_mirror, exited_worker, &mirror);
        for (;;) {
            MUTEX_LOCK(mirror.run_mutex);
            int running = mirror.running;
            MUTEX_UNLOCK(mirror.run_mutex);
            if (!running) break;
            usleep(1000);
        }
        raop_rtp_mirror_stop(&mirror);
        assert(mirror.joined == 1);
        assert(mirror.mirror_data_sock == -1);
        errno = 0;
        assert(fcntl(old_fd, F_GETFD) == -1 && errno == EBADF);
        /* A finished worker must no longer block a subsequent SETUP. */
        unsigned short port = 0;
        raop_rtp_mirror_start(&mirror, &port, 0);
        assert(port != 0 && mirror.running && !mirror.joined);
        raop_rtp_mirror_stop(&mirror);
        assert(mirror.joined && mirror.mirror_data_sock == -1);
        raop_rtp_mirror_stop(&mirror);
    }
    MUTEX_DESTROY(mirror.run_mutex);
    logger_destroy(mirror.logger);
    puts("mirror worker exit/rejoin/restart regression passed");
    return 0;
}
