# Develop on the Mac, run on the Pi

Use the Mac as the source checkout and the Pi as the native build/test machine. The helper sends a snapshot of local edits over SSH, builds incrementally on the Pi, then switches the existing receiver service. GitHub is for commits and sharing; pushing a commit is not required for each test.

The development helper is separate from UxPlay's playback implementation. Hardware playback and successful discovery must still be checked on the actual Pi.

## One-time preparation

On the Pi, install the small additions used by the development helper:

```bash
sudo apt install rsync ccache python3
```

The C++/CMake/GStreamer build dependencies should already be installed from the [setup guide](raspberry-pi-uhf.md). The helper retains `Release`, `NO_X11_DEPS=ON` and two build jobs for an initial comparable baseline. It uses ccache if available. No kernel upgrade is part of this setup.

On the Mac, open a terminal in this repository. Python 3, SSH and rsync must be available. Create `.pi-dev.json`, replacing the example user and hostname with the Pi's SSH login:

```json
{"host": "youruser@projector.local"}
```

This file is ignored by Git and excluded from transfers. If `.local` discovery is unreliable, use the current IP shown by `hostname -I` on the Pi. A router DHCP reservation can keep that address stable. An existing SSH alias also works.

Check the connection and installed tools:

```bash
./scripts/pi-dev check
```

You can override the saved host for any command:

```bash
./scripts/pi-dev check --host youruser@192.168.1.124
```

The IP above is an example, not discovery of your Pi. Use normal SSH host-key verification. An SSH key makes unattended build commands convenient; establish ordinary `ssh youruser@host` access first. Agent runs use SSH batch mode and cannot answer password prompts. Interactive deployment can ask for the Pi's sudo password when switching the service. No password is stored and no sudoers rules are installed.

## The iteration loop

1. Edit locally with Codex.
2. Run the deploy command from the Mac:

```bash
./scripts/pi-dev deploy
```

3. Select the receiver in UHF and test the change.
4. Inspect recent service logs from the Mac:

```bash
./scripts/pi-dev logs
```

The first build compiles the whole project. Later builds reuse the same CMake build directory and only rebuild affected code. Actual build times need measurement on the Pi. The receiver remains running during compilation, though compilation competes for CPU and can disturb playback; measure video performance after the build ends. A successful deployment restarts the service and interrupts the current stream.

To compile without switching the receiver:

```bash
./scripts/pi-dev build
```

To return to the previous development release:

```bash
./scripts/pi-dev rollback
```

To restore the receiver command used before these development deployments:

```bash
./scripts/pi-dev stable
```

The latter removes only `90-uxplay-dev.conf`. An existing `50-uhf-test.conf` remains in place, so the pre-development UHF build stays available. Existing installed binaries and original configuration files are not overwritten. The name `stable` describes restoring that baseline; it does not certify its playback reliability.

## What is transferred and saved

Tracked files and nonignored new files are copied into a local snapshot. Deleted files remain deleted; local settings, Git internals and build products are excluded. Source symlinks are rejected. Avoid editing the disposable source directory on the Pi: the next transfer replaces its contents.

```text
~/uxplay-dev/
  src/             source mirror; the only rsync deletion boundary
  build/Release/   persistent native build cache
  releases/        separate binaries, source archives and build metadata
  state/           deployment state and lock
```

Each release has a timestamp, Git revision and dirty marker, plus a fingerprint of the transferred source. This identifies uncommitted test builds even though the executable's existing version string still says 1.73.7. Activation snapshots the selected configuration for that release.

The helper checks the existing service account and command before writing its dedicated override. Custom command-line arguments are rejected rather than silently discarded. It checks that the intended executable remains running with a stable process ID and restart count, and attempts to restore the previous override if activation fails. These process checks do not prove that AirPlay discovery, audio or video works.

## Recovery and validation

If a new build fails to compile, the running service stays unchanged. If a build runs but has a playback regression, use `rollback` or `stable` and retain the failing release's logs and source archive.

