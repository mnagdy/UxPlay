# Raspberry Pi UHF compatibility branch

This branch starts from upstream UxPlay **v1.73.7** and backports the existing direct HTTP/HTTPS video playback implementation. It is a small compatibility branch, not an official UxPlay release.

The original upstream README and licenses are retained. General upstream documentation is in [README.md](README.md).

## Provenance

- Base: `df67c212a433cf6dda3676dd40c097900d24e645` (`v1.73.7`).
- Direct video handling: `6f9250414ee33a4e008b6a7638ac448ace0395c9`.
- Follow-up routing change: `2ece5790c0cd834291006f0ceb5b6b0e6a7abbee`.
- Both changes were cherry-picked with their original authors and upstream commit references.

In stock 1.73.7, a direct URL such as `http://phone-address:port/stream.m3u8` is rejected by the YouTube-oriented URL handling. This branch forwards ordinary HTTP/HTTPS locations to the existing GStreamer playback path. It does not depend on the playlist being named `master.m3u8`.

## Installation

See [the Raspberry Pi guide](docs/raspberry-pi-uhf.md). The reproducible starting point is tag **`v1.73.7-uhf.1`**; development continues on **`pi-uhf`**. The executable still reports `1.73.7`, so retain the source revision and distinguish the installed binary by its filename.

For local iteration, see [developing on the Mac and deploying to the Pi](docs/development-on-pi.md). The development helper keeps incremental builds and separate releases on the Pi, with a dedicated service override and rollback. Playback optimisation proposals are in [the Pi performance plan](docs/raspberry-pi-performance.md).

## Evidence and remaining work

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

The source and test files in this commit match the archived source for the running release `20260909T101349217073Z-cd33afca3ada-dirty`; subsequent changes only update documentation. The deployed executable has SHA-256 `0b2e79b31ead385e07eef59e2012117042d5a104870d99e393bd0a9ea46ab8a2`. The release's original source fingerprint is `907c7d7bde908bd5a6bc9bb682777b4a4b9744b0dd8bd1bba371de4d3d7a9621`.

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
