# Pi 4 HEVC 4K60 research — 10 September 2026

The Pi 4 specification includes HEVC 4Kp60 decoding. Our measured 4K60 failures
do not establish the decoder's maximum throughput: the direct-DRM null-output
benchmark never initialized hardware. The highest-priority investigation is
the path from decoded buffers to the display, with a separate decoder benchmark.
[Raspberry Pi specification](https://www.raspberrypi.com/products/raspberry-pi-4-model-b/specifications/)

## Relevant evidence

A December 2023 mpv report describes Pi 4 4K60 Main 10 drops while the reporter
obtained better results with FFmpeg direct DRM output and other players.
Maintainers discussed the cost of shader rendering and importing the Pi's
unusual frame layouts. This is a close precedent, not a confirmed diagnosis:
the report used older packages and HDR; our generated clips are SDR.
[Issue 13055](https://github.com/mpv-player/mpv/issues/13055),
[maintainer's GL-import observation](https://github.com/mpv-player/mpv/issues/13055#issuecomment-1848796558)

The mpv manual explicitly describes an alternative plane arrangement for 4K:
video on the primary plane, OSD on an overlay, and an OSD surface smaller than
the video. Our rejected overlay trials used the opposite default assignment.
Swapping planes is a documented candidate; it alone does not establish the
cause of EINVAL. The new `overlay-primary` harness path exercises it.
[mpv 0.40 manual](https://github.com/mpv-player/mpv/blob/v0.40.0/DOCS/man/vo.rst)

A Raspberry Pi engineer's May 2026 post documents the Pi OS Trixie/6.18 FFmpeg
DRM test route. It runs unpaced, so successful output alone does not prove
real-time A/V playback. The discussion concerns Pi 5; use the method as a
diagnostic and verify its behaviour on this Pi 4.
[engineer's post](https://forums.raspberrypi.com/viewtopic.php?p=2378371)

The RPi FFmpeg maintainer also documents this direct-output route for Pi 4,
describing it as development/test infrastructure rather than a complete player.
[RPi FFmpeg issue 9](https://github.com/jc-kynesim/rpi-ffmpeg/issues/9)

## Current installed capabilities, checked without playback

- mpv `0.40.0-3+deb13u1`.
- FFmpeg `8:7.1.5-0+deb13u1+rpt2`.
- Mesa EGL/DRI `26.2.1-2~bpo13+0~rpt1`.
- libdrm `2.4.134-3~bpo13+1+rpt1`.
- FFmpeg has `vout_drm`, default codec `wrapped_avframe`, and a `show_all` option.
- mpv includes `dmabuf-wayland`, `gpu`, `gpu-next`, and its software-scaling
  `drm` output. The latter must not be confused with `gpu-context=drm`.

## Recommended experiments

1. **Reference direct presentation with the installed Pi FFmpeg.** Adapt the
   maintainer's `-no_cvt_hw -hwaccel drm ... -f vout_drm` test to our generated
   Main/Main 10 files. Own the display through the existing service-stop and
   restoration guard. Record hardware formats, submitted-frame throughput,
   diagnostics and shutdown. Submitted frames are not independently counted
   HDMI presentations. Its unpaced video-only result is a diagnostic, not an
   AirPlay or audio-sync qualification. Independently establish decoder speed
   using explicit hardware frames and a compatible output; do not repeat the
   mpv null-output test and interpret failed initialization as slow decoding.

2. **Compare mpv plane assignments.** Keep `vo=gpu`, `gpu-context=drm`,
   `hwdec=drm`, `gpu-hwdec-interop=drmprime-overlay`, and `profile=fast`.
   Test these additional settings through a guarded new harness case:

   ```text
   --drm-drmprime-video-plane=primary
   --drm-draw-plane=overlay
   --drm-draw-surface-size=1280x720
   ```

   The 720p surface is our adaptation to the current HDMI mode; the manual's
   4K-output example uses 1080p OSD. First inspect plane format/modifier and
   stacking support. If atomic commits still fail, collect the exact rejected
   properties and kernel diagnostics. Do not use zero frame-drop counters as
   proof of presentation.

3. **Use a reference player if those paths remain inconclusive.** LibreELEC/Kodi
   is a useful comparison because its maintained Pi stack supports hardware
   HEVC and 4K output. A separate boot medium would preserve the working
   receiver. It would be a qualification reference, not a decision to replace
   the AirPlay architecture.
   [LibreELEC Pi guidance](https://wiki.libreelec.tv/raspberry_pi_faq)

`dmabuf-wayland` is another candidate, but compositor support for the actual
Pi frame formats/modifiers must be established first. The old mpv discussion
contains both suggestions to use it and subsequent format-support failures;
it is not a verified drop-in fix for this installed system.

## Boundaries

The `hdmi_enable_4kp60` setting enables native 4K60 HDMI modes on the Pi 4's
HDMI0 output. It does not establish smooth decoding/downscaling at the already
active 720p60 mode. Native 4K HDMI also needs a suitable display and cable.
[Raspberry Pi display configuration](https://www.raspberrypi.com/documentation/computers/config_txt.html#hdmi_enable_4kp60)

Our current evidence is zero-drop 4K30 Main/Main 10, including a two-minute
Main 10 loop, versus substantial 4K60 drops. No active throttling was observed.
Blind clock, memory-split or package changes are not the first experiment.
Seeking and teardown still require qualification because the earlier HEVC
driver hang is distinct from presentation throughput. There is a related
open 6.18 accurate-seek report; it does not demonstrate that changing kernels
will fix our current performance problem.
[Pi kernel issue 7537](https://github.com/raspberrypi/linux/issues/7537)

## Follow-up experiments

The installed helper now exposes `--path ffmpeg-drm` and
`--path overlay-primary`; runnable commands and measurement boundaries are in
[the test plan](hevc-4k-test-plan.md). No packages or persistent receiver
settings were changed. Each playback experiment temporarily stops the receiver
and checks restoration afterward.

The first FFmpeg smoke report (`20260910T214543-bdbde5.json`) was rejected by
two parser assumptions: verbose output inserts `1 reference frame` before
`drm_prime`, and its success summary says `0 decode errors`. The parser now
recognizes that hardware-output format and only exempts the exact zero-error
count. Nonzero decode errors and other failures remain failures. Linux tests
exercise these cases. The subsequent six smoke cycles passed
(`20260910T214650-1b5e9a.json`).

The 4K60 Main FFmpeg benchmark submitted all 1,200 frames and exited normally
in 20.894 seconds (57.43 submitted FPS including startup), but logged
`create_dst_bufs: Failed to create V4L2 buffer`. It remains a failed diagnostic
run (`20260910T214724-e08276.json`), despite the final zero-decode-error count.
There were no current throttle flags or new kernel-journal entries in the
inspected ten-minute window. This is not a clean decoder-speed measurement.

Live KMS inspection during the primary-plane smoke test confirms a Main 10
P030 video framebuffer on primary plane 91 at immutable zpos 0, and an AR24 OSD
framebuffer on overlay plane 127 at zpos 1. The overlay plane's allowed zpos
range is 1–17. This supports investigating the default video's attempted zpos
0 as the earlier rejection cause; it is not a captured kernel explanation of
the failed atomic request.

The revised mpv path subsequently passed all six smoke cycles and both 4K60
Main/Main 10 display trials with zero recorded output/decoder drops, no atomic
commit failures and 0.5 ms p95 A/V difference. During the 4K60 Main 10 loop,
KMS inspection showed the 3840×2160 P030 framebuffer on primary plane 91 scaled
to 1280×720, with the separate AR24 OSD on overlay plane 127. The viewer
confirmed smooth picture and audible tone during that loop. This is evidence
that the revised path handles this synthetic 4K60 source at the existing HDMI
mode; it does not yet qualify AirPlay integration, UHF, seeking or native 4K
HDMI output.

The two-minute loop completed with normal shutdown and receiver restoration
(`20260910T215026-06f441.json`). Steady playback had zero output/decoder drops,
zero clock stalls and 0.5 ms p95 A/V difference. Five output drops occurred
during initial startup, excluded from steady metrics. No current throttling
was observed. Retain that startup distinction when reporting the result.