If SSH is interrupted, inspect `~/uxplay-dev/state/lock` and check for an active build/deployment before removing a stale lock. Do not clear it while another deployment is running. Neither lock recovery nor old-release deletion is automatic.

After every relevant playback change, test discovery, sound/picture, pause/resume, channel change, stop/reconnect and switching phones. Record startup time and frame behaviour using the same sample streams. Logs can contain media URLs or tokens; review them before sharing publicly.

Run the local deployment-helper tests without a Pi:

```bash
python3 -m unittest discover -s tests -p 'test_pi*.py'
```

The playback code also has optional C/GStreamer tests. They use generated video and headless sinks, so they do not take over the projector. In a Linux build environment with the existing development dependencies and GStreamer's base/good plugins:

```bash
cmake -S . -B build/tests -DNO_X11_DEPS=ON -DUXPLAY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tests --parallel 2
ctest --test-dir build/tests --output-on-failure
```

The tests cover buffering and user intent, safe time conversion, the production renderer's startup/control path, stream replacement, live progress, and diagnostics through actual playbin3 playback. `tests/Dockerfile` provides a Debian Trixie environment for running them on a Mac with Docker; this checks Linux code but does not emulate the Pi's video hardware.

YouTube cache tests also cover interrupted collection and complete-cache reuse. When Python 3 and FFmpeg are installed at configuration time, CTest adds an HTTP HLS test using generated H.264 video and a separate AAC audio playlist. It checks advancing playback from zero and from a 165-second resume point. A mixed H.264/VP9 master reproduces the GStreamer 1.26.2 demux failure; the test then runs the production Pi 4 playlist filter and verifies that both initial playback and resume advance. FFmpeg needs the libx264, libvpx-vp9 and AAC encoders. The test runtime needs GStreamer's HLS, MPEG-TS and codec plugins; the Dockerfile includes these dependencies. All media and HTTP serving stay local to the test environment.

For the first playback iteration, deploy and test a 1080p stream followed by the problematic 4K stream. Collect `Direct playback:` entries using `./scripts/pi-dev logs`, together with the seconds from selecting the receiver to visible video. Also test pause/resume, seeking past 40 minutes in a series, stop/reconnect and channel changes. Current diagnostics observe first video-sink buffer arrival, not physical presentation; they do not yet distinguish manifest download from media-segment download.

