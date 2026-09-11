# Playback findings and remaining work

Consolidated 10 September and updated 11 September 2026 from retained investigations and test
records. These are recorded observations, not a new live-device check. Current
release information belongs in [FORK.md](../FORK.md); local reports and generated
media are mapped in the [local evidence index](local-evidence.md).

## Unresolved playback problems

**UHF audio-only works, but some video streams fail or have no sound.** Working
H.264 streams and failing 720p25 HEVC streams must remain separate comparisons;
this is not solely a 4K throughput problem. In the GStreamer investigation, UHF
explicitly stopped its ALAC transport before requesting video. Three complete
HEVC fragments then contained no real compressed AAC, and no decoded or sink
audio appeared. GAP repair allowed video to advance but cannot create missing
sound. YouTube subsequently produced real AAC, PCM and audible sound. See the
[complete audio trace](diagnostics/2026-09-10-uhf-audio.md#current-candidate-activated-silent-uhf-reproduced-with-complete-audio-trace).

The later mpv failure had HEVC video cache growing while playback stayed at zero
and the selected AAC track never initialized. Its completed 30.068-second startup
capture counted 1,250 selected video packets and zero selected audio packets.
These are demux queue observations, not a capture proving what the broadcaster
sent. Source audio, UHF conversion/selection and FFmpeg extraction remain possible
failure stages. Preserve the same failing item for comparison with known-good
H.264 and the generated HEVC fixtures. See [logging and interpretation limits](playback-logging-audit.md)
and the local evidence index for the packet reports and synthetic comparison.

The [11 September Sky News export inspection](diagnostics/2026-09-11-sky-news-export.md)
subsequently found 750 HEVC samples and zero audio samples in three complete
phone-served fragments spanning 30 seconds. Every media-payload byte belonged
to video, although AAC was advertised; the user confirmed sound inside UHF.
For this sample, the omission therefore precedes Pi parsing/decoding. A later
[UHF debug-log analysis](diagnostics/2026-09-11-uhf-debug-analysis.md) records AAC
selected, input-audio timestamps and 18 exporter write errors. These are separate
attempts and support a UHF/iPhone export-writing problem, without establishing
which component causes it. AAC ADTS-to-ASC conversion is a specific unproved
hypothesis. A UHF developer report or focused export-setting comparison remains
the next step; no receiver fix or general app compatibility is established.

**Channel 4 compatibility remains unresolved; its encryption method is unknown.**
Its observed GStreamer failure preceded media reaching parsebin and differed from
UHF's absent audio. Synthetic tests reproduced the same NULL-buffer assertions
when an AES-128 key download failed or returned no data. The isolated 1.26.2 patch
requires a successful response and exactly 16 key bytes; six encrypted cases and
normal/missing-audio/replacement comparisons passed. It is not installed by the
receiver build and does not provide missing keys or protected-service DRM support.
The actual Channel 4 playlist encryption, key format and HTTP result still need
evidence. Do not describe this as a confirmed FairPlay limitation or a confirmed
Channel 4 fix. Keep the [Channel 4 investigation](diagnostics/2026-09-10-uhf-audio.md#additional-channel-4-attempts)
and [patch with rerun instructions](../patches/README.md).

**Initial YouTube startup still varies.** The replacement bug is fixed in the
recorded user test: the old video remains paused and switching is fast. One
initial start still took eight seconds, with most of that delay after playlist
preparation and while starting at a nonzero position. Compare fresh connections
at zero and at a resume position before changing seek/network policy. The
[startup record](startup-switch-regression.md) preserves the measurements,
release identities and rollback boundaries.

## Pi, GStreamer and display lessons

The tested device is a Pi 4 Model B Rev 1.2, Debian 13 arm64, kernel
`6.18.34+rpt-rpi-v8`, GStreamer 1.26.2, mpv 0.40.0, and Raspberry Pi FFmpeg
7.1.5. The recorded display is `vc4-drm` on `/dev/dri/card1`, connector
`HDMI-A-1`, with ALSA `plughw:CARD=vc4hdmi0,DEV=0`. Its **active HDMI mode is
1280×720 at 60 Hz**, although the connector advertises modes through 1080p.
Recheck addresses, card numbering and active mode before a new test. The
read-only `scripts/pi-mpv-check` inventories packages, libraries and devices.

| Observed problem | Resolution or boundary to preserve |
| --- | --- |
| Stock UxPlay rejected UHF's `/stream.m3u8` URL | Upstream direct HTTP/HTTPS backport removes the YouTube-only URL-shape restriction. The original stable and separately installed UHF binaries remain historical recovery paths. |
| YouTube reused incomplete caches or referenced unavailable playlists | Require a complete usable cache, correlate FCUP responses, omit unavailable variants and required audio dependencies, and reject an empty playable result. |
| A mixed H.264/VP9 YouTube master failed in GStreamer 1.26.2 | `hls-pi4` selects declared H.264/AAC-LC through 1080p60. This is source selection, not transcoding, and does not alter UHF's direct media. |
| Repeated `kmssink` startup cost 3.6–4.0 seconds | Pi profile selects `driver-name=vc4` unless a driver/device was explicitly supplied. Controlled initialization fell to 155 ms; sender/network time is separate. |
| Empty declared AAC kept adaptive HLS at 0% while video accumulated | Conservative fMP4 interval/fragment-completion GAP repair lets video progress while preserving later real audio. The direct-HTTP Pi path uses a three-second low watermark; cached YouTube keeps its separate policy. Neither change repairs an absent soundtrack. |
| Generic `fakesink` appeared to reject hardware HEVC | `fakevideosink` supports the needed metadata/DMA-DRM negotiation. It rendered the sample, and the receiver's real `kmssink` also decoded it. A parallel `kmssink` test failed with EACCES because the receiver already owned DRM. |
| Real UHF HEVC stop wedged the kernel in `hevc_d_h265_stop` | Exclude the exact GStreamer `v4l2slh265dec` factory under `hls-pi4`, retaining H.264 hardware. A physical power cycle was required; application restart could not release the blocked kernel operation. Later mpv success does not establish that all driver seek/stop triggers are fixed. |
| Full RAOP feature mask in HTTP `/server-info` broke working AirPlay | iOS requested unsupported HTTP `/fp-setup` and never reached `/play`. Restore legacy HTTP `0x27F`; sharing masks needs the full additional handshake and key-consumer path. HTTP `/fp-setup2` remains an unsupported 421 response. |

The [9 September investigation](diagnostics/2026-09-09-uhf.md) and
[development guide](development-on-pi.md) retain exact regression behavior and
timing. Relevant implementation references are [GStreamer DRM discovery](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gst-plugins-bad/sys/kms/gstkmssink.c#L475),
[stateless HEVC negotiation](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gst-plugins-bad/sys/v4l2codecs/gstv4l2codech265dec.c#L374),
and the [related Pi HEVC driver report](https://github.com/raspberrypi/linux/issues/7537).
The researched beta-kernel candidate was not installed; there is no evidence
that a kernel upgrade resolved this device's failure.

## What improved HEVC and H.264 output

H.264 1080p60 was choppy with software or hardware copy-back output. Standalone
copy-back comparisons dropped about 50 output frames/second with debug both on
and off. The fast profile alone reduced this to about 28–31; PBO uploads and
display-resample did not help. **`mpv-h264-hwdec v4l2m2m` with
`mpv-render-profile fast`** reduced the measured rate to about 1.8/second and the
user confirmed very smooth picture and sound. A subsequent real YouTube test
was also visibly smooth but recorded about 7.9 output drops/second over 62
seconds. Different runs are not interchangeable; neither result guarantees
lossless 60 fps. The retained [mpv guide](mpv-screen-development.md) records the
receiver comparison; `.pi-dev/independent-h264/` holds its generated media.

HEVC's improvement came from **direct display-plane output**, not a demonstrated
decoder-speed limit or a clock/memory tweak. Ordinary GPU rendering dropped
4K60 frames, particularly Main 10. The initial DRM-overlay arrangement falsely
appeared to pass: mpv reported atomic frame-commit rejection only as warnings
while its drop counters stayed zero. Such rejection now invalidates a result
regardless of warning severity or zero counters.

The passing standalone arrangement was:

```text
--hwdec=drm
--gpu-hwdec-interop=drmprime-overlay
--drm-drmprime-video-plane=primary
--drm-draw-plane=overlay
--drm-draw-surface-size=1280x720
```

It used `vo=gpu`, DRM/OpenGL and the fast profile. KMS inspection confirmed a
3840×2160 P030 video framebuffer on primary plane 91 at immutable zpos 0,
scaled to 1280×720, plus AR24 OSD on overlay plane 127 at zpos 1. The overlay's
allowed zpos range was 1–17. This supports a plane-order explanation for the
earlier rejection; it is not a captured kernel explanation of the failed commit.
See the [mpv 0.40 video-output reference](https://github.com/mpv-player/mpv/blob/v0.40.0/DOCS/man/vo.rst).

The two-minute Main 10 4K60 loop had user-confirmed smooth picture and tone,
zero steady decoder/output drops and 0.5 ms p95 A/V difference; five startup
output drops were excluded from steady metrics. Shutdown and receiver restoration
were clean. A direct-DRM null-output benchmark never initialized hardware and
proves no speed limit. The FFmpeg 4K60 benchmark submitted 1,200 frames but
reported `Failed to create V4L2 buffer`; it remains a failed diagnostic result.

The receiver's opt-in `pi4-hevc-experimental` policy integrates this arrangement,
tries `drm` before `v4l2m2m`, restricts hardware to HEVC/H.264 and disables runtime
software fallback. The ordering avoids unsupported stateful HEVC probes while
retaining H.264's `h264_v4l2m2m` path. Generated AirPlay 4K60 Main 10 and 1080p60
H.264 smoke tests passed with clean stops; the user also confirmed YouTube and
iPlayer. Keep the [HEVC AirPlay trial](hevc-airplay-trial.md) and
[qualification/rerun guide](hevc-4k-test-plan.md). **Actual failing UHF streams,
HEVC seeking and extended mixed playback remain unqualified.** Main 10 does not
mean HDR, and these results do not establish native 4K HDMI output.

Pi 4's published ceilings are H.264 1080p60 and HEVC 4K60, not guarantees for
every profile, bitrate or output stack. A lower HDMI mode still requires decoding
the full source; remuxing cannot turn unsupported 4K H.264 into HEVC. Mirror
resolution/frame-rate settings do not constrain direct video.
[Pi 4 specification](https://www.raspberrypi.com/products/raspberry-pi-4-model-b/specifications/)

## Repeat the independent HEVC baseline

Keep `.pi-dev/independent-hevc/` intact. Its generated 45-second moving-pattern
fixture is HEVC Main 1280×720/25, 8-bit YUV420, with quiet 440 Hz stereo AAC-LC
at 48 kHz. MP4 and remuxed TS each contain 1,125 video and 2,111 audio packets.
Both HTTP AirPlay trials had user-confirmed moving picture and tone with software
HEVC; positions reached 38.08 and 38.30 seconds after 40 seconds respectively.
They exited cleanly on release `20260910T185438654187Z-52c2997e3800-dirty`, receiver PID 7006,
sessions 6 and 7. These are independent-source results, not UHF compatibility.

Discover the current `_airplay._tcp` port with `dns-sd -L Projector
_airplay._tcp local.` and stop discovery after receiving the result. From UxPlay,
replace the placeholders with that address and port:

```sh
python3 scripts/airplay-fixture-send --host PI_ADDRESS --port AIRPLAY_PORT \
  --file .pi-dev/independent-hevc/hevc-720p.mp4 --seconds 40 \
  --output .pi-dev/independent-hevc/mp4-repeat.json

python3 scripts/airplay-fixture-send --host PI_ADDRESS --port AIRPLAY_PORT \
  --file .pi-dev/independent-hevc/hevc-720p.ts --kind ts --duration 45 --seconds 40 \
  --output .pi-dev/independent-hevc/ts-repeat.json
```

The helper serves only the supplied trusted file under a random path, replaces
active video, and stops its session afterward. It requires mutual LAN reachability
and this fork's binary-plist/session-control support. TS is paced by average
bitrate with a two-second probe allowance and no advertised content length; it
does not reproduce broadcaster timing/corruption. This is direct HTTP AirPlay,
not an iPhone sender, screen mirroring or YouTube's negotiated cache. Evidence
files include `fixture-metadata.json`, `mp4-result.json`, `ts-result.json`,
`combined-report.json` and `receiver-summary.json` in that fixture directory.

## Architecture and acceptance work still worth keeping

The selected architecture is an iPhone-controlled Pi output endpoint. The older
Kodi/LibreELEC setup was superseded. UxPlay owns discovery, protocol negotiation,
source preparation and controls; one direct-video backend owns both media audio
and video. GStreamer continues mirroring/audio-only. Keep backend selection,
device policy, output ownership and bounded diagnostics separate. A second RAOP
audio clock cannot repair silent HLS. Original binaries/configurations and managed
release snapshots provide rollback; [development instructions](development-on-pi.md)
explain `rollback` versus restoring the pre-development `stable` command.

Remaining acceptance work beyond the two main app failures:

- Verify HEVC keyframe/accurate seeking, repeated stop/replacement and a longer
  real-stream run without kernel waits or orphaned players. Earlier proposed
  qualification gates were ten startups per representative stream, twenty mixed
  control cycles and a one-hour mixed-codec soak; these are unfinished targets.
- Verify video ↔ mirroring ↔ audio-only, phone lock/background, reconnect,
  two-phone handover, long seeks and volume. Controls with the same reused Apple
  session ID carry no distinct wire generation, so an indistinguishable delayed
  control cannot always be identified.
- Check idle/loading/error/return-to-ready HDMI screens, projector off/on,
  stop/crash recovery, blank or console flashes during ownership transfer and
  debug-on/off overhead. GStreamer hardware-memory overlay parity and paused-frame
  updates remain limited; no dedicated graphics VT/getty change was implemented.
  Later mDNS registration loss has no confirmation callback in the current design.
- Preserve the three [logging audit gaps](playback-logging-audit.md#audit-across-the-playback-paths):
  GStreamer evidence after first output, explicit coordinator/renderer generation
  binding and observer reset, and structured generation-checked RAOP bus errors.
  Keep typed packet counts distinct from source/network, decode and physical output.

Record exact kernel, player/libraries, fixture hashes, active display mode,
requested and observed decoder, debug mode, picture and sound separately, drops,
A/V timing, throttle flags and clean exit/restoration. A passing build, healthy
service PID or first sink buffer alone does not qualify the phone-to-projector
workflow. Keep media and private reports local; avoid signed URLs, credentials,
keys and raw protocol logs in shared records.

## Rebuild headless regression tools

The old local `build/` directories were generated Linux/container outputs and
were removed during cleanup. Rebuild from the maintained CMake definitions and
[development guide](development-on-pi.md); the historical builds used Debug,
`UXPLAY_BUILD_TESTS=ON` and `NO_X11_DEPS=ON`. Sanitizer builds added
`-fsanitize=address,undefined -fno-omit-frame-pointer` to C/C++ flags.

For an isolated headless GStreamer comparison, disable hardware decoders only
in that test process so it does not compete with the running receiver. Replace
the executable path with the newly built runner and pass the appropriate fixture
arguments from the maintained tests:

```sh
GST_PLUGIN_FEATURE_RANK=v4l2h264dec:0,v4l2slh265dec:0 \
  /absolute/path/to/current/test_direct_renderer_hls --pi4 ...
```

This preserves the old software-only wrapper's purpose without a stale fixed
remote build path. It is not a hardware-output or receiver acceptance test.
