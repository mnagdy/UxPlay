/* SPDX-License-Identifier: GPL-3.0-or-later
 * Private, bounded datagram control. Called only from the GLib main loop.
 * Directory ownership protects both the socket and reply addresses.
 */
#ifndef UXPLAY_DEBUG_CONTROL_H
#define UXPLAY_DEBUG_CONTROL_H
#ifndef _WIN32
#include <sys/un.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib/gstdio.h>

static int debug_fd = -1, debug_lock = -1;
static gchar *debug_dir = NULL, *debug_path = NULL;
static void debug_control_close() {
    if (debug_fd >= 0) { close(debug_fd); unlink(debug_path); debug_fd = -1; }
    if (debug_lock >= 0) { close(debug_lock); debug_lock = -1; }
    g_clear_pointer(&debug_path, g_free);
    g_clear_pointer(&debug_dir, g_free);
}
static void debug_control_init(screen_info_mode_t *mode) {
    debug_dir = g_build_filename(g_get_home_dir(), ".uxplay-control", NULL);
    struct stat st;
    if ((g_mkdir(debug_dir, 0700) && errno != EEXIST) ||
        lstat(debug_dir, &st) || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 077)) goto failed;
    {
        gchar *path = g_build_filename(debug_dir, "lock", NULL);
        debug_lock = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        g_free(path);
        if (debug_lock < 0 || flock(debug_lock, LOCK_EX | LOCK_NB)) goto failed;
        debug_path = g_build_filename(debug_dir, "control.sock", NULL);
        struct sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        if (strlen(debug_path) >= sizeof(addr.sun_path)) goto failed;
        g_strlcpy(addr.sun_path, debug_path, sizeof(addr.sun_path));
        int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) goto failed;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, O_NONBLOCK);
        unlink(debug_path); /* exclusive lock establishes stale socket ownership */
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) { close(fd); goto failed; }
        debug_fd = fd;
        path = g_build_filename(debug_dir, "mode", NULL);
        gchar *saved = NULL;
        if (g_file_get_contents(path, &saved, NULL, NULL)) {
            if (!strcmp(saved, "on\n")) *mode = SCREEN_INFO_DEBUG;
            else if (!strcmp(saved, "off\n")) *mode = SCREEN_INFO_STATUS;
        }
        g_free(saved); g_free(path);
        atexit(debug_control_close);
        return;
    }
failed:
    debug_control_close();
    g_warning("Live debug control unavailable (private directory, socket or receiver lock).");
}
static gboolean debug_control_tick(gpointer) {
    if (debug_fd < 0) return TRUE;
    for (int i = 0; i < 8; ++i) {
        char request[32];
        struct sockaddr_un peer = {};
        socklen_t size = sizeof(peer);
        ssize_t count = recvfrom(debug_fd, request, sizeof(request), 0,
                                 (struct sockaddr *)&peer, &size);
        if (count < 0) break;
        /* Reply only to helper sockets inside our private directory. */
        if (!memchr(peer.sun_path, 0, sizeof(peer.sun_path)) ||
            !g_str_has_prefix(peer.sun_path, debug_dir) ||
            peer.sun_path[strlen(debug_dir)] != '/' ||
            strchr(peer.sun_path + strlen(debug_dir) + 1, '/')) continue;
        const char *reply = "ERROR Invalid command";
        bool on = count == 2 && !memcmp(request, "on", 2);
        bool off = count == 3 && !memcmp(request, "off", 3);
        if (on || off) {
            gchar *path = g_build_filename(debug_dir, "mode", NULL);
            if (g_file_set_contents(path, on ? "on\n" : "off\n", -1, NULL)) {
                screen_info_mode_t mode = on ? SCREEN_INFO_DEBUG : SCREEN_INFO_STATUS;
                screen_status_set_mode(mode);
                video_renderer_configure_screen(mode);
                video_renderer_screen_refresh();
                if (status_display && playback_output_released && !mpv_owns_output)
                    show_status_display();
                reply = on ? "Debug overlay on" : "Debug overlay off";
            } else reply = "ERROR Could not save setting; overlay unchanged";
            g_free(path);
        } else if (count == 6 && !memcmp(request, "status", 6)) {
            reply = screen_status_get_mode() == SCREEN_INFO_DEBUG ? "Debug overlay on" : "Debug overlay off";
        }
        sendto(debug_fd, reply, strlen(reply), 0, (struct sockaddr *)&peer, size);
    }
    return TRUE;
}
#else
static void debug_control_init(screen_info_mode_t *) {}
static gboolean debug_control_tick(gpointer) { return TRUE; }
#endif
#endif
