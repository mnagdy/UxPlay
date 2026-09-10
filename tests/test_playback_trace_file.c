#define _GNU_SOURCE
#include "../renderers/playback_trace_file.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char directory[256];
static char path[512];
static const char *good = "MPV playback: session=10 state=failed terminal_report=1 child_pid=42 child_generation=10 error_code=5 end_reason=error";

static const char *file_path(const char *name) {
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    return path;
}

static void clean(void) {
    DIR *dir = opendir(directory);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) assert(!unlink(file_path(entry->d_name)));
    closedir(dir);
}

static size_t read_file(const char *name, char *out, size_t size) {
    FILE *file = fopen(file_path(name), "rb");
    assert(file);
    size_t got = fread(out, 1, size - 1, file);
    assert(feof(file));
    fclose(file); out[got] = 0; return got;
}

static void basic_and_privacy(void) {
    assert(playback_trace_file_set_directory_for_tests(directory, 4096));
    assert(playback_trace_file_write(good));
    char data[4096]; size_t before = read_file("playback-trace.jsonl", data, sizeof(data));
    assert(strstr(data, "\"MESSAGE\":\"MPV playback: session=10"));
    assert(strstr(data, "\"_BOOT_ID\":\""));
    assert(strstr(data, "\"_PID\":\""));
    assert(strstr(data, "\"__REALTIME_TIMESTAMP\":\""));
    assert(strstr(data, "\"__MONOTONIC_TIMESTAMP\":\""));
    assert(data[before - 1] == '\n');
    const char *bad[] = {
        "raw PRIVATE_SENTINEL", "MPV playback: session=1 state=playing url=https://PRIVATE_SENTINEL",
        "MPV playback: session=1 state=PRIVATE_SENTINEL", "MPV playback: session=1 state=playing codec=secret",
        "MPV playback: session=1 state=playing Authorization=secret", "MPV playback: session=1 state=playing path=/secret",
        "MPV playback: session=1 state=playing\nPRIVATE_SENTINEL", "MPV playback: session=1 state=playing\rPRIVATE_SENTINEL",
        "MPV playback: session=1 state=playing position=NaN", "MPV playback: session=1 state=playing position=inf",
        "MPV playback: session=1 state=playing session=2", "MPV playback: session=0 state=playing",
        "MPV playback: session=18446744073709551616 state=playing", "MPV playback: session=1 state=playing paused=2",
        "MPV playback: session=1 state=playing schema=99", "MPV playback: session=1 state=playing size=99999x720",
        "MPV playback: session=1 state=playing ", "MPV playback: session=1  state=playing",
        "MPV playback: session=1", "MPV control: session=1 state=playing", "Playback session: session=1 action=stop"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) assert(!playback_trace_file_write(bad[i]));
    char oversized[7000]; memset(oversized, 'a', sizeof(oversized)); oversized[6999] = 0;
    assert(!playback_trace_file_write(oversized));
    assert(!playback_trace_file_write(NULL));
    assert(read_file("playback-trace.jsonl", data, sizeof(data)) == before);
    assert(!strstr(data, "PRIVATE_SENTINEL"));
    assert(playback_trace_file_write("MPV control: session=10 action=seek accepted=1 value=12.500 monotonic_ms=25"));
    assert(playback_trace_file_write("Playback session: session=10 event=request route=direct-http monotonic_ms=25"));
    assert(playback_trace_file_write("MPV playback: session=10 state=failed failure_stage=audio-output"));
    assert(playback_trace_file_write("MPV playback: session=10 state=failed failure_stage=video-output"));
    struct stat st;
    assert(!stat(directory, &st) && (st.st_mode & 0777) == 0700);
    assert(!stat(file_path("playback-trace.jsonl"), &st) && (st.st_mode & 0777) == 0600);
    clean();
}

