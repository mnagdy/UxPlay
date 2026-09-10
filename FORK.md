# Raspberry Pi UHF compatibility branch

This branch starts from upstream UxPlay **v1.73.7** and backports the existing direct HTTP/HTTPS video playback implementation. It is a compatibility branch, not an official UxPlay release.

The original upstream README and licenses are retained. General upstream documentation is in [README.md](README.md).

## Current status — 10 September 2026

The optional mpv backend now handles direct AirPlay video and its audio;
GStreamer remains the default and continues to handle mirroring and audio-only
AirPlay. HDMI status/debug modes show receiver readiness, stream information,
playback progress and bounded diagnostics. See [the mpv development guide](docs/mpv-screen-development.md)
for configuration and validation history.

The opt-in `mpv-decode pi4-hevc-experimental` policy now uses hardware HEVC
decoding and direct display-plane output while retaining `h264_v4l2m2m` for
H.264. A two-minute independent 4K60 Main 10 test produced user-confirmed smooth
picture and tone with no steady-playback output drops. Receiver smoke tests
also passed for 4K60 HEVC and 1080p60 H.264; the user confirmed working YouTube
and iPlayer. The connected display is 720p60, so these results establish decoding
4K source material, not native 4K HDMI output. Problematic UHF sources and longer
real-stream qualification remain open. See [the HEVC trial](docs/hevc-airplay-trial.md)
and [the staged test suite](docs/hevc-4k-test-plan.md).

The current Pi release is `20260910T213612844711Z-52c2997e3800-dirty`.
YouTube replacement keeps the old video paused, and cached HLS avoids unsupported
playlist downloads and exposes the highest-bandwidth available compatible variant
with its required audio. The user reports fast video switching. One initial start
still took eight seconds; the remaining delay occurred after playlist preparation
while opening at a nonzero position. Initial startup is not yet resolved. Evidence
and limitations are in [the startup follow-up](docs/startup-switch-regression.md).

Linux backend, receiver, cache and real HTTP playback checks passed, as did the
default build's cache checks and native Pi compilation/activation health checks.
The `software` and `pi4-safe` policies retain their existing behavior. Local
stream captures, generated fixtures, reports and device configuration are excluded
from Git.

## Earlier recovery checkpoint — 10 September 2026

The user confirmed working YouTube playback after a silent UHF video stream on release `20260910T132850290330Z-52c2997e3800-dirty`. The sampled UHF HEVC video reached the display sink 1.341 seconds after the receiver's play request. UHF explicitly stopped its separate audio transport before requesting video, then supplied three complete video fragments without real audio samples. **UHF sound remains unresolved.** Receiver startup measurements exclude time spent on the phone before its request, and this does not establish compatibility with every application or 4K stream.

This source includes missing-audio timeline repair, the Pi4 software-HEVC recovery workaround, faster direct-HLS buffering, terminal failed-stream cleanup, case-insensitive headers and bounded audio diagnostics. It also includes subsequently tested request-line fragmentation and FairPlay bounds fixes that have not yet been activated on the Pi. The working legacy HTTP capability profile is retained.

All eleven native test groups passed for the current playback and protocol code. The two latest protocol groups also passed with UndefinedBehaviorSanitizer. Real HTTP fixtures cover normal audio/video, absent audio, delayed responses, and successful replacement after an actual stream error. A separately built [GStreamer key-validation patch](patches/README.md) passed six encrypted-stream cases and three playback/switching comparisons; the ordinary receiver build does not install that dependency patch. Historical measurements and earlier validation stages follow below.

## Provenance

- Base: `df67c212a433cf6dda3676dd40c097900d24e645` (`v1.73.7`).
- Direct video handling: `6f9250414ee33a4e008b6a7638ac448ace0395c9`.
- Follow-up routing change: `2ece5790c0cd834291006f0ceb5b6b0e6a7abbee`.
- Both changes were cherry-picked with their original authors and upstream commit references.

In stock 1.73.7, a direct URL such as `http://phone-address:port/stream.m3u8` is rejected by the YouTube-oriented URL handling. This branch forwards ordinary HTTP/HTTPS locations to the existing GStreamer playback path. It does not depend on the playlist being named `master.m3u8`.

## Installation

