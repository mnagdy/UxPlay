# AirPlay startup and replacement follow-up — 10 September 2026

The user confirmed YouTube and iPlayer picture/sound on the HEVC receiver,
but reported slow startup and the old YouTube item resuming during replacement.

## Evidence

The retained trace for receiver PID 12888 shows:

- Generation 4: playlist request at 13607.967 s, source queued at 13614.140 s
  (6.173 s preparation), and file-loaded about 9.89 s after queuing.
- Generation 5: request at 13640.104 s. A resume was incorrectly accepted for
  old generation 4 at 13640.701 s. The new source was queued at 13648.222 s,
  8.118 s after the request, and loaded about 15.83 s after queuing.
- Generation 5 requested a start at 533 s; the backend first opened at zero
  and then issued an exact seek. Generation 7 was similarly loaded at zero,
  paused and seeking toward 29.44 s, eventually failing its control timeout.

Both cached YouTube requests completed six playlist entries. Source preparation
is separate from mpv initialization and initial-position handling. These traces
do not establish that the HEVC display-plane change caused the source-fetch
delay, and do not establish a return to instantaneous startup.

Evidence is retained locally in `.pi-dev/hevc-startup-regression-report.json`.

## Changes

An accepted replacement pauses the previous player immediately. While the new
source is being prepared, rate commands update the new request's pending pause
intent; they cannot resume the old player. A removed playlist remains paused
until a new request is accepted. Pending scrubs cannot seek the old stream.
The new request's pause intent is applied when its source is ready.

The backend now supplies the initial position as mpv's `--start` option before
loading media. Each generation already owns a fresh process, so the option
cannot leak to a later item. This removes the load-at-zero followed by a second
exact seek and decoder reinitialization. Initial positioning remains pending
until playback-restart and is covered by the existing bounded load timeout.
Subsequent user seeks retain their existing behavior.

