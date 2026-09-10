# AirPlay video backend and HDMI feedback

Latest Pi update, 10 September 2026: `mpv-render-profile fast` with
`mpv-h264-hwdec v4l2m2m` substantially improves 1080p60 HDMI playback. The
independent AirPlay fixture now has user-confirmed smooth picture and sound
with the full debug overlay. Some output frames are still dropped; this is not
lossless 60 fps or long-run qualification. The render-profile option defaults
to `default`, and hardware decoding remains restricted to H.264. See
[Pi qualification](pi-mpv-qualification.md) for the controlled comparisons.
The implementation history below records the earlier software-only baseline.

The same YouTube 1080p60 source was subsequently retested on the deployed
release `20260910T193744320774Z-52c2997e3800-dirty`; the user confirmed smooth
picture and sound. The live capture showed direct H.264 hardware output,
audio/video synchronization at zero and about 7.9 output drops/second over a
62-second interval, compared with about 44.5/second before. HEVC software
playback, H.264 seeking and clean child shutdown also passed the targeted
regression checks. The earlier failing UHF HEVC source remains a separate issue.

Implementation status: development build, 10 September 2026. The optional mpv backend and HDMI status/debug modes are implemented, built on the projector's Raspberry Pi, and tested there with software decoding and headless outputs. The user activated the preview and supplied a photo confirming a working H.264 picture and debug overlay. Hardware decoding, HEVC/4K and full playback acceptance remain unqualified. No OS image is created.

## Build and select

GStreamer remains the default and continues to handle screen mirroring and RAOP audio-only sessions. Selecting mpv sends direct AirPlay video, including its associated audio, through mpv/FFmpeg. Both direct HTTP media and the receiver's cached YouTube HLS route use that selection.

The optional build requires mpv at runtime and json-c development headers at build time. The status renderer also requires GStreamer's `textoverlay` plugin and a font. On the Debian test environment these are supplied by `mpv`, `libjson-c-dev`, `gstreamer1.0-x` and `fonts-dejavu-core`, in addition to the existing UxPlay dependencies. A Pi's media packages must be assessed against its kernel and graphics stack; installing a generic mpv package does not establish hardware decoding support.

```sh
cmake -S . -B build/mpv -DNO_X11_DEPS=ON -DUXPLAY_ENABLE_MPV=ON
cmake --build build/mpv --parallel 2
```

The existing Pi development helper can prepare an optional build without activating it:

```sh
./scripts/pi-dev build --with-mpv
```

It records the option in release metadata. A later build without `--with-mpv` explicitly disables the option, including in a reused CMake cache. `deploy --with-mpv` follows the existing service activation and rollback procedure; activation interrupts playback and requires the normal Pi service authorization.

Add these lines to the receiver configuration selected by `-rc`:

```text
airplay-video-backend mpv
mpv-decode software
screen-info debug
```

Change the first value to `gstreamer` and restart to compare the existing backend. `screen-info status` shows normal receiver feedback; `screen-info off` disables the added display work. Both switches also accept command-line flags, which override configuration. Global defaults remain `gstreamer` and `off`.

On a Pi running directly on HDMI, mpv has its own output settings. Existing GStreamer `-vs`, `-vsync`, sink options and `-as` do not configure mpv:

```text
mpv-vo gpu
mpv-gpu-api opengl
mpv-gpu-context drm
```

Use `mpv-drm-device`, `mpv-drm-connector` and `mpv-audio-device` only after identifying the actual device and output. `mpv-executable` can select a particular installed build. The selected executable is checked through a private IPC connection at receiver startup, without loading media or opening playback outputs. An unavailable or incompatible backend fails explicitly; it does not silently select GStreamer.

## Pi 4 decoding

| Policy | Current behavior |
| --- | --- |
| `software` | Uses `hwdec=no`. This is the default mpv development baseline, including for HEVC. It makes no promise of real-time 4K performance. |
| `pi4-safe` | Requires an explicitly qualified `mpv-h264-hwdec` setting, currently `v4l2m2m` or `v4l2m2m-copy`. Hardware decoding is restricted to H.264; HEVC remains software. The local mpv build must actually support the selected method. |
| `pi4-hevc-experimental` | Reserved but deliberately rejected at startup until standalone hardware playback and shutdown are qualified on this Pi. |

