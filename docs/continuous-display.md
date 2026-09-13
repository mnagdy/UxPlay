# Continuous display ownership — PiPlay milestone 1

The Pi now starts the supervised Wayland display stack automatically. The
original direct-HDMI receiver remains available through the tested rollback
command. Physical stream transitions passed in the bounded trial; startup and
recovery of the installed service passed on 13 September. Repeated audio
reliability remains a separate qualification boundary.

## Ownership

A separately supervised Wayland compositor owns the display for the duration of
the receiver session. The receiver creates one fullscreen GStreamer status
surface at startup and keeps it mapped underneath transient media windows.
Loading and error text updates that surface without reopening it. mpv's
`force-window=no` is retained: a player waiting for its first frame should not
cover the status surface with an empty forced window.

Direct video still uses the existing supervised mpv child and private IPC.
Mirroring and audio-only AirPlay still use GStreamer. Player generation checks,
child termination deadlines and exclusive audio handover remain in place.
The compositor owning HDMI does not make it safe to run two audio sessions.

The receiver configuration switch is:

```text
display-owner wayland
vs waylandsink
screen-info status
airplay-video-backend mpv
mpv-decode pi4-wayland-experimental
mpv-vo dmabuf-wayland
mpv-h264-hwdec v4l2m2m
```