See [the Raspberry Pi guide](docs/raspberry-pi-uhf.md). The reproducible starting point is tag **`v1.73.7-uhf.1`**; development continues on **`pi-uhf`**. The executable still reports `1.73.7`, so retain the source revision and distinguish the installed binary by its filename.

For local iteration, see [developing on the Mac and deploying to the Pi](docs/development-on-pi.md). The development helper keeps incremental builds and separate releases on the Pi, with a dedicated service override and rollback. Playback optimisation proposals are in [the Pi performance plan](docs/raspberry-pi-performance.md).

## Initial evidence and investigation scope

The equivalent backport was built on a Raspberry Pi 4 running Raspberry Pi OS Lite. Its user reported successful UHF live TV and series playback. This is a limited device report, not an exhaustive compatibility test.

Open investigation items:

- Roughly 20 seconds from selecting the AirPlay receiver to video starting.
- Low frame rate with some 4K content; the selected decoder and power/thermal state have not been measured.
- AirPlay discovery after reboot was confirmed on 9 September 2026; investigate further only if it recurs.
- Channel changes, reconnects, phone lock/background behaviour, two-phone handover, extended playback and long seeks need recorded verification.

Keep playbin3 (`hls`) as the initial configuration. A playbin2 (`hls 2`) experiment is not a validated improvement.

Potential development areas are startup timing instrumentation, live-stream playback status, buffering/pause handling and error recovery. Establish measurements before changing these behaviours. This branch does not add DRM support, HEVC hardware drivers, or a proven low-latency configuration.

## First playback iteration

The development working tree now separates requested pause/play/stop from buffering, handles 0% buffering and streams without buffering notifications, preserves pause while seeking, validates long seek positions, and reports live position without requiring a finite duration. Controls received during renderer rebuilding retain their intent; superseded renderer bus/EOS events are ignored. This does not redesign all application threading or mirroring behaviour.

`Direct playback:` log entries identify the selected decoder and input format, decoded-frame memory, initial buffering milestones and first arrival at the video sink. Sink arrival is not physical presentation, and a DMA-BUF observation alone does not establish end-to-end zero-copy. No driver, decoder preference or buffering-size changes are included in this iteration.

Optional headless regression tests use `-DUXPLAY_BUILD_TESTS=ON`; see [the development guide](docs/development-on-pi.md). Real UHF startup time and 4K frame rate still require before/after measurements on the Pi.

Validation on 9 September 2026: all three C/GStreamer test groups passed on Debian Trixie ARM64 and on a Raspberry Pi 4 with kernel `6.18.34+rpt-rpi-v8` and GStreamer `1.26.2`. Linux AddressSanitizer/UndefinedBehaviorSanitizer checks passed with leak detection disabled for that integrated run; 19 deployment-helper tests also passed. The first iteration was built natively and deployed with a successful process-health check. UHF playback comparison is pending.

## YouTube first-frame freeze

On 9 September, the Pi's YouTube master playlist advertised 18 audio/video entries while its cache had fetched only the first few. A repeated `/play` request reused that incomplete cache because the old check required only a playback location. GStreamer decoded a first frame, then stayed paused while repeatedly receiving HTTP 404 for an uncached quality variant. A generated HTTP HLS stream with a missing advertised variant reproduced that stall; the same stream with all variants available played from both zero and a 165-second resume position.

Cached YouTube playback now requires the master and every referenced cache entry to be present, including duplicate URI aliases. An interrupted cache is fetched again on a repeated play request. FCUP responses must match the outstanding URL before they can advance collection, and a duplicate final response cannot restart playback. Direct HTTP playback used by UHF retains its separate path. Logs are flushed by line so the journal shows playback events promptly.

The first device retest exposed the reason collection had stopped: YouTube returned HTTP 404 for a media playlist even though it appeared in the master. Collection now continues past unavailable media playlists. Before playback, the master is filtered to available variants and audio renditions; variants whose required audio group is unavailable are removed too. A failed master or a cache with no playable variant is still rejected. This prevents an adaptive quality switch from requesting a playlist the receiver cannot serve.

These changes address incomplete playlist reuse. They do not establish an improvement to 4K decoding or eliminate the time spent fetching YouTube's playlists. The later Pi 4 profile device test is recorded below.

