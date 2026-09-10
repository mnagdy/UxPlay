# Qualifying mpv on Raspberry Pi 4

## 10 September 2026: 1080p60 output comparison

A generated H.264 High 1080p60/AAC fixture reproduced choppy playback through
the receiver with hardware copy-back. Standalone HDMI comparisons measured
about 50 output drops/second with either the debug overlay on or off. The mpv
fast profile reduced that to about 28–31. Adding PBO uploads or display-resample
timing did not improve it. Fast rendering plus `v4l2m2m` direct hardware output
reduced drops to about 1.8/second, with A/V timing at zero and the user confirming
very smooth picture and audible sound. All test children exited cleanly and the
receiver was restored. This is a substantial improvement, not lossless 60 fps
qualification or a long-run guarantee.

`-mpv-render-profile default|fast` now makes the tested rendering choice explicit.
The default leaves mpv's rendering defaults intact. Fast uses mpv's built-in
fast profile (simpler scaling and reduced processing); decoder selection remains
separate. The candidate Pi settings are:

```text
mpv-decode pi4-safe
mpv-h264-hwdec v4l2m2m
mpv-render-profile fast
```

HEVC hardware remains disabled. Validate these settings through the actual
receiver, including YouTube, HEVC software playback, seek/stop and switching,
before claiming production acceptance. Detailed local evidence is retained in
`.pi-dev/independent-h264/`.

## Earlier baseline and qualification procedure

The receiver's mpv adapter is initially a software playback baseline. The local
automated tests cover real mpv/FFmpeg over HTTP MP4, MPEG-TS HLS, fMP4 HLS,
separate HLS audio and software HEVC, using null video/audio outputs. They do
not establish Pi hardware decoding, visible HDMI picture, audible sound, UHF
compatibility, or 4K performance.