Once native incremental builds are measured, consider cross-compilation or CI if compilation is the bottleneck. A Mac build itself cannot validate Linux GStreamer/DRM behaviour. CMake supports reusable build directories through [its configure/build commands](https://cmake.org/cmake/help/latest/manual/cmake.1.html); [rsync](https://download.samba.org/pub/rsync/rsync.1) handles incremental transfer.

## Raspberry Pi 4 YouTube compatibility

Add `hls-pi4` on its own line in the receiver configuration (or pass `-hls-pi4` on the command line) to enable HLS and keep cached YouTube video within the tested H.264/AAC-LC path. The profile retains available variants with declared H.264 and AAC-LC codecs, a positive resolution no larger than 1920×1080, and no declared frame rate above 60 fps. Adaptive quality selection remains available among compatible variants. If none qualify, playback is rejected with a diagnostic message.

This selects from declared playlist metadata; it does not transcode media or guarantee that every H.264 profile/bit depth is hardware-decodable. Ordinary HTTP streams from UHF use their existing route. For `kmssink`, the profile also selects the Pi's `vc4` display driver, unless `driver-name`, `bus-id` or `fd` was explicitly configured. This avoids several seconds of generic driver discovery for each new sink. The profile also excludes `v4l2slh265dec` from automatic decoder selection in this receiver process. A real UHF stop wedged that Pi HEVC driver in an uninterruptible kernel wait and prevented later YouTube playback. HEVC uses `avdec_h265` when installed; H.264 hardware selection remains unchanged. Software HEVC performance, especially above 720p, still requires measurement. Removing `hls-pi4` restores the default playlist, decoder and display-driver choices. Keep the existing `hls` line if HLS should remain enabled after removing the profile.

## UHF stream-loading diagnostics

Normal INFO logging now includes a numeric `session` for each direct-video request. Incoming AirPlay request timestamps and an allowlisted sender/route label distinguish time spent before renderer startup. Play, pause and stop intent is recorded against the active session.

For each pipeline, diagnostics record HTTP response status and selected headers, first source data, the initial manifest's entry/variant counts, target duration and media sequence, first media data entering the HLS parser, decoder selection and first video-sink arrival. The manifest summary captures at most 64 KiB and marks truncation; it describes the initial manifest, not subsequent live playlist refreshes. The HTTP bus messages cover requests exposed by GStreamer, principally the initial source; GStreamer 1.26's adaptive segment downloader does not expose every response or successful fragment statistic on the bus. Its reported terminal HTTP errors are included when available.

Buffering reports identify their source and mode, include rate/time estimates, and record decreases as well as increases. While play is requested but startup/rebuffering has not completed, a `stage=waiting` report appears every five seconds for the first minute, then every thirty seconds. It includes current/pending pipeline state, requested intent, preroll/live flags, observed source/media byte counts and idle time, plus adaptive bandwidth and audio/video buffer levels where supported. `-1` means unavailable. Media byte counts measure delivery to the HLS parser and can include retries; they are not network throughput measurements. The live flag describes GStreamer's no-preroll state handling, not whether a television programme is live.

Output is bounded per session: 32 network reports, 64 buffering changes, 8 errors/warnings, 32 controls and 24 waiting snapshots, with at most 16 observed elements and 16 buffering sources. Buffer probes count bytes without reading media content. The bounded initial manifest capture is summarised using only numeric directives; URLs, cookies, arbitrary headers, tags, error payloads and media data are not printed by these diagnostics. Full upstream debug logging (`-d` / `GST_DEBUG`) remains a separate, potentially sensitive facility.

The diagnostics use the existing renderer lifetime lock and timer; they do not change buffering thresholds, playback intent, decoder choice, or retry policy. The headless tests include real HTTP playback, an HTTP 404, a server that sends headers then delays its body, repeated buffering regressions, private metadata and observer teardown.

## Missing audio and channel replacement

The receiver now repairs a specific GStreamer 1.26 push-mode fragmented-MP4 failure: a completed HLS segment contains video while a declared audio track has no samples. A bounded top-level MP4 parser identifies segment boundaries without storing media bodies. For a confirmed empty interval, the receiver sends timed missing-data GAP events through the existing audio track. Real audio remains selected and its timestamps are unchanged. Until that track's first real sample arrives, adaptive audio buffering is bypassed after video preroll; pause and stop still apply. Ordinary buffering resumes when audio arrives.

This is deliberately conservative: ambiguous/coalesced boundaries, malformed/unbounded boxes, unsupported buffer lists, multiple audio/video tracks and non-forward segments do not trigger repair. It requires `hlsdemux2` and an explicit MP4 `styp` boundary. The first boundary may follow initialization metadata in the same buffer; boundaries after media retain the conservative guard. This allows repair when two initial segments are already available without waiting for a third. It cannot recreate audio samples the sender never supplies. These limits leave other formats and sender-specific failures to investigate.

`stage=gap-repair` records accepted gap intervals with numeric timing only. `stage=gap-only-audio` records the temporary buffering mode. A first video-sink buffer still measures arrival, not physical presentation.

The optional `direct_renderer_hls_gaps` test generates legal video and tone and serves a growing live playlist with ordinary, absent, late and interrupted audio. It counts real sink buffers and checks audio timing, then replaces both a missing-audio stream and a genuinely stalled HTTP response. To run on a Pi without FFmpeg, generate with `tests/run_hls_gap_test.py --generate-only DIRECTORY --ffmpeg FFMPEG_PATH`, copy that synthetic directory, and pass `--fixtures DIRECTORY` alongside the Pi's `test_direct_renderer_hls` executable.

Ordinary empty HTTP control replies now include `Content-Length: 0`; a standard client can finish reading the reply while keeping the connection open. Upgrade and bodyless-status framing is preserved. Renderer startup also returns without a one-second preroll wait, allowing control processing to continue. Separate audio-renderer tests cover packet delivery racing stop/restart and queued errors after teardown.

### UHF startup and audio trace follow-up

On the audited GStreamer 1.26.2 core and adaptivedemux2 plugin, the repair also observes the video fragment completion notification emitted after HLS drains the entire response. It only acts for one attached parsebin and one unambiguous active video parser, complete MP4 boxes, and a whole fragment with video but no audio. A completed mdat or elapsed timer is insufficient. Other versions retain the next-segment/EOS fallback. Tests cover a single initially published fragment and audio delayed into the final moof of the same response.

For the explicit Pi4 profile, direct HTTP HLS uses a three-second low buffering watermark instead of GStreamer's automatic ten-second target. The thirty-second download ceiling and ordinary pause/resume buffering remain. The request callback carries the actual source route; cached YouTube playback keeps its existing buffering configuration. This reduces initial wait but is not a promise that every network/source can sustain playback.

Safe numeric RAOP traces distinguish setup, actual network media, audio_process delivery, renderer acceptance/drop reasons, and forced video-transition stops. Empty and format-only packets are counted separately. RTP summaries stop repeating after the first minute and include a final exit summary. These diagnostics do not change audio ownership or imply that an accepted compressed packet reached the speaker.

RAOP teardown traces now distinguish the sender stopping its audio stream, the sender ending its session, and connection destruction. Direct playback separately records `audio-decoder-input`, `audio-decoder-output` and `first-audio-sink-buffer`, with fixed codec names and numeric rate/channel information. Headers, empty buffers and GAP buffers are excluded from these media milestones, including when they precede real media in a buffer list. Session summaries retain each audio stage independently. Audio-sink arrival is not a measurement of physical sound.

`AirPlay capabilities: endpoint=server-info` records a numeric feature mask when the sender requests it. The HTTP response deliberately retains its working legacy `0x27F` profile. A real iPhone experiment returning the full RAOP/discovery mask instead caused repeated HTTP `/fp-setup` requests, which this receiver does not implement, and prevented playback requests. The experiment was reverted. Sharing the mask requires implementing and validating the additional handshake first; it did not solve the missing-audio problem.

### Failed streams and control requests

A terminal direct-playback error now stops that request's pipeline and releases its media resources. Playback status reports a stopped timeline with readiness false. Later rate, seek, buffering and preroll messages cannot restart the failed pipeline; a new play request starts a fresh session. The control connection remains available, so a failed channel does not require a service restart before another app can play. Old pipeline errors are ignored once a replacement request has been registered.

HTTP and RTSP request header names are matched without regard to ASCII letter case, as the protocols require. Header values remain byte-for-byte unchanged. Protocol names and versions are obtained from the parser's bounded callbacks, so a request line split across network reads retains its correct protocol. Tests cover every request-line split and bytewise input with independent allocations, mixed-case session/authentication headers, binary bodies and reverse-channel handling.

FairPlay setup also rejects unsupported mode indices before reading its response table or changing saved handshake state. The unsupported HTTP `/fp-setup2` route checks body length before reading its version byte and retains its existing 421 response. These guards do not add HTTP FairPlay support or change advertised capabilities.

An isolated [GStreamer key-validation patch](../patches/README.md) and encrypted-stream fixtures address a reproduced dependency failure. This dependency patch is not installed by the ordinary receiver build.

## Optional mpv and HDMI feedback build

Use `./scripts/pi-dev build --with-mpv` to prepare a release with the optional direct-video backend. The receiver remains unchanged until activation. See [configuration, dependencies and validation limits](mpv-screen-development.md); the default build still uses GStreamer and disables the additional screen UI.