Validation: six C/HTTP/GStreamer test groups passed on Debian Trixie ARM64 with AddressSanitizer and UndefinedBehaviorSanitizer (integrated leak detection disabled), including the unavailable-variant fallback. Tests cover failed audio dependencies, all variants failing, duplicate URI aliases and delayed responses with an older request ID. The HTTP handler test fails with premature playback when only the old cache-reuse condition is restored, and passes with the fix. Five test groups also passed natively on the Pi for the final fallback build; its FFmpeg-dependent HTTP media fixture is not installed. Release `20260909T095659695775Z-cd33afca3ada-dirty` was deployed with successful process-health checks. The later Pi 4 profile device test is recorded below.

## Pi 4 YouTube codec compatibility

The next device retest successfully completed the playlist cache but still froze after the first frame. On the Pi's GStreamer 1.26.2, the master offered both H.264 and VP9; hlsdemux2 failed to construct common video caps and stopped with a flow error before playback began. The same cached video reproduced the failure with headless audio/video sinks. Restricting that master to its available H.264/AAC-LC variants allowed the playback clock to advance on the Pi. A generated mixed-codec HLS fixture also reproduced the demux error independently.

The opt-in `-hls-pi4` option (configuration line `hls-pi4`) now filters cached YouTube variants to declared H.264/AAC-LC, at most 1920×1080 and at most 60 fps when declared. It keeps adaptive choices within that codec family, rejects a cache with no matching variant, and does not change UHF's direct HTTP route. It is metadata selection, not transcoding or a guarantee that all H.264 profiles are supported. Unconfigured receivers retain the existing selection behaviour.

Bounded error diagnostics report the failing element's factory, error domain/code and an allowlisted flow reason without copying the error message, debug payload or signed stream URL. The successful device playback report is recorded below.

Validation of the Pi 4 profile: all six C/HTTP/GStreamer groups passed on Debian Trixie ARM64 with AddressSanitizer/UndefinedBehaviorSanitizer (integrated leak detection disabled), including the generated mixed-codec failure and filtered initial/resume playback. All five test groups available natively on the Pi passed. Release `20260909T101349217073Z-cd33afca3ada-dirty` was activated with a separate `hls-pi4` configuration and remained running after reboot.


## Confirmed working baseline — 9 September 2026

After reboot, the receiver started automatically with no service restarts. Its AirPlay and audio advertisements were visible from the Mac, and the advertised TCP port accepted a connection. The user then confirmed AirPlay was working and reported that YouTube playback and switching videos worked very well. This confirms the tested device workflow; extended playback, phone handover and 4K performance still need separate testing.

The historical baseline commit `52c2997e380084a44a0bea95846bbbd5e444013d` preserved the source and tests archived for release `20260909T101349217073Z-cd33afca3ada-dirty`, with documentation updates. That release's executable has SHA-256 `0b2e79b31ead385e07eef59e2012117042d5a104870d99e393bd0a9ea46ab8a2`. Its original source fingerprint is `907c7d7bde908bd5a6bc9bb682777b4a4b9744b0dd8bd1bba371de4d3d7a9621`; later source changes are described separately.

Working receiver configuration:

```text
n Projector
nh
s 1920x1080
fps 30
vs kmssink
as alsasink
hls
nohold
nofreeze
hls-pi4
```

Before committing, all 19 deployment-helper tests and all five headless test groups available on the Pi passed again. The optional FFmpeg-dependent HTTP HLS fixture is not installed on the Pi; its earlier container validation is recorded above.

## UHF loading diagnostics

The 10 September user comparison confirmed ALAC audio-only playback, silent UHF video, and successful YouTube picture/sound afterward. The latest startup build reached the UHF video sink 2.025 seconds after the request; four completed fragments contained no audio samples. Audio RTP had stopped before video negotiation. Follow-up changes record explicit teardown reasons and distinguish compressed audio, decoded audio and audio-sink arrival. An experiment sharing the RAOP/discovery feature mask with `/server-info` caused iOS to request an unsupported HTTP `/fp-setup` handshake, preventing playback. It was reverted: the legacy HTTP profile remains necessary until that additional protocol path is implemented and validated. UHF's missing audio is unresolved.

