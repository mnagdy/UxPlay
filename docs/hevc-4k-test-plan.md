# Raspberry Pi 4 HEVC / 4K qualification

Prepared 10 September 2026. These tests isolate the decoder, display and media
container from UHF. They do not enable HEVC hardware decoding in the receiver.

## Measured results, 10 September

At the existing 720p60 HDMI mode, direct DRM hardware decoding through `gpu`
completed the six 720p Main/Main 10 start/stop trials. The viewer confirmed
picture and sound. Both 4K30 Main and Main 10 then completed with zero recorded
decoder or display drops. The 4K tests have not yet received visual confirmation.

| Input / path | Measured display drops after warm-up | Result |
|---|---:|---|
| 4K30 Main / gpu | 0 | Measured pass |
| 4K30 Main 10 / gpu | 0 | Measured pass |
| 4K60 Main / gpu | 73, about 7.2% | Failed smoothness target |
| 4K60 Main 10 / gpu | 662, about 62.5% | Failed; A/V difference reached 503 ms at p95 |
| DRM overlay, both smoke and 4K60 | Counter unreliable | Frame commits rejected by driver |
| 4K60 Main / overlay-primary | 0 | Measured pass; 0.5 ms p95 A/V difference |
| 4K60 Main 10 / overlay-primary | 0 | Measured pass; 0.5 ms p95 A/V difference |

The revised `overlay-primary` path passed six Main/Main 10 smoke cycles
(`20260910T214804-3248f4.json`) and both 4K60 display tests
(`20260910T214912-0b1330.json`, `20260910T214952-fd021e.json`). The 4K60 tests
recorded zero output and decoder drops, zero atomic commit failures and no
steady-playback clock stalls. Hardware decoding was observed, each process
quit normally, and the receiver was restored. Visual confirmation is pending.

The following 120-second 4K60 Main 10 loop also passed
(`20260910T215026-06f441.json`). It recorded zero decoder drops and zero output
drops during 112.915 seconds of measured steady playback, with 0.5 ms p95 A/V
difference and no clock stalls or rejected commits. Five output drops were
observed during initial startup (at 0.2 seconds of media), outside the warm-up
window; the loop reset the counter afterward. There were no active throttle
flags, the player quit normally, and the same receiver release was restored.
The viewer explicitly confirmed **smooth picture and audible tone** during
this loop. That confirmation applies to this synthetic 4K60 Main 10 test;
the original short 4K60 Main test has automated evidence only.

The independent FFmpeg direct-output path passed six short smoke cycles. Its
4K60 Main benchmark submitted 1,200 frames in 20.894 seconds but reported
`create_dst_bufs: Failed to create V4L2 buffer`; it is retained as a failed
diagnostic run. Full frame submission is not proof of successful HDMI
presentation or a clean driver run.

A subsequent 120-second 4K30 Main 10 run also passed: zero decoder/display
drops, no clock stalls, and 0.5 ms p95 A/V difference across approximately
113 seconds of steady playback after excluding startup and loop boundaries.
Its final shutdown was clean and the receiver was confirmed active again.

The DRM overlay trials initially received false passes because mpv logs failed
atomic frame commits at warning severity while leaving drop counters at zero.
Review of the complete retained messages invalidated those results, including
the overlay smoke trials. The runner now fails these messages regardless of
severity and counts them beyond the retained-message limit. Do not use those
older overlay passes to qualify the path.

The direct-DRM null-output benchmark could not initialize hardware decoding.
It supplies no valid decoder throughput or speed-limit result. All these trials
exited cleanly and restored the receiver; no current thermal/power throttling
was observed. The existing AirPlay receiver configuration remains unchanged.
Seeking, container comparisons and actual AirPlay HEVC hardware integration
remain unqualified; the current results do not establish native 4K HDMI output.

## What we can establish

