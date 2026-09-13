/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
/* Exercise the private UDP worker lifecycle without an audio renderer. */
#include "../lib/raop_rtp.c"

static THREAD_RETVAL exited_audio_worker(void *arg) {
    raop_rtp_t *audio = arg;
    MUTEX_LOCK(audio->run_mutex);
    audio->running = 0;
    MUTEX_UNLOCK(audio->run_mutex);
    return 0;
}

int main(void) {
    logger_t *logger = logger_init();
    logger_set_level(logger, LOGGER_ERR);
    raop_callbacks_t callbacks = {0};
    const unsigned char key[16] = {0}, iv[16] = {0};
    timing_protocol_t timing = NTP;
    raop_ntp_t *ntp = raop_ntp_init(logger, &callbacks, "127.0.0.1", 4, 0, &timing);
    assert(ntp);
    /* remotelen selects the address family; remote itself is textual. */
    raop_rtp_t *audio = raop_rtp_init(logger, &callbacks, ntp, "127.0.0.1", 4, key, iv);
    assert(audio);
    for (int attempt = 0; attempt < 20; attempt++) {
        audio->csock = socket(AF_INET, SOCK_DGRAM, 0);
        audio->dsock = socket(AF_INET, SOCK_DGRAM, 0);
        assert(audio->csock >= 0 && audio->dsock >= 0);
        int control_fd = audio->csock, data_fd = audio->dsock;
        audio->running = 1;
        audio->joined = 0;
        THREAD_CREATE(audio->thread, exited_audio_worker, audio);
        for (;;) {
            MUTEX_LOCK(audio->run_mutex);
            int running = audio->running;
            MUTEX_UNLOCK(audio->run_mutex);
            if (!running) break;
            usleep(1000);
        }
        raop_rtp_stop(audio);
        assert(audio->joined && audio->csock == -1 && audio->dsock == -1);
        errno = 0;
        assert(fcntl(control_fd, F_GETFD) == -1 && errno == EBADF);
        errno = 0;
        assert(fcntl(data_fd, F_GETFD) == -1 && errno == EBADF);
        unsigned short remote_port = 9000, control_port = 0, data_port = 0;
        unsigned char codec = 2;
        unsigned int sample_rate = 44100;
        raop_rtp_start_audio(audio, &remote_port, &control_port, &data_port, &codec, &sample_rate);
        assert(control_port && data_port && audio->running && !audio->joined);
        raop_rtp_stop(audio);
        assert(audio->joined && audio->csock == -1 && audio->dsock == -1);
        raop_rtp_stop(audio);
    }
    raop_rtp_destroy(audio);
    raop_ntp_destroy(ntp);
    logger_destroy(logger);
    puts("audio worker exit/rejoin/restart regression passed");
    return 0;
}