The launcher supplies a private `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` for the
already-running compositor. Audio settings remain those of the current device.
The kiosk shell makes all surfaces fullscreen. The receiver enables status if
omitted and rejects configurations that would put a direct-DRM sink under
Wayland. It sets GStreamer's `fullscreen=true` automatically so portrait and
oversized video retain the compositor's configured bounds. The private
waylandsink build includes upstream fix
[`96b800f8`](https://github.com/GStreamer/gstreamer/commit/96b800f82a3084eec8e4e970a7f5ce6743967e45),
which safely stores this property before the first frame creates the window.
Leaving the property false caused portrait mirroring to override the negotiated
1280×720 bounds with 498×1080, which Weston correctly rejected.
Remove `mpv-gpu-context`, `mpv-gpu-api`, `mpv-drm-device` and
`mpv-drm-connector` from this mode; those settings belong to the direct-HDMI
path. Software `wlshm` is permitted for headless software tests, but is not
the Pi hardware profile.

The new experimental decoding policy requests hardware H.264/HEVC without
software fallback. It does not pass DRM plane options to mpv: the compositor
must assign the decoded buffers to appropriate display planes. This is a
different output path from the qualified direct-DRM HEVC arrangement.

If the persistent status surface fails, the receiver stops for supervised
recovery rather than continuing to advertise successful display operation.

## Compositor qualification

The Pi's available Weston package is 14.0.2. Source inspection identified two
issues that a stock package installation would not solve:

* Its format table lacks P030, the format used by our recorded Pi 4 Main 10
  direct-plane test. Both DRM framebuffer and GL import paths consult this table.
* Kiosk-shell can hide the previous fullscreen window and fail to restore it
  when the newer window disappears on a system with no input seat. A projector
  without a keyboard or mouse is a normal PiPlay configuration.

The isolated compositor recipe and patches accompany this prototype. They must
not be mistaken for proof that Pi P030 modifiers, scaling or plane assignment
work. In particular, a loading/debug overlay can change whether video is scanned
out directly or composited. Test normal status and debug overlays separately.

Relevant upstream sources:

* [Weston 14 format table](https://sources.debian.org/data/main/w/weston/14.0.2-1/libweston/pixel-formats.c)
* [Weston 14 DRM framebuffer import](https://sources.debian.org/data/main/w/weston/14.0.2-1/libweston/backend-drm/fb.c)
* [Weston 16 format metadata](https://sources.debian.org/data/main/w/weston/16.0.0-1/libweston/pixel-formats.c)
* [mpv 0.40 output reference](https://github.com/mpv-player/mpv/blob/v0.40.0/DOCS/man/vo.rst)

## Acceptance gates

1. Headless compositor captures show the background before media, the actual
   media frame during playback, and that same background after stop, replacement,
   EOF and failure. Test without an input device. A live process is insufficient.
2. Receiver tests retain the status surface throughout normal and forced child
   termination, replacement, error recovery and mirroring handover. The legacy
   renderer-owned path continues to release it as before.
3. On the Pi, maintain one compositor PID and the same HDMI mode throughout
   ready → loading → video → ready. Observe the projector during repeated cycles;
   no terminal should be exposed. This does not cover firmware/early boot splash.
4. Compare the retained H.264 and HEVC Main/Main 10 fixtures with the existing
   baseline: actual decoder, picture, sound, frame drops, failed frame commits,
   CPU use, pause/seek, and clean stop. Test overlays both off and on.
5. Test real iPhone direct playback, mirroring, audio-only playback and handover.
   Source decoding at 4K while HDMI is 720p is not native 4K presentation.
6. Terminate the receiver and compositor separately during the trial; verify
   bounded cleanup and restoration of the recorded receiver service.

Do not switch the normal startup service permanently until these gates pass.
The bounded trial launcher restores the existing service and does not replace
its saved configuration or install a service override.

## Durable startup integration — 13 September 2026

The [startup-service installer](display-service.md) is installed from the
accepted R5 receiver plan. Its first V2 activation exposed `/run` being mounted
`noexec`: the copied Wayland plugin could not map executable code. Automatic
rollback restored the original receiver. V3 stages plugin/broker code under
`/var/cache/piplay-display`, retaining only health/Wi-Fi state under `/run`.
A targeted root test reproduced both blocked plugin and broker under `/run`
and passed the same checks on the new mount. Startup now rejects a noexec cache.

The native suites ran 65 tests: 63 passed and two root-only cases skipped;
the separate privileged Host/service suite passed 30/30, covering those root
cases. The generated systemd unit and exact service-host preflight passed.
V3 activation, receiver-only restart with an unchanged compositor, compositor
recovery, supervisor SIGKILL cleanup, installed rollback and reactivation all
passed. The user confirmed ready initially, after recovery and after reboot.
Boot `f4d72685-6a86-491f-a58b-57d775fe18f3` automatically started the installed
stack with NRestarts=0. Postboot mirroring/rotation and no-flash disconnect also
passed. The subsequent YouTube playlist request returned E3 before mpv opened;
its precise cause is still under investigation. A different-video retry passed
picture, sound and clean return to ready. Spotify audio remains choppy or silent
at the projector while phone progress continues. Power saving is off; the
current postboot association is 5 GHz, approximately -74 dBm. All 18,695
delivered ALAC frames were accepted by appsrc, but sampled delivery gaps and
heavy retransmission precede the decoder. Network delivery versus reorder
buffering is not yet isolated. Ethernet comparison is impractical; a bounded
numeric packet-header capture subsequently confirmed a 9.401-second arrival
gap on 5 GHz, with zero capture drops. A stronger 2.4 GHz comparison also failed
the listening test. The original Wi-Fi profile was restored and the comparison
profile removed. At the user's request, playback investigation is now deferred;
see [the open issue notes](playback-findings.md#deferred-during-piplay-display-integration--13-september).
Evidence is in
`.pi-dev/display-service-20260913/`.

## Current verification — 12 September 2026

The optional receiver builds on the Pi. Seven relevant Linux test groups passed
with mpv enabled, including real local HTTP playback; four display/audio groups
passed with mpv disabled. The receiver handover suite exercises both modes.
The launcher has 36 tests covering independent process-group cleanup,
preservation of device/audio configuration, live log access and a pinned private
mpv override. All cases passed across unprivileged and privileged Linux runs;
the root-only permissions case is checked separately from unprivileged runs.

Real headless compositor captures passed 25/25 with the kiosk patch. The same
test failed 13/25 captures with stock Weston, showing black while its background
pipeline remained alive. The test explicitly confirms no input seat. See the
[local capture evidence](../.pi-dev/wayland-display-qa-20260911/README.md) for
versions, patches, images, and the limits of sampled screenshots.

The isolated compositor starts on the Pi using headless Pixman, headless GL and
the real DRM backend. The live trial held one compositor PID at 1280×720/60 Hz
through two failed AirPlay playback requests. The player failed with SIGSEGV
during video-output initialization, before an initialized decoder or video
frame was reported; the receiver and compositor survived. A standalone
`dmabuf-wayland` player reproduced the crash in its video-output thread, so this
is not yet a working Pi playback path. The compositor advertised P030 with the
Pi modifier, but successful P030 presentation remains unverified.

A diagnostic `gpu/wayland/opengl` alternative played 720p HEVC with hardware
decoding. It performed poorly with the 4K60 Main 10 fixture: 570 output drops,
zero reported decoder drops, and only about 11 seconds of media progress within
a 30-second bound. Do not promote that alternative as the solution for 4K.

Sending SIGTERM to the trial receiver triggered cleanup and restored the
original service. The same 720p fixture then reported ready playback, progressed
to 18.88 seconds in 20.11 seconds, and accepted stop. Physical picture, sound,
and the absence of visible flashes still need observer confirmation. See the
[live trial evidence](../.pi-dev/continuous-display-live-20260911/README.md).
No permanent service activation has been performed.

The stock player's fullscreen crash was reproduced under native headless GL
with debug symbols. A driver-loading backport alone still crashed. Upstream
[resize fix 43ce03c5](https://github.com/mpv-player/mpv/commit/43ce03c519f6ca7e61c73616d4aa10ecb4a380d2)
defers an early resize when the video rectangle has zero dimensions. With that
fix, the identical `--fullscreen --force-window=no` command played five seconds
of HEVC Main through `dmabuf-wayland`, reported `drm` hardware decoding and
`drm_prime[rpi4_8]`, and exited normally. This repairs the reproduced startup
failure; the physical HDMI acceptance came from the later R2 trial below.

The final isolated player build passes all 11 unit tests. Its image-format
reference needed the five formats supplied by the pinned Raspberry Pi FFmpeg
package; that exact reference adjustment is recorded separately from player
code. The installed binary then passed the same five-second headless Main
test. A short headless Main 10 run also initialized P030 import but dropped
frames under GL/GDB, so it establishes import compatibility only. Build and
before/after evidence is in `.pi-dev/mpv-wayland-native/`.

The 12 September trial exposed a missing integration check: the first private
player omitted Lua, which also removes the `osc`, `load-scripts` and `ytdl`
options that the receiver passes to disable scripts. It exited during receiver
startup. The supervisor restored the original receiver; the user's ready-screen
observation belonged to that restored service. The earlier standalone smoke
and capability checks did not cover the receiver's complete startup arguments.

The R2 build includes Lua 5.2 and validates those exact disable options. The
receiver now provides `-check-startup`, using the same effective configuration,
player arguments and private IPC handshake as normal startup, then exiting
before GStreamer, debug control, discovery or any output is initialized. The
trial helper requires and runs this check before stopping the existing service.
Missing configuration, unavailable IPC, unsupported builds and help/version
shortcuts fail closed in check mode. The receiver integration tests cover these
exits and child cleanup; both build variants compile.

The R2 physical trial passed the three direct-video cases: HEVC 720p25,
H.264 1080p60 and HEVC 4K60 Main 10. The user confirmed smooth picture and
sound, no terminal flashes on the first two start/stop cycles, and a clean
return to ready after Main 10. HDMI remained 720p60. KMS showed the 4K P030
buffer scaled on a video plane above the persistent background. Complete
Main 10 drop counts were unavailable after natural EOF; the terminal snapshot
had already cleared them.

Portrait iPhone mirroring failed because waylandsink replaced the compositor's
fullscreen bounds with the taller phone image. The compositor and background
survived, but status text alone is not successful mirroring. The private
waylandsink fix above addresses that sizing path. A repeat phone test,
audio-only/direct-video handover and compositor-termination recovery remain
open. The normal service was restored after the failed mirroring trial.
Detailed results are in
[12 September evidence](../.pi-dev/continuous-display-live-20260912/README.md).

The corrected mirroring path passed seven native headless pixel captures:
498×1080 portrait with centered 332×720 content and black padding, rotation
to 1920×1080 and back within the same running pipeline, a separate oversized
landscape start, and restoration of the persistent background after both
closes. Turning fullscreen off reproduced the protocol error and failed the
media captures. This checks geometry and surface restoration; the repeated
iPhone/HDMI test is still required. Evidence is retained in
`.pi-dev/wayland-mirroring-geometry-qa-20260912/`.

R3's subsequent physical test displayed the first phone frame but then froze.
The geometry rejection was gone. Its log entered a pre-existing timestamp
retry path that translated the sender clock twice after a 22.916 ms startup
adjustment. This can schedule the first frame far into the future. GStreamer's
[video sink displays preroll](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gst-plugins-base/gst-libs/gst/video/gstvideosink.c#L287)
before the [base sink waits for its presentation time](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gstreamer/libs/gst/base/gstbasesink.c#L2738).
A native bounded test confirmed one preroll frame with subsequent buffers
queued behind a future timestamp, versus all 20 buffers rendered with timely
timestamps. The retry now recomputes each attempt from the original sender
timestamp. This correction still requires a repeated physical mirroring test.

The callback regression exercises the observed 22.916 ms startup correction,
the next frame, and the ordinary no-correction path. Restoring the old retry
expression makes the test fail with a first frame roughly 57 years ahead.
Native receiver and screen-video tests passed. During the first 30 seconds
of an active mirroring session, at most 16 fixed numeric progress records now
show decoder input, decoded video, sink buffers, observation ages and pipeline
state. Unknown counts remain distinct from zero; sink arrival is still not
proof of physical presentation.

The corrected plan `prepared-r4-mirror-timing` passed preflight with baseline
receiver 21524 unchanged. Automatic start was refused because sudo required a
password. Start the prepared trial with:

```sh
ssh -t mo@projector.local 'sudo python3 /home/mo/uxplay-dev/display-trial/pi-display-trial run /home/mo/uxplay-dev/display-trial/prepared-r4-mirror-timing'
```

The ten-minute bound and automatic restoration remain in place. No permanent
service activation has been performed.

R4 was subsequently started after a Pi reboot. Although Projector initially
did not appear on the phone, Mac-side mDNS browsing and resolution confirmed
both live AirPlay advertisements and the current listener. The user then
confirmed discovery and working screen mirroring. In a 28.112-second progress
sample, decoder input reached 659 buffers, decoded output 603 and sink input
600; the pipeline remained PLAYING without an error. Its reported QoS drop
count was 55, so this is continuing, user-accepted mirroring rather than a
zero-drop claim. No timestamp-correction message occurred in this session;
the corrected retry branch is covered by the regression. The user also
confirmed responsive portrait/landscape rotation and a ready-screen return
without terminal flashes. YouTube direct video then passed picture, sound and
clean return to ready; its H.264 hardware path reported zero decoder/output
drops and a normal player exit. Terminating the compositor also exercised
supervised cleanup: trial processes exited and the original receiver resumed
with HDMI ownership. The physical transition during that injected fault was
not separately qualified. Evidence is in the 12 September `r4/` directory.

Audio-only playback remains unresolved. R4 stopped an active ALAC session
because its sender did not issue `/feedback` within the watchdog window,
even though audio packets continued arriving. R5 counts delivered, valid ALAC
audio as liveness and uses an atomic watchdog counter; missing audio still
expires normally. Native callback and audio-renderer tests passed (2/2),
including forty simulated seconds of valid audio and timeout after it stops.
The trial also enables the existing `async` timed-audio option. The user
confirmed the connection stayed up but reported startup choppiness and
intermittent stops before playback settled. This is not an audio acceptance.
With Wi-Fi power saving temporarily off, the next fresh ALAC session passed:
the user reported steady sound from startup and a clean ready-screen return
without terminal flashes. All 3,995 delivered buffers were accepted by appsrc,
and teardown came from the sender. Retransmissions persisted, so a single
successful run does not establish Wi-Fi power saving as the cause. Repeated
starts and long-run audio reliability remain unqualified.

The R5 trial then ended: the normal receiver service resumed, no trial
compositor/player remained, and Wi-Fi power saving was restored to on with
exit 0. Every planned display-transition path now has a passing physical run;
durable startup/service integration is not yet installed.

The maintained entrypoints are:

* `scripts/pi-build-weston-trial`: source-pinned, user-local Weston and private
  seatd build. It extracts dependency packages without installing them.
* `scripts/pi-build-mpv-wayland-trial`: isolated mpv 0.40 build with the matching
  Debian patch set and bounded Wayland corrections. Its output does not replace
  the installed player.
* `scripts/pi-build-waylandsink-trial`: builds only the private waylandsink
  module from the matching Raspberry Pi 1.26.2 source package, retaining its
  vendor patches and adding the upstream fullscreen setter guard. It links
  the installed GStreamer libraries and verifies the selected plugin path.
* `scripts/pi-display-trial prepare`: derives a private trial configuration from
  a built release and the existing device configuration. Optional
  `--mpv-executable` pins a private player binary by hash without changing the
  original receiver's player selection. `--libinput-quirks-dir` points the
  isolated compositor at its extracted device data; the first trial exposed
  missing data at libinput's normal system path.
  `--wayland-plugin` pins the private waylandsink. Check/run snapshot that
  module, exclude its system duplicate, and use a fresh private plugin registry.
  Preflight verifies the selected file/version, installed support-library
  linkage, and safe fullscreen construction before any display is opened.
* `scripts/pi-display-trial check`: verifies pinned files, runtime libraries and
  the actual receiver/player startup handshake without stopping the receiver.
* `scripts/pi-display-trial run`: requires Pi sudo, runs for at most ten minutes,
  then terminates its own processes and restarts the previous receiver service.
  It also cleans up after normal interruption or startup failure. If a process
  cannot be reaped, it refuses to start a competing receiver and reports recovery
  as necessary. SIGKILL or power loss cannot execute Python cleanup; the normal
  unchanged boot service remains the reboot recovery route.

During a trial, known log files are readable only by the receiver account and
root. Root retains control of their directory entries until cleanup completes,
so live troubleshooting does not require ending the trial or exposing logs to
other users.

Weston 14 requires `--backend=drm`, rather than an absolute module path. The
compiled private prefix supplies its backend; the launcher still verifies the
actual module hash. Absolute paths are supported for `--shell`.

## What this milestone does not claim

This establishes the display-ownership foundation. It does not implement the
branded animation, boot splash, OS image, Imager provisioning or whole-system
updates. Existing sender/export and protected-stream limitations remain separate
from display transitions; see [playback findings](playback-findings.md).