The 9 September UHF investigation found multiple requests stuck at 0% buffering before any video decoded. INFO logging now correlates each request, control and pipeline stage with a session number; records exposed HTTP status/headers and terminal HTTP error codes; summarises the initial HLS manifest without printing its URLs; and counts media delivered to the HLS parser. Buffering decreases are visible, and an existing renderer timer reports state, data idle times and adaptive audio/video buffer levels during prolonged loading. Output and observers are bounded per session. See [the diagnostics guide](docs/development-on-pi.md#uhf-stream-loading-diagnostics) for coverage and limitations.

All seven C/HTTP/GStreamer test groups passed in the ARM64 Linux container with AddressSanitizer/UndefinedBehaviorSanitizer (integrated leak detection disabled). All six groups available on the Pi passed, including a real HTTP 404 and delayed response body. The optional container HLS fixture also verified the numeric summary and playback of a direct media playlist. The logging build was activated on 9 September at 12:25 BST and captured a UHF stream with growing video buffers but no buffered audio. A sampled fragment declared AAC audio but contained only video samples. A local headless comparison reproduced the 0% stall with the empty audio track declared and completed buffering with that declaration removed; missing sound in UHF's AirPlay output remains unresolved. These changes add diagnostic information, not a claimed UHF playback fix.

## Receiver recovery candidate, 9 September 2026

The next candidate repairs confirmed empty audio intervals in fragmented-MP4 HLS while retaining the audio track for later sound. Pi tests now play the captured failing HEVC fragment with its original audio declaration retained, and generated live-stream tests preserve normal, late and interrupted audio. Missing-audio and stalled-HTTP channel replacements produce new video and audio; measured replacement calls took 15 ms and 104 ms in the isolated tests. See [the implementation and test limits](docs/development-on-pi.md#missing-audio-and-channel-replacement).

The Pi profile selects `vc4` for `kmssink` unless a device was explicitly configured. Isolated sink initialization measured 155 ms versus 3.6–4.0 seconds with generic discovery. Hardware HEVC also passed through the existing receiver's actual `kmssink` output; a generic headless sink had given a misleading negotiation failure. That short playback proof did not cover the later observed kernel shutdown hang. Under `hls-pi4`, automatic selection now excludes only `v4l2slh265dec`, preserving H.264 hardware decoding and using software HEVC where available.

Empty HTTP control replies now have explicit lengths, fixing a reproduced standard-client timeout. Audio renderer teardown is serialized against arriving packets and ignores queued errors from stopped/replaced renderers; the original queued-error-after-stop path crashed in the regression test. This candidate still requires activation and another phone-to-projector comparison. It does not establish perfect compatibility across applications or restore sound absent from the sender's HLS output.

## UHF stop recovery, 9 September 2026

The recovery build activated at 13:21 BST retained working YouTube playback: the first actual display buffer arrived in 791 ms, and the user confirmed good playback. UHF then reached the missing-audio repair after 10.197 seconds but produced no decoded frame before the stop. Stopping it left the receiver in the Pi kernel's `hevc_d_h265_stop` IRQ wait, preventing later YouTube playback. The first kernel blocked-task report predates the separate cancellation probe, whose later close was blocked on the receiver's driver mutex. Service liveness alone did not detect this failure.

The Pi profile now excludes the exact stateless HEVC factory from automatic decoder selection before pipelines start. The change is local to this receiver process; other decoder ranks, including H.264 hardware, are preserved. Software HEVC throughput and phone-to-projector audio remain to verify after the kernel is cleared by reboot. An earlier successful 15-second HEVC hardware fixture was insufficient to establish stop/reconnect safety. The installed driver has an unbounded IRQ wait during shutdown; an [upstream Pi4 report](https://github.com/raspberrypi/linux/issues/7537) describes the same class of unkillable decoder hang.

A separate startup correction accepts initialization metadata followed by the first MP4 segment boundary in one buffer. It keeps later boundaries conservative and avoids waiting for a third segment when two are already published. Regression coverage measures the first real rendered video buffer rather than treating a PLAYING state alone as success.

The following candidate adds audited GStreamer 1.26.2 fragment-completion repair, reducing the single-initial-fragment regression from 10.464 s to 0.383 s while preserving delayed AAC within the same response. The Pi4 direct-HTTP route also uses a three-second buffering target; cached YouTube retains its previous settings. Additional numeric RAOP network/delivery/renderer traces target the remaining missing-sound issue without changing audio ownership. These changes still require integrated and real-device validation.