The existing `hls-pi4` cached-YouTube H.264/AAC-LC selection remains available. It does not transcode UHF media or turn a direct 4K H.264 stream into HEVC. See [Pi qualification](pi-mpv-qualification.md) for the separate device checks. The implementation gives us a second decoder/demuxer path to diagnose the current HEVC/4K failures; it does not certify those failures fixed.

## Screen behavior

The idle screen identifies the receiver and reports startup, network availability and readiness. Playback requests enter preparing/opening states before media starts. The active player reports buffering, playing, paused, seeking, stopping and failures. An error remains visible after output release until a new request arrives.

Debug mode adds available codec, size, frame rate, requested decode policy, observed decoder/hardware state, sink progress, audio format/output, volume, timing and overlay support. Missing measurements say Unknown. mpv progress reflects advancing reported playback position, not a measured HDMI frame count. Its first timestamp is only a baseline. Its revised panel uses actual short decoder names and pixel format, buffered seconds, audio position, A/V timing and separate decoder/output drop counters. GStreamer counters are observations at input, decoder and sink stages; they cannot establish that the projector displayed a frame or that speakers were audible.

The mpv adapter collects warning and error reports by a fixed set of player modules through its existing private IPC connection. Raw log text and arbitrary module names are discarded. Module counts identify where errors were reported; they are not proof of a fatal failure or its root cause. Fixed end-file reasons, recognized mpv API error codes and child exit status are retained through shutdown. A debug trace records state changes and a bounded measurement summary every two seconds while a child is active. Debug mode saves typed records in a private on-disk history with one 1 MiB file and one rotated file; the report exporter includes this history after reboot. Raw protocol logs are excluded. Fatal details remain on the idle error screen until a new request arrives. The [logging audit](playback-logging-audit.md) records verified gaps, the revised capture fields, privacy boundaries and the measurements still unavailable in stock mpv.

The idle presenter owns one GStreamer video output and no audio output. Before playback acquires HDMI, the idle presenter releases it. Before returning to idle or mirroring, mpv must exit and be reaped. A failure to release output prevents a competing renderer from starting. No global terminal-login/getty configuration is changed by this patch.

mpv uses its native OSD. The GStreamer overlay currently blends into writable SystemMemory frames at the sink, leaving decoder selection and upstream memory negotiation unchanged. Unqualified DMABuf/GL memory paths are reported in diagnostics; the overlay does not force a hardware pipeline into software to make text visible. A paused GStreamer picture cannot show a new label until another video frame arrives. These limitations require HDMI testing on the Pi, as does any brief blank interval during DRM ownership handover.

Readiness checks include a usable network interface, the running listener and successful initial discovery registration. This version has no callback confirming a later loss of the mDNS registration; real phone discovery remains a required acceptance check.

## Stream replacement and controls

mpv runs as a supervised child with a private socketpair. Media URLs are sent in escaped JSON commands, not shell commands or process arguments. The player does not load user scripts, save resume state, or emit raw media logs to the receiver. New screen fields use bounded, sanitized labels.

Scoped HLS cache URLs identify a specific cache lifetime and accept loopback requests only. An old URL cannot return a new stream's manifest. Request IDs and reverse-channel routing distinguish outstanding cached playlist fetches. Controls from a different Apple session are rejected; commands that reuse the same Apple session ID carry no separate stream generation on the wire, so an indistinguishable delayed command cannot be identified reliably.

Pause/resume, seek, stop, volume and playback-info use the selected backend. Unknown duration is valid for live media and is not treated as end-of-stream. Ordinary rejected seeks remain control errors rather than stopping playback. Replacement waits for the previous child to exit before starting its successor. Startup, load, seek and child shutdown have bounded supervision; a kernel driver stuck in uninterruptible sleep still requires device recovery.