The current projector connection is **1280×720 at 60 Hz**. Its advertised modes
do not include 3840×2160. We can test whether the Pi decodes a 4K source and
displays it smoothly at the current output resolution. Native 4K HDMI output
requires a suitable display and a separate run; changing output invalidates
the test gates. A 24 fps source on a 60 Hz display also has uneven frame cadence
even without dropped frames. Prefer 30/60 fps for judging fluid motion here.

The installed Pi 4 kernel is `6.18.34+rpt-rpi-v8`; mpv is 0.40.0, with Raspberry
Pi FFmpeg 7.1.5 at runtime. Its HEVC hardware options are `drm` and `drm-copy`.
The interop options include `drmprime` and `drmprime-overlay`. These are observed
capabilities, not proof of functional decoding. HEVC uses the stateless Request
decoder; the working H.264 `v4l2m2m` configuration is a different path.

The earlier HEVC experiment left a process blocked in kernel shutdown. A
different player can still reach that driver. Consequently, repeated playback
and clean shutdown come before 4K qualification. Accurate seeking is separate
and comes last among control tests. A related upstream report describes an
unkillable Pi 4 HEVC process after accurate seeking on another 6.18 kernel:
[Raspberry Pi Linux issue 7537](https://github.com/raspberrypi/linux/issues/7537).
It is relevant evidence, not proof of the exact cause on this installed kernel.

## Generated fixtures

All clips last 20 seconds, contain a moving pattern, a burned-in format label,
and a quiet 440 Hz tone encoded as stereo AAC at 48 kHz. Every MP4 is fully
software-decoded and its frame count, dimensions, profile and audio checked.
The same compressed streams are remuxed into MPEG-TS, HLS with TS segments,
and HLS with fragmented MP4 segments. The manifest hashes every file.

| Fixture ID | Dimensions | FPS | Profile | Target video bitrate |
|---|---|---:|---|---:|
| `720p25-main` | 1280×720 | 25 | Main, 8-bit | 3 Mbps |
| `720p25-main10` | 1280×720 | 25 | Main 10, 10-bit | 3 Mbps |
| `1080p30-main` | 1920×1080 | 30 | Main, 8-bit | 5 Mbps |
| `2160p24-main` | 3840×2160 | 24 | Main, 8-bit | 10 Mbps |
| `2160p30-main` | 3840×2160 | 30 | Main, 8-bit | 12 Mbps |
| `2160p60-main` | 3840×2160 | 60 | Main, 8-bit | 20 Mbps |
| `2160p30-main10` | 3840×2160 | 30 | Main 10, 10-bit | 12 Mbps |
| `2160p60-main10` | 3840×2160 | 60 | Main 10, 10-bit | 20 Mbps |

All are SDR BT.709. **Main 10 does not mean HDR.** HDR tone mapping, unusually
high bitrates, long GOPs and real-content stress tests remain later work.
Target bitrate is an encoding setting; the manifest records actual file sizes.

## Three hardware paths

| `--path` | Decoder | Display interop | Purpose |
|---|---|---|---|
| `gpu` | `drm` | `drmprime` | Direct hardware frames through GPU rendering |
| `overlay` | `drm` | `drmprime-overlay` | Compare direct DRM plane presentation |
| `copy` | `drm-copy` | automatic | Measure the cost/compatibility of copying frames |

All use the fast render profile, existing HDMI/ALSA devices and a visible test
overlay. Software fallback is disabled, and the actual `hwdec-current` value
must match. The runner never attaches to the receiver's private player socket.

## Test order

| Stage | Exercise | What the result distinguishes |
|---|---|---|
| `smoke` | Alternate 720p Main/Main 10, three start/stop cycles each, up to 9 seconds per clip | Decoder startup, audio and safe teardown |
| `benchmark` | Decode one whole clip, no audio or display, untimed, no frame dropping | Approximate decode throughput including startup; says nothing about HDMI smoothness |
| `display` | Play one clip with audio and debug overlay | End-to-end decode, scaling, display and sound |
| `seek-keyframes` | Pause, resume, seek back to 2 seconds using a keyframe | Control behaviour and recovery |
| `seek-exact` | Repeat with an accurate seek | The more demanding driver teardown/restart case |
| `http` | Serve one MP4, TS or HLS variant over loopback HTTP | Container/demux differences with the same compressed media |
| `soak` | Loop one passing clip for 120 seconds | Accumulating drops, thermal issues and repeated loop boundaries |

Start with `gpu` smoke. If it fails **but exits cleanly and restores the receiver**,
inspect the report and try `overlay` smoke, then `copy` smoke. Each path must pass
its own smoke gate. Do not run them as one unattended batch.

For a passing path, test 1080p30, then 4K24, 4K30, 4K60; repeat 4K30/60 for
Main 10. A benchmark is a useful comparison, not a prerequisite for display:
some direct-frame paths may be incompatible with a null output while supporting
a real output. Display, seek and HTTP results remain separate.

The runner requires a passing smoke result for every later stage, a passing
display result for that clip before control/HTTP/soak stages, and keyframe seek
before accurate seek. Gates are tied to this boot, kernel, mpv version, helper,
fixture manifest and display mode. A failed repeat revokes that stage's pass.

## Running on the Pi

Prepared location:

```sh
cd /home/mo/uxplay-dev/state/hevc-4k-tests
```

The two smoke-test MP4s are staged first. The complete validated matrix is in
the development checkout under `.pi-dev/hevc-4k/fixtures`; copy each larger
fixture directory to the Pi's `fixtures/` before selecting it. The runner
checks files needed for the selected stage, so the first test does not depend
on transferring every 4K container variant over the current slow connection.

Verify the files and preview the first stage without affecting playback:

```sh
python3 pi-hevc-tests --fixtures fixtures
```

The first actual test is:

```sh
python3 pi-hevc-tests --fixtures fixtures --stage smoke --path gpu --run
```

Run as `mo` in the SSH terminal. If sudo asks for a password, enter it there.
The runner temporarily stops the receiver, runs the selected stage and restores
the same service. No package, kernel, release or receiver setting is changed.
The existing video will stop when an actual test begins.

After a smoke pass, examples for one clip/path:

```sh
python3 pi-hevc-tests --fixtures fixtures --stage benchmark --path gpu --fixture 2160p30-main --run
python3 pi-hevc-tests --fixtures fixtures --stage display --path gpu --fixture 2160p30-main --run
python3 pi-hevc-tests --fixtures fixtures --stage seek-keyframes --path gpu --fixture 2160p30-main --run
python3 pi-hevc-tests --fixtures fixtures --stage seek-exact --path gpu --fixture 2160p30-main --run
python3 pi-hevc-tests --fixtures fixtures --stage http --path gpu --fixture 2160p30-main --transport hls-ts --run
python3 pi-hevc-tests --fixtures fixtures --stage soak --path gpu --fixture 2160p30-main --run
```

For HTTP comparisons, repeat with `--transport mp4`, `ts`, `hls-ts` and
`hls-fmp4`. These tests serve complete local files without artificial network
pacing. They do not exercise AirPlay or Wi-Fi. The existing
`scripts/airplay-fixture-send` provides a separate paced-TS AirPlay test.

## Evidence and acceptance

### Direct-output experiments

Two additional paths compare the current GPU import path with direct display
output. Both use the same exclusive player ownership, bounded shutdown,
receiver restoration and environment-specific smoke prerequisites.

```sh
python3 pi-hevc-tests --fixtures fixtures --stage smoke --path ffmpeg-drm --run
python3 pi-hevc-tests --fixtures fixtures --stage benchmark --path ffmpeg-drm --fixture 2160p60-main --run
python3 pi-hevc-tests --fixtures fixtures --stage benchmark --path ffmpeg-drm --fixture 2160p60-main10 --run
python3 pi-hevc-tests --fixtures fixtures --stage display --path ffmpeg-drm --fixture 2160p60-main10 --run
python3 pi-hevc-tests --fixtures fixtures --stage smoke --path overlay-primary --run
python3 pi-hevc-tests --fixtures fixtures --stage display --path overlay-primary --fixture 2160p60-main --run
python3 pi-hevc-tests --fixtures fixtures --stage display --path overlay-primary --fixture 2160p60-main10 --run
```

`ffmpeg-drm` uses the installed Raspberry Pi FFmpeg `vout_drm` output, preserves
DRM hardware frames, and disables audio. Smoke plays four seconds per clip;
display is paced and benchmark runs unpaced. Its pass means complete hardware
frame submission and clean exit with no detected diagnostic failure. It does
**not** certify smoothness, real-time throughput, audio, or independently counted
HDMI presentations. Compare submitted FPS separately with the source rate;
the measurement includes startup and display output. Full progress and bounded
verbose diagnostics are retained in each report. Control/HTTP/soak stages are
not supported for this reference output.

`overlay-primary` keeps mpv audio and metrics but places video on the primary
plane and the OSD on an overlay with a 1280×720 surface. It requires the same
drop, sync and error checks as the GPU path. Rejected atomic display commits
fail a run even when mpv reports zero dropped frames.

The connected display currently runs at 720p60: these experiments test decoding
4K sources and scaling them to that mode, not native 4K60 HDMI output.

### Report interpretation

Each run writes a timestamped JSON report under `results/` with decoder and
decoded dimensions, playback/audio clocks, A/V difference, output and decoder
drops, player events, warning/error messages, temperature and throttle flags,
shutdown method, exit code and receiver restoration status. A short debug
overlay shows the same playback essentials on HDMI.

For a display qualification the automated targets are:

- Requested hardware and expected decoded dimensions actually observed.
- Advancing audio and playback clocks, roughly real-time progress.
- At most 1% dropped frames after warm-up and 95th-percentile A/V difference
  no larger than 80 ms. Missing telemetry is not a pass.
- No reported playback errors, repeated clock stalls or current power/thermal
  throttling. Historical throttle flags are retained as context.
- A normal player exit without forced termination, and receiver service restored.

These are initial qualification thresholds, not a guarantee of perceptual
smoothness. Every visible result still needs **smooth picture and audible tone**
confirmed by the viewer. Reports leave that confirmation pending. Seek and loop
jumps are excluded from steady-playback calculations. A headless result must
reach EOF within the clip duration; its FPS estimate uses the independently
validated frame count and is not a measurement of frames presented on HDMI.

If a player needs a signal to exit, the stage fails and the suite stops. If it
cannot be reaped, the runner does not start another receiver into the held
device. A recovery timer likewise starts the receiver only when neither player
is present. A process stuck in kernel wait may require physical recovery;
signals and timeout code cannot guarantee its release. Keep the report and
inspect the system before another test. No automatic reboot is attempted.

## What follows a successful qualification

Only after a hardware path passes playback, control, shutdown and longer tests
should it be added as an isolated receiver experiment. Reuse these fixtures
through direct AirPlay, then compare paced TS, HLS, and real UHF streams while
checking that audio packets arrive. The current receiver deliberately leaves
HEVC hardware disabled; sending 4K fixtures to it today would test its software
path. Do not interpret that as this hardware qualification.

The earlier UHF failure also involved missing usable audio at the selected
queue. Successful generated HEVC tests cannot establish that UHF stream is
fixed. Retain working YouTube/H.264 playback and recheck picture, sound and
switching before promoting any HEVC receiver change.

## Rebuilding and checking the suite

On a development Linux machine with FFmpeg/libx265 and DejaVu fonts:

```sh
python3 tests/make_hevc_fixtures.py /new/empty/fixtures --jobs 1
python3 -m unittest discover -s tests -p test_pi_hevc_tests.py -v
```

Generate on container-local disk if using Docker, then copy out; do not encode
on the Pi under test. Full validation can be repeated with `--verify-existing`.
Fixture media and raw test reports stay in ignored local storage, not Git.

Tooling tests cover hardware selection, measured evidence, gate dependencies,
shutdown escalation, restoration decisions and the private IPC lifecycle with
a simulated Linux child. They do not execute the Pi HEVC driver.