The option follows the
[mpv 0.40 playback-control documentation](https://github.com/mpv-player/mpv/blob/v0.40.0/DOCS/man/options.rst).
No decoder, display-plane, quality or receiver configuration setting changes.

## Validation and activation

The Linux backend, receiver callback and real HTTP playback groups pass.
Receiver tests reproduce playlist removal followed by a new request and a
resume before source readiness, assert the old stream stays paused, and verify
the new pause intent. MP4, MPEG-TS HLS, fragmented-MP4 HLS, separate-audio HLS
and HEVC tests start at a nonzero position while paused, then exercise seeking,
resume and shutdown. These are headless tests, not iPhone latency acceptance.

Pi release `20260910T212127226779Z-52c2997e3800-dirty` was activated by the user
and verified running as PID 13469 with zero service restarts. Its configuration
is unchanged. The one-use activation was completed; its obsolete local helper
was removed during cleanup. The activation helper validated candidate binary/source checksums, the previous
release, configuration and player inactivity. It uses managed service switching
with rollback on failure. At this checkpoint, rollback was the working HEVC receiver
`20260910T210453311246Z-52c2997e3800-dirty`.

After activation, two generated files were submitted through AirPlay at an
initial position of four seconds: 1080p60 H.264 and 4K60 Main 10 HEVC. Both
reported playback advancing from the requested position by the two-second
poll (not ready at one second). Hardware decoding was observed for each:
`h264_v4l2m2m` and `drm` HEVC, respectively. No decoder errors or decoder drops
were recorded. Output drops occurred during startup (6 H.264, 24 HEVC) and did
not increase during subsequent observed playback. Both `/stop` requests were
accepted, both players exited with code 0 without signals, and the receiver
returned to idle. The known virtual-terminal notice remains in diagnostics.

Evidence is in `.pi-dev/startup-switch-live-report.json` (PID 13469,
generations 2 and 3), `startup-switch-h264-result.json` and
`startup-switch-hevc-result.json`. These short direct-HTTP tests do not measure
YouTube's FCUP preparation or verify visual acceptance of the switching fix.

The user subsequently confirmed that the previous item stays paused during
replacement. Startup still takes 5–10 seconds.

## Cached HLS startup follow-up

PID 13469 generations 5 and 6 spent 5.715 and 5.000 seconds preparing the
playlist cache, followed by 2.923 and 5.158 seconds from queuing to readiness.
Generation 4 was a slower outlier (2.584 seconds preparation, 19.548 seconds
to readiness). Two-second progress sampling is not a precise first-picture
measurement. Evidence is `.pi-dev/youtube-startup-after-switch-fix.json`.

For the mpv backend with `hls-pi4` enabled, the receiver now applies the
existing compatibility filter before requesting media playlists. Unsupported
variants and unused audio groups are not downloaded. Compatible variants are
still downloaded so a failed preferred playlist can fall back to another
available quality. Once downloads finish, the master exposed to mpv contains
the highest-bandwidth playable variant and its required media groups. This
matches mpv's default `hls-bitrate=max` selection while avoiding initialization
of multiple quality variants. Missing or invalid bandwidth metadata leaves
selection with mpv. All available tracks within required groups are retained.

The GStreamer finalizer and profiles without `hls-pi4` retain their existing
behavior. Direct HTTP playback and HEVC/H.264 decoder/display settings are
unchanged. This is a startup optimization to qualify, not evidence that
YouTube's startup delay has been eliminated.

Linux cache, HTTP handler, backend, receiver callback and real HTTP playback
tests pass. New cases cover skipping unsupported FCUP requests, highest
available quality selection, missing video/audio fallback, malformed bandwidth
metadata, scoped media URLs and cached resume. The default build also passes
its cache and HTTP handler groups.

Pi release `20260910T213612844711Z-52c2997e3800-dirty` was activated after
the user disconnected AirPlay, with unchanged configuration. The managed
health check passed; PID 14044 was recorded running with zero restarts.
This one-use activation also completed; its obsolete local helper was removed.
Use [the maintained development workflow](development-on-pi.md) for new
activation or managed rollback. The checks included exact previous release,
candidate source/binary hashes, expected HEVC configuration, no pending reboot,
no competing mpv/FFmpeg, and service health with rollback on failure.

The recorded rollback target is `20260910T212127226779Z-52c2997e3800-dirty`.

The user reported an eight-second initial start, then confirmed that switching
videos is pretty fast. PID 14044 shows:

| Generation | Requested position | Playlist preparation | Queued to file-loaded | First selected video packet after request |
|---|---:|---:|---:|---:|
| 2 | 25.21 s | 0.345 s | 1.813 s | 7.622 s |
| 3 | 0 s | 1.925 s | 1.717 s | about 3.64 s |
| 4 | 1053 s | 0.946 s | 2.043 s | about 3.34 s |

Generation 2 reduced 19 advertised media routes to six compatible downloads,
then exposed two entries (selected video and required audio). Generation 3
reduced 17 to six downloads and two exposed entries. The initial eight-second
delay is now after cache preparation: file-loaded was reported at 2.158 seconds,
but selected packets did not arrive until 7.622 seconds. These packet timestamps
measure delivery to mpv's selected queues, not network first-byte time or physical
first picture. Initial seeking remained pending during that gap. Generation 4
also started at a nonzero position and did not show the same long gap; do not
conclude that every resume or every fresh connection incurs eight seconds.

The receiver remained healthy with zero restarts. Generation 2 had zero decoder
or output drops; generation 3 had two output drops and no decoder drops.
Hardware H.264 decoding remained active. Evidence is retained in
`.pi-dev/youtube-startup-filtered-report.json` and
`.pi-dev/youtube-startup-switch-comparison.json`.

Before changing seek or network options, compare a fresh AirPlay connection
starting a video at zero with a fresh connection resuming that same video.
The remaining initial-start delay is not yet resolved.