Mirror/audio setup callbacks still wait synchronously, for at most six seconds, for an active mpv child to release its outputs and for mirror pipelines to be rebuilt. The main loop continues supervising the child during that wait. These setup callbacks can temporarily occupy a protocol worker; fully asynchronous setup would require a separate RTP startup/buffering change. Direct-video play and control callbacks enqueue work without doing player I/O.

## Validation

```sh
cmake -S . -B build/tests -DNO_X11_DEPS=ON \
  -DUXPLAY_BUILD_TESTS=ON -DUXPLAY_ENABLE_MPV=ON
cmake --build build/tests --parallel 2
ctest --test-dir build/tests --output-on-failure
python3 -m unittest discover -s tests -p 'test_pi*.py'
```

The Linux test suite covers status state/generation handling, real headless status and video-overlay rendering, no audio owner in the idle presenter, GStreamer decoder/memory preservation, fake mpv IPC faults and child lifetimes, receiver callback/ownership transitions, stale cache/control requests, and actual mpv HTTP MP4, MPEG-TS HLS, fMP4 HLS, separate audio HLS and software HEVC. Actual mpv media tests use null video/audio outputs.

Before activation is considered successful, test the actual phone-to-projector path: idle HDMI feedback, UHF HD H.264, the failing HEVC and 4K examples, YouTube, mirroring and audio-only AirPlay; picture and sound separately; pause/seek/resume; channel/phone switching; disconnect and stop; clean receiver shutdown and subsequent device reacquisition. Compare debug on/off and retain the previous release/configuration for rollback.

Observed verification on 10 September 2026: both optional-enabled and default-disabled builds compiled in an aarch64 Debian Trixie container (GStreamer 1.26.2, mpv 0.40.0, FFmpeg runtime 7.1.5, json-c 0.18). All 19 CTest groups passed in the enabled build; six relevant screen/audio/cache/direct-renderer groups also passed in the disabled build. All 21 local deployment-helper tests passed. Focused model and mpv adapter suites passed AddressSanitizer/UndefinedBehaviorSanitizer checks. Actual idle/status/error and video-overlay frames were rendered and visually inspected.

After reconnecting to the Pi's current IP, both default and optional-enabled native release builds passed on kernel `6.18.34+rpt-rpi-v8`. Eight native CTest groups passed: status model, idle renderer, video overlay, mpv adapter, receiver handover, audio observations, scoped HTTP cache and actual mpv HTTP playback. The latter includes software HEVC at a small fixture resolution; it is not a 4K throughput test. Tests used headless outputs and disabled GStreamer hardware decoder selection. The connected HDMI output advertises up to 1080p, so decoding a 4K input and downscaling to that display is a separate objective from 4K HDMI output.

The user subsequently activated release `20260910T162215921580Z-52c2997e3800-dirty`; its running executable hash and mpv child were verified with no automatic receiver restarts. Their photo shows direct HTTP H.264 at 1280×720, 25 fps, software decoding, an advancing position and a visible debug overlay. AAC stereo at 48 kHz and ALSA output are reported, and the user explicitly confirmed both picture and sound. The decoder drop counter reads zero in that snapshot; this is not an end-to-end HDMI drop measurement. The user also retested the previously failing streams and reported that they remain broken. The HEVC/4K objective remains unresolved; return to idle and switching are separate acceptance checks.

The failing-stream photo identifies HEVC at 1280×720 and 25 fps, software decoding, position 0.0 seconds and no reported audio format/output; this alone does not establish why audio initialization is missing. The user observed a grey/blocky first frame, followed by no moving video or sound. The screen's "Waiting for stream data" state detects a lack of advancing playback; it does not independently establish a network failure. A two-second process sample found mpv at about 1% CPU with sleeping threads and no observed kernel wait hang. This does not support CPU saturation during that sample.

Separate native tests passed for generated 720p25 HEVC Main and Main10 with AAC: both MP4 files and all four combinations of bit depth with MPEG-TS/fMP4 HLS over loopback HTTP. These used software decoding, null outputs and the adapter's demuxer/protocol settings. They establish basic decoder and HLS capability for synthetic clips, not compatibility with the actual failing stream or its physical output path. The diagnostic revision addresses missing evidence and misleading decoder labels; it does not claim to fix that stream.
