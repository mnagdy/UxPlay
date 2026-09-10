#ifndef UXPLAY_PLAYBACK_TRACE_FILE_H
#define UXPLAY_PLAYBACK_TRACE_FILE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in caller only: stores a bounded, typed diagnostic record, never raw logs.
 * False means rejected/unavailable/busy; playback must continue normally.
 * Current + previous are limited to 1 MiB each. No per-record fsync: a power
 * loss can lose recent records. Does not configure or enable journal storage. */
bool playback_trace_file_write(const char *text);

/* Test-only configuration, before concurrent use. NULL restores production
 * location. Directory must be absolute and owned by this process's user. */
bool playback_trace_file_set_directory_for_tests(const char *directory, size_t max_bytes);

#ifdef __cplusplus
}
#endif
#endif
