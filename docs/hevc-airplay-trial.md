# HEVC hardware AirPlay trial — 10 September 2026

The opt-in `mpv-decode pi4-hevc-experimental` policy now enables the tested
Pi 4 HEVC output arrangement in the receiver. `software` and `pi4-safe` retain
their prior behavior. GStreamer still owns mirroring and audio-only AirPlay.

Required settings alongside the existing DRM device/connector and ALSA device:

```text
airplay-video-backend mpv
mpv-decode pi4-hevc-experimental
mpv-h264-hwdec v4l2m2m
mpv-render-profile fast
mpv-vo gpu
mpv-gpu-context drm
mpv-gpu-api opengl
screen-info debug
```

The backend passes `--hwdec=drm,v4l2m2m` with hardware decoding restricted to
H.264 and HEVC. The installed native HEVC decoder supports DRM; native H.264
does not, so H.264 retains `h264_v4l2m2m`. DRM is tried first to avoid probing
the unsupported stateful HEVC decoder. The policy uses `drmprime-overlay`,
video on the primary plane, and a separate 1280×720 OSD overlay. Runtime
software fallback is disabled, and actual decoder/format remains visible in
diagnostics. Other unsupported codecs may still decode in software.

mpv documents ordered hardware-decoder lists in its
[0.40 options reference](https://github.com/mpv-player/mpv/blob/v0.40.0/DOCS/man/options.rst).
The plane arrangement is based on the independently measured
[qualification results](hevc-4k-test-plan.md).

Atomic frame-commit rejection now records `frame-present-failure` and counts
as a video-output error even when mpv emits it at warning level. This avoids
interpreting a zero drop counter as proof of successful display submission.

The trial release was `20260910T210453311246Z-52c2997e3800-dirty`. At that
checkpoint, managed rollback targeted `20260910T193744320774Z-52c2997e3800-dirty`,
the previously verified H.264 receiver. Later activations changed that target:
see [the startup record](startup-switch-regression.md) and inspect managed state
through [the development workflow](development-on-pi.md) before rollback.

Native compilation succeeded. Linux checks passed for backend lifecycle,
controls, replacement, protocol/HTTP playback, receiver selection, screen
status and diagnostics. Added coverage checks the HEVC configuration guard,
decoder and plane arguments, unchanged arguments for other policies, and
warning-level atomic rejection accounting.

## AirPlay smoke results on the final release

Two generated MP4s were served over local HTTP and submitted through the real
AirPlay `/play` endpoint, then stopped through `/stop`. The receiver retained
its full debug overlay and packet diagnostics. Reports were collected after
playback to avoid adding diagnostic-export load to the measurement.

| Source | Observed decoder | Output drops | Result |
|---|---|---|---|
| 3840×2160 60 fps Main 10 HEVC | `hevc`, hardware `drm`, DRM PRIME frames | 19 during startup, none afterward | Normal audio/video progress; clean stop |
| 1920×1080 60 fps H.264 | `h264_v4l2m2m`, hardware `v4l2m2m` | 5 during startup, none afterward | Normal audio/video progress; clean stop |

Both had zero decoder drops and zero logged decoder errors. A/V difference
was approximately zero at the trace's millisecond precision. The existing
headless-service virtual-terminal notice remained counted as one output-module
error; no frame-commit rejection was reported. Both player processes exited
normally with code 0 and no signal, and the receiver returned to idle with
zero service restarts. These are short automated checks, not visual acceptance
of this receiver build or long-run qualification.

The earlier integration candidate tried `v4l2m2m` before `drm`, producing
three unnecessary HEVC decoder-probe errors. The final ordering removes them.
Its earlier measurements also overlapped detailed report collection and are
not used for the final steady-playback result. Raw sanitized evidence is kept
locally in `.pi-dev/hevc-airplay-final-report.json` (receiver PID 12888,
generations 2 and 3); media and reports are not committed.

Independent 4K60 Main 10 playback was previously visually confirmed, but that
does not qualify UHF source delivery. Test a formerly failing UHF stream,
then YouTube/H.264, stop/reconnect, and return to UHF. HEVC seeking, mirroring
transitions and longer real-stream playback still require qualification.
The connected HDMI display remains 720p60; native 4K HDMI is not tested.