static void packet_capture_and_warning_privacy(void) {
    assert(playback_trace_file_set_directory_for_tests(directory, 8192));
    const char *capture =
        "MPV playback: session=11 state=playing schema=2 "
        "packet_capture_enabled=1 packet_capture_active=0 packet_capture_complete=1 "
        "packet_capture_started_ms=1000 packet_capture_ended_ms=16000 packet_capture_errors=0 packet_log_rejected=2 "
        "audio_packets=0 video_packets=375 audio_packet_bytes=0 video_packet_bytes=1234567 "
        "audio_packets_with_pts=0 video_packets_with_pts=375 "
        "audio_packet_first_at_ms=0 audio_packet_last_at_ms=0 video_packet_first_at_ms=1040 video_packet_last_at_ms=15960 "
        "audio_packet_first_pts=0.000 audio_packet_last_pts=0.000 video_packet_first_pts=-0.040 video_packet_last_pts=14.960 "
        "last_warning_stage=demux last_warning_reason=unknown last_warning_at_ms=15500 "
        "packet_corrupt_warnings=1 pes_mismatch_warnings=2 demux_read_warnings=3";
    assert(playback_trace_file_write(capture));
    char data[8192]; size_t before = read_file("playback-trace.jsonl", data, sizeof(data));
    assert(strstr(data, capture));
    const char *bad[] = {
        "packet_capture_enabled=2", "packet_capture_active=-1", "packet_capture_complete=true",
        "packet_capture_started_ms=-1", "packet_capture_ended_ms=NaN", "packet_capture_errors=-1", "packet_log_rejected=-1",
        "audio_packets=18446744073709551616", "video_packets=-1", "audio_packet_bytes=1.5",
        "video_packet_bytes=inf", "audio_packets_with_pts=-1", "video_packets_with_pts=1e3",
        "audio_packet_first_at_ms=-1", "audio_packet_last_at_ms=NaN",
        "video_packet_first_at_ms=-1", "video_packet_last_at_ms=1.1",
        "audio_packet_first_pts=NaN", "audio_packet_last_pts=inf",
        "video_packet_first_pts=-inf", "video_packet_last_pts=1e999",
        "last_warning_stage=https://PRIVATE_SENTINEL", "last_warning_reason=PRIVATE_SENTINEL",
        "last_warning_reason=unknown\nPRIVATE_SENTINEL", "last_warning_at_ms=-1",
        "packet_corrupt_warnings=-1", "pes_mismatch_warnings=NaN", "demux_read_warnings=18446744073709551616",
        "packet_text=PRIVATE_SENTINEL"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char record[512];
        snprintf(record, sizeof(record), "MPV playback: session=11 state=playing %s", bad[i]);
        assert(!playback_trace_file_write(record));
    }
    assert(read_file("playback-trace.jsonl", data, sizeof(data)) == before);
    assert(!strstr(data, "PRIVATE_SENTINEL"));
    const char *new_reasons[] = {
        "pes-size-mismatch", "packet-corrupt", "demux-read-error", "hls-expired-segments", "hls-sequence-change",
        "virtual-terminal-unavailable", "frame-present-failure", "drm-display-failure"
    };
    for (size_t i = 0; i < sizeof(new_reasons) / sizeof(new_reasons[0]); ++i) {
        char record[256];
        snprintf(record, sizeof(record), "MPV playback: session=11 state=playing diagnostic_reason=%s last_warning_reason=%s",
                 new_reasons[i], new_reasons[i]);
        assert(playback_trace_file_write(record));
    }
    const char *stages[] = {"video-decode", "audio-decode", "video-output", "audio-output", "demux", "source", "unclassified", "unknown"};
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        char record[128];
        snprintf(record, sizeof(record), "MPV playback: session=11 state=playing last_warning_stage=%s", stages[i]);
        assert(playback_trace_file_write(record));
    }
    clean();
}

static void rotation_and_lock(void) {
    assert(playback_trace_file_set_directory_for_tests(directory, 512));
    for (int i = 0; i < 20; ++i) assert(playback_trace_file_write(good));
    struct stat st;
    assert(!stat(file_path("playback-trace.jsonl"), &st) && st.st_size <= 512);
    assert(!stat(file_path("playback-trace.previous.jsonl"), &st) && st.st_size <= 512);
    int fd = open(file_path(".playback-trace.lock"), O_RDONLY);
    assert(fd >= 0 && !flock(fd, LOCK_EX | LOCK_NB));
    assert(!playback_trace_file_write(good));
    close(fd);
    assert(playback_trace_file_write(good));
    pid_t child = fork(); assert(child >= 0);
    if (!child) { for (int i = 0; i < 100; ++i) (void)playback_trace_file_write(good); _exit(0); }
    for (int i = 0; i < 100; ++i) (void)playback_trace_file_write(good);
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    char data[1024];
    size_t n = read_file("playback-trace.jsonl", data, sizeof(data));
    assert(n <= 512 && data[0] == '{' && data[n - 1] == '\n');
    n = read_file("playback-trace.previous.jsonl", data, sizeof(data));
    assert(n <= 512 && data[0] == '{' && data[n - 1] == '\n');
    clean();
}