Pi 4 advertises H.264 decoding through 1080p60 and H.265 decoding through 4K60.
Those ceilings do not guarantee every profile, chroma format, bitrate or display
path. 4K H.264 exceeds the advertised H.264 hardware capability. Main10 decode,
10-bit output, HDR and tone mapping are separate qualifications.
[Pi 4 specification](https://www.raspberrypi.com/products/raspberry-pi-4-model-b/specifications/).

## Collect an inventory without changing playback

Run this from a copy of the project on the Pi:

```sh
./scripts/pi-mpv-check > mpv-inventory.json
```

The script only reports the model/kernel, package and media-library versions,
mpv executable hash, compiled decoder/hardware-method/output lists, and device
names/modes exposed by sysfs and ALSA. It does not start media, acquire a video
output, stop the receiver, install packages, change configuration or read source
URLs. Each subprocess has time/output limits. Missing components are recorded
as unavailable; `qualification: not-run` is intentional.

Check the following before choosing a playback command:

1. The model really is Pi 4 and the report comes from the intended service host.
2. mpv's runtime FFmpeg library versions and resolved paths match the intended
   Raspberry Pi OS package set. The standalone `ffmpeg` executable may use a
   different build.
3. H.264 V4L2 M2M support appears in the selected build. For Pi HEVC, investigate
   the actual V4L2 Request integration, including Pi buffer formats. A listed
   `hevc_v4l2m2m` decoder or generic `drm` hwdevice alone is not proof.
4. The DRM connector, card and ALSA device correspond to the working projector
   path. Sysfs modes are advertised modes, not proof that a mode is active or
   that mpv can acquire it under the receiver's service account.

Prefer evaluating matching distribution packages before building custom media
libraries. The official Pi distribution includes its own FFmpeg acceleration
patches; upstream and distribution capabilities differ.
[Pi FFmpeg packaging](https://github.com/RPi-Distro/ffmpeg/blob/pios/trixie/debian/rules),
[mpv DRM PRIME implementation](https://github.com/mpv-player/mpv/blob/master/video/out/hwdec/hwdec_drmprime.c).

## Prepare local fixtures and a controlled output window

Identify the exact codecs of the working UHF HD rendition and the failing
renditions. Keep resolution and codec separate: HD can be HEVC and 4K need not
be HEVC. Use local, non-sensitive fixtures with these characteristics first:

| Fixture | Purpose |
| --- | --- |
| H.264 8-bit 4:2:0 at the current working HD resolution, with AAC | Working regression baseline |
| HD HEVC Main, with AAC | Separate HEVC behavior from 4K throughput |
| HD HEVC Main10, where used by the real source | Check 10-bit buffer compatibility |
| 4K HEVC Main and Main10 at representative rates/bitrates | Establish the requested hardware target |
| Equivalent HTTP/HLS delivery of the passing fixtures | Separate decode/output from transport and demuxing |

Keep the active receiver release/configuration and its recovery procedure
available. These next tests take display/audio ownership: run them in a
controlled playback window after the previous receiver/player has released
those outputs. Use the actual receiver service account and environment; a
successful interactive login session alone is not the service qualification.

The following software-only example uses placeholder device and fixture names.
Replace them with the verified local values before running. It neither selects
a higher display mode nor implies software playback will keep up with 4K.

```sh
timeout --signal=TERM --kill-after=3s 45s mpv \
  --no-config --load-scripts=no --ytdl=no --hwdec=no \
  --vo=gpu --gpu-context=drm --gpu-api=opengl \
  --drm-device='/dev/dri/REPLACE_WITH_VERIFIED_CARD' \
  --drm-connector='REPLACE_WITH_VERIFIED_CONNECTOR' \
  --audio-device='alsa/REPLACE_WITH_VERIFIED_HDMI_DEVICE' \
  -- '/absolute/path/to/local-h264-aac-test.mp4'
```

`timeout` bounds ordinary userspace playback; it cannot release a process stuck
in uninterruptible kernel I/O. If termination fails, stop testing, preserve the
failure evidence and follow the existing recovery path. Do not start a second
player into a held device or repeatedly exercise a hanging decoder.

## Qualify hardware paths independently

For H.264, test only a method actually exposed by that mpv build. A candidate
replacement for `--hwdec=no` is:

```text
--hwdec=v4l2m2m-copy --hwdec-codecs=h264
```

Some matched builds expose `v4l2m2m` as a direct path instead. Neither name is
a universal recipe. Record the actual decoder and `hwdec-current`, including
software fallback, and verify picture, audio, seek and teardown. Copy-back may
reduce compatibility problems but increases memory traffic. It still invokes
the hardware decoder and its kernel driver.

After the exact combination passes qualification, the receiver can use:

```text
airplay-video-backend mpv
mpv-decode pi4-safe
mpv-h264-hwdec v4l2m2m-copy
```

Use the tested method, not the example by assumption. `pi4-safe` restricts
hardware selection to H.264; HEVC continues in software. Merely setting this
configuration is not evidence that hardware is active.

The receiver deliberately rejects `pi4-hevc-experimental` at present. There is
no verified HEVC hardware policy to enable in this implementation. A future
standalone feasibility experiment must select the correct API for the matched
build: existing Pi-patched FFmpeg commonly uses DRM integration, while the newer
dedicated `v4l2request` interface needs matching changes. Do not substitute
`hevc_v4l2m2m` or broad `--hwdec=auto-unsafe` for that investigation.
[mpv Request API work](https://github.com/mpv-player/mpv/pull/14690),
[Linux stateless decoder interface](https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html).

The previous HEVC stop failure involved the kernel decoder. A different player
can alter its trigger, but uses the same underlying hardware/driver. HEVC hardware
acceptance therefore requires repeated clean seek/stop/replacement, not just
successful first-frame decoding. If local HEVC/4K playback cannot pass, identify
the specific build, driver or display blocker before treating AirPlay integration
as the fix.

## Projector inventory, 10 September 2026

The Pi was reached at its current address after reboot. It is a Pi 4 Model B
Rev 1.2 running Debian 13 arm64 and kernel `6.18.34+rpt-rpi-v8`. The installed
mpv is `0.40.0-3+deb13u1`; its runtime FFmpeg libraries are the Pi distribution's
`7.1.5-0+deb13u1+rpt2` build. GStreamer is `1.26.2`.

- mpv lists `v4l2m2m` and `v4l2m2m-copy` for H.264, and `drm` and `drm-copy`
  for native HEVC. There is no method named `v4l2request` in this build's list.
- `/dev/dri/card1` is the `vc4-drm` display device. Its `HDMI-A-1` connector
  is connected and advertises modes through 1080p; `HDMI-A-2` is disconnected.
- The corresponding HDMI audio device is `alsa/plughw:CARD=vc4hdmi0,DEV=0`.
  The service account has the audio, video and render group access required
  for the listed devices.
- The service has no graphical-session environment or controlling terminal.
  The first user trial nevertheless acquired the output and displayed H.264
  video with the debug overlay. Repeated release and reacquisition remain
  separate checks.

The software preview selects `gpu`, `drm`, `opengl`, the above display and audio
devices, and `hwdec=no`. Its production IPC preflight accepted those options
and shut down cleanly without media or output acquisition. Eight targeted
native test groups also passed using headless outputs. The existing receiver
stayed running throughout those automated tests. The user then activated the
preview and confirmed that a previously working stream still plays. Their
photo records H.264 at 1280×720 and 25 fps, software decoding, AAC stereo at
48 kHz and ALSA output. They confirmed both picture and audible sound. This
establishes an H.264 regression baseline; it does not enable either hardware
policy. Retesting the previously failing streams still failed, so HEVC/4K
playback remains unresolved.

## Record each result

### H.264 hardware-copy trial, 10 September 2026

After the user reported choppy 1080p23.98 YouTube playback with software decoding,
five short native tests used the installed `v4l2m2m-copy` method and a generated
H.264 High 8-bit 1920×1080 24000/1001 fps file with AAC. Software fallback was
disabled in this test. All five observed actual `hwdec-current=v4l2m2m-copy`,
decoded 1080p `yuv420p` parameters, advancing time before and after seeking,
an explicit stop event, and exit zero after quit without TERM/KILL. One
unclassified warning occurred; no error or command failure was recorded.

These tests used null video/audio outputs while leaving the active receiver
alone. They establish a basis for a controlled receiver trial with
`mpv-decode pi4-safe` and `mpv-h264-hwdec v4l2m2m-copy`; they do not certify HDMI
rendering, audible audio, the real YouTube source or sustained operation.
That trial keeps HEVC in software. The previous software configuration remains
the rollback target. Confirm actual hardware selection in the overlay, then
compare the rate of newly accumulated output drops and the visible motion.
Do not compare cumulative drop totals from sessions with different durations.

Keep one record per exact kernel, mpv, loaded FFmpeg libraries and output stack.
Record fixture codec/profile/bit depth/chroma, resolution/rate/bitrate; requested
and actual decoder/hwdec; pixel format and output; dropped frames (unknown is
different from zero); advancing playback time; CPU/temperature/throttling;
first visible picture, first audible sound and A/V sync; and clean device release.
Use local fixture names in diagnostics. Do not save signed source URLs, headers,
keys or raw logs from private streams.

Initial acceptance gates:

- Known-good H.264 remains at least as reliable as the existing receiver.
- Enabled hardware is demonstrated by the actual decoder path and output,
  rather than inferred from a capability list or low CPU.
- Ten representative startups, twenty mixed seek/stop/replacement cycles and
  a one-hour mixed-codec soak complete without a kernel hang or orphaned player.
- Player stop releases DRM/ALSA, and the receiver can return to mirroring and
  audio-only playback. Verify both picture and sound physically.
- Equivalent HTTP/HLS fixtures pass after their local versions, then the actual
  supported UHF HEVC/4K streams pass from iPhone through the projector.

These are initial evidence gates, not a universal reliability guarantee.
Changing relevant media libraries or the kernel invalidates the corresponding
hardware qualification. The backend switch is independently useful, but the
4K/HEVC objective remains open until the actual device and UHF tests pass.

## Repeat the automated software tests

With the optional mpv test binary built, and `mpv`, `ffmpeg`, `libx264` and
`libx265` available:

```sh
python3 tests/run_mpv_backend_test.py /absolute/path/to/test_mpv_backend
```

The runner creates temporary fixtures, serves them over loopback HTTP with byte
ranges, and checks playback metadata, pause/resume, seek completion and shutdown
through the actual adapter. Video and audio go to null outputs. The runner
also checks that remote playlists cannot open local media files. The
adapter uses FFmpeg demuxing with an explicit network protocol allowlist;
arbitrary mpv playlist formats are outside the AirPlay video input contract.
The separate fake-child suite covers partial/out-of-order/malformed IPC, stale generations,
nonseekable/live status, deadlines, descriptor isolation and forced cleanup.