static void torn_tail_recovery(void) {
    assert(playback_trace_file_set_directory_for_tests(directory, 4096));
    assert(playback_trace_file_write(good));
    int fd = open(file_path("playback-trace.jsonl"), O_WRONLY | O_APPEND);
    assert(fd >= 0 && write(fd, "{torn", 5) == 5); close(fd);
    assert(playback_trace_file_write(good));
    char data[4096]; read_file("playback-trace.jsonl", data, sizeof(data));
    assert(!strstr(data, "torn"));
    int lines = 0; for (char *p = data; *p; ++p) if (*p == '\n') ++lines;
    assert(lines == 2);
    clean();
    fd = open(file_path("playback-trace.jsonl"), O_WRONLY | O_CREAT, 0600);
    assert(fd >= 0 && write(fd, "{torn", 5) == 5); close(fd);
    assert(playback_trace_file_write(good));
    read_file("playback-trace.jsonl", data, sizeof(data));
    assert(!strstr(data, "torn"));
    clean();
}

static void unsafe_files_and_directory(void) {
    assert(playback_trace_file_set_directory_for_tests(directory, 512));
    assert(!symlink("does-not-exist", file_path("playback-trace.jsonl")));
    assert(!playback_trace_file_write(good)); clean();
    assert(!symlink("does-not-exist", file_path(".playback-trace.lock")));
    assert(!playback_trace_file_write(good)); clean();
    assert(playback_trace_file_write(good));
    assert(!symlink("does-not-exist", file_path("playback-trace.previous.jsonl")));
    assert(!playback_trace_file_write(good)); clean();
    int fd = open(file_path("playback-trace.jsonl"), O_CREAT | O_WRONLY, 0644); assert(fd >= 0); close(fd);
    assert(!playback_trace_file_write(good)); clean();
    fd = open(file_path("target"), O_CREAT | O_WRONLY, 0600); assert(fd >= 0); close(fd);
    char target[512]; snprintf(target, sizeof(target), "%s/target", directory);
    assert(!link(target, file_path("playback-trace.jsonl")));
    assert(!playback_trace_file_write(good)); clean();
    assert(!mkfifo(file_path("playback-trace.jsonl"), 0600));
    assert(!playback_trace_file_write(good)); clean();
    assert(!chmod(directory, 0755));
    assert(!playback_trace_file_write(good));
    assert(!chmod(directory, 0700));
    assert(!symlink(directory, file_path("symlink")));
    char symlink_path[512]; snprintf(symlink_path, sizeof(symlink_path), "%s/symlink", directory);
    assert(playback_trace_file_set_directory_for_tests(symlink_path, 512));
    assert(!playback_trace_file_write(good)); clean();
    assert(!playback_trace_file_set_directory_for_tests("relative", 512));
    assert(!playback_trace_file_set_directory_for_tests(directory, 128));
    assert(!playback_trace_file_set_directory_for_tests(directory, 2 * 1024 * 1024));
}

int main(int argc, char **argv) {
    snprintf(directory, sizeof(directory), "/tmp/uxplay-trace-test-XXXXXX");
    assert(mkdtemp(directory));
    if (argc == 2 && !strcmp(argv[1], "--accept-lines")) {
        assert(playback_trace_file_set_directory_for_tests(directory, 1024 * 1024));
        char line[8192]; int count = 0, rejected = 0;
        while (fgets(line, sizeof(line), stdin)) {
            size_t length = strlen(line);
            if (length && line[length - 1] == '\n') line[--length] = 0;
            if (!playback_trace_file_write(line)) ++rejected;
            ++count;
        }
        clean(); assert(!rmdir(directory));
        printf("formatter records accepted=%d rejected=%d\n", count - rejected, rejected);
        return !count || rejected ? 1 : 0;
    }
    basic_and_privacy();
    packet_capture_and_warning_privacy();
    rotation_and_lock();
    torn_tail_recovery();
    unsafe_files_and_directory();
    assert(playback_trace_file_set_directory_for_tests(NULL, 0));
    assert(!rmdir(directory));
    puts("persistent playback trace tests passed");
    return 0;
}
