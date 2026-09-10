# Raspberry Pi 4 playback optimisation proposal

Investigated 9 September 2026 against the `pi-uhf` fork at `cd33afca3ada203ce698e4b6a3aa49496dcfa499`.

This is a design proposal. No performance changes or device tests were made during this investigation. The user previously reported working UHF live and series playback, approximately 20-second startup, and poor frame rate for some 4K streams. Discovery recovery was unconfirmed at the time of this initial proposal; see the later device findings below. The current kernel, GStreamer packages, stream codec/profile and projector's native resolution are still unknown.

## Later device findings on 9 September 2026

After reboot, AirPlay discovery and receiver connectivity were verified, and the user confirmed working YouTube playback and smooth switching between videos with `hls-pi4` enabled. The exact working release and configuration are recorded in [FORK.md](../FORK.md#confirmed-working-baseline--9-september-2026). This does not establish improved 4K playback or measured startup latency.

The original investigation above predates implementation and device access. The Pi is now confirmed to run kernel `6.18.34+rpt-rpi-v8` and GStreamer `1.26.2`. YouTube diagnostics observed 720p H.264 decoding through `v4l2h264dec`, with DMA-BUF frames reaching `kmssink`. This confirms that particular decoder/output path, not 4K performance or physical presentation.

The sampled 720p25 HEVC stream was also verified through the running receiver on 9 September. After removing an audio-track declaration with no accompanying audio samples in the private test fixture, `v4l2slh265dec` delivered decoded frames to `kmssink` and entered PLAYING in 3.925 seconds. A 15-second observation produced no diagnostic errors or warnings. A headless `fakevideosink` comparison rendered 121 hardware-decoded frames in five seconds with no drops. Generic `fakesink` lacks the same hardware-buffer negotiation and had produced a misleading decoder failure; this short run did not establish safe decoder teardown. This verifies the sampled video path, not the missing soundtrack, physical projected image quality or other HEVC profiles.

A later real UHF stop at 13:22 BST wedged the receiver in the Pi kernel's `hevc_d_h265_stop` IRQ wait, blocking all subsequent playback including YouTube. The `hls-pi4` profile therefore excludes only `v4l2slh265dec` from automatic selection before pipelines start; HEVC uses software where available, while H.264 hardware decoding remains available. This supersedes the earlier narrow hardware-playback conclusion. The existing lockup requires a reboot; restarting the application cannot interrupt that kernel wait. Software performance at higher HEVC resolutions is not yet validated. [Related Pi4 driver report](https://github.com/raspberrypi/linux/issues/7537).

The Pi4 profile now supplies `driver-name=vc4` when using `kmssink`, unless the sink options explicitly select `driver-name`, `bus-id` or `fd`. Isolated sink initialization took 3.619 and 4.047 seconds with automatic driver discovery, compared with 0.155 seconds in both runs with `vc4` selected. GStreamer's default discovery tries thirteen other DRM drivers first. The change applies whenever this profile creates the configured video sink, preserves codec selection, and avoids that repeated discovery cost. These are initialization measurements; sender and network delays still affect total startup time. [GStreamer 1.26.2 driver discovery](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gst-plugins-bad/sys/kms/gstkmssink.c#L475).

The YouTube first-frame stall persisted after incomplete/unavailable playlist handling was corrected. A cached master combining H.264 and VP9 reproduced a fatal hlsdemux2 caps error even with headless sinks. Filtering the same stream to H.264/AAC-LC allowed its clock to advance on the Pi. The new opt-in `hls-pi4` profile applies that selection to cached YouTube playlists, with a 1080p60 ceiling. Generated mixed-codec, initial-playback and resume regressions pass, along with native Pi tests. See [the fork notes](../FORK.md) for deployment and device-retest status. UHF 4K performance and startup measurements remain outstanding.

## Hardware constraints

| Input | Pi 4 hardware capability | Proposed policy |
| --- | --- | --- |
| H.264 up to 1080p60 | Dedicated decoder within the published ceiling | First baseline to validate |
| HEVC/H.265 up to 4K60 | Dedicated decoder; the full software/display path must support it | Enable higher resolutions after device verification |
| H.264 above 1080p | Outside the published hardware ceiling | Prefer a lower-resolution rendition at the source |
| Other codecs/profiles | Do not assume acceleration | Identify the stream and verify support before selection |

These are codec ceilings, not guarantees for every bitrate, profile, bit depth, HDR format or player. See the [official Pi 4 datasheet](https://datasheets.raspberrypi.com/rpi4/raspberry-pi-4-datasheet.pdf). More RAM or CPU tuning does not expand the dedicated H.264 decoder's resolution limit.

The Pi's Hardware Video Scaler can scale and convert decoded pictures for display. It cannot avoid decoding the input first. A 1080p projector fed a 4K stream still requires a 4K decode; choosing an available 1080p compressed rendition removes that work earlier. See the [Linux display-driver documentation](https://docs.kernel.org/gpu/vc4.html).

## What the fork currently does

UHF direct video uses a different path from AirPlay mirroring:

```text
UHF supplies a media URL
  -> UxPlay direct-video handler
  -> GStreamer playbin3
  -> automatically selected demuxer and decoder
  -> configured video sink (currently kmssink)
```

In [video_renderer.c](../renderers/video_renderer.c), the HLS branch around lines 311–344 creates playbin, sets the video sink, and enables DOWNLOAD and BUFFERING. It does not apply the decoder/converter arguments used by the mirroring branch. Consequently, mirror options such as `-v4l2`, `-s` and `-fps` do not select the direct-video decoder or make a 4K UHF source become 1080p.

Around lines 510–518, direct playback starts PAUSED with a state wait of at most one second. Around lines 888–904, buffering messages below 100% pause playback and 100% resumes it. There is no fixed 20-second startup timer here. Network/source delay, HLS segment availability, buffering, decoder setup and first presentation must be timed separately.

## Preferred efficient path

```text
Compatible compressed stream
  -> Pi hardware decoder
  -> shared decoded-frame buffers (DMA-BUF)
  -> display hardware, including scaling when needed
  -> HDMI
```

The objective is to avoid CPU decoding, unnecessary CPU colour conversion, and copies of decoded frames. Buffer layout and DRM modifiers must remain compatible between decoder and display; simply finding a hardware-decoder plugin or selecting kmssink does not prove this path is in use. [GStreamer explains DMA-BUF negotiation and its constraints](https://gstreamer.freedesktop.org/documentation/additional/design/dmabuf.html).

The upstream UxPlay README's broad Pi HEVC limitation needs qualification. A Raspberry Pi engineer explains that kernel 6.18 introduced the formats/control semantics needed by GStreamer's decoder, and that Trixie packages were moving to that kernel in May 2026. This is evidence to use matching Pi OS kernel and multimedia packages, not to replace individual components blindly. [Maintainer explanation](https://github.com/raspberrypi/linux/issues/7363#issuecomment-4441508294).

An August 2026 Pi 4 report describes successful plain `v4l2slh265dec` to `kmssink` playback with kernel 6.18.39 and GStreamer 1.26.2, but a decoder hang during a particular accurate segment seek. Its sample is 1080p; it does not establish working 4K, 10-bit, HDR or zero-copy operation for our setup. Test seeking, stopping and reconnecting as well as steady playback. [Device report](https://github.com/raspberrypi/linux/issues/7537).

## Proposed changes, in order

1. **Add direct-playback diagnostics.** Record monotonic timestamps for receiving `/play`, creating the player, obtaining the manifest and first media data, completing initial buffering, and presenting the first frame. Distinguish a buffer reaching the sink from actual presentation. Report the selected decoder, input codec/profile/size/frame rate, decoded format and buffer memory type. Redact credentials and tokens in URLs.

2. **Verify and select the hardware path.** Add an optional direct-playback decoder policy, using the stream's actual capabilities. Verify successful negotiation through the actual display sink. Keep fallback explicit and visible in diagnostics. A plugin's existence or rank alone is not proof of acceleration.

3. **Choose compatible renditions before decoding.** When an HLS master playlist offers alternatives, filter by codec, resolution and frame rate as well as bandwidth. If the projector is 1080p, favour a suitable 1080p rendition. Allow 4K HEVC when the full path has passed testing. A single-rendition playlist cannot supply an absent lower-resolution version; selection may then have to happen in UHF/provider settings. AdaptiveDemux2 offers bitrate controls, but bitrate alone cannot enforce the Pi's codec ceiling. [GStreamer properties](https://gstreamer.freedesktop.org/documentation/adaptivedemux2/GstAdaptiveDemux2.html).

4. **Tune startup buffering using measurements.** Separate live and on-demand playback policies. Test a smaller initial buffer while retaining enough buffering for continuous playback and network variation. Preserve the user's requested pause/play state: currently reaching 100% can resume a paused stream. Do not promise a particular startup time until source delay and segment duration are known. Review DOWNLOAD behaviour in the actual pipeline before removing it.

5. **Reduce repeated setup only if it matters.** Consider reusing the player object, asynchronous state changes, and cancellation of obsolete channel selections. Reset URI, queues, timestamps and session state correctly. Retain full teardown as a fallback. Keeping stale pipeline state is not a safe shortcut.

6. **Consider another playback backend if the display path remains costly.** Preserve UxPlay's discovery and AirPlay controls while testing Raspberry Pi's packaged libVLC as an optional direct-video engine. The Pi distribution maintains DRM PRIME and direct DRM output patches. This could reuse existing Pi integration without adding a media-centre interface, but requires proof of hardware decoding, headless output, audio sync and control/session handling. [Pi VLC patches](https://github.com/RPi-Distro/vlc/tree/pios/trixie/debian/patches). A backend replacement is a larger change than diagnostics and decoder selection.

## Measurements needed on the Pi

These commands are read-only; run them on the Pi:

```bash
uname -r
gst-launch-1.0 --version
dpkg-query -W gstreamer1.0-plugins-bad libgstreamer1.0-0
gst-inspect-1.0 v4l2slh265dec
gst-inspect-1.0 v4l2h264dec
vcgencmd get_throttled
vcgencmd measure_temp
```

A missing plugin is useful diagnostic information. Its presence is not a playback test. Also record the projector model/native resolution and inspect the actual stream codec/profile. Avoid pasting playlist credentials or signed URLs into a public issue.

First restore reliable receiver discovery using the [setup guide](raspberry-pi-uhf.md). Then compare the same sources before and after each change: 1080p H.264, 4K HEVC, and a higher-resolution H.264 case that should trigger rendition selection where available. Record startup timings over several cold starts and channel changes, CPU usage, displayed/dropped frames, buffering stalls, memory use, power/thermal flags and audio sync. Repeat a representative sample over Ethernet to isolate the Pi's Wi-Fi leg.

Test pause, seek, stop, rapid channel changes, phone lock, reconnect and switching phones. Do not run a second foreground player against the HDMI display while the service owns it. Stable 4K playback and a faster startup remain acceptance criteria, not achieved results.

Do not begin with on-device transcoding: it adds another decode/scale/encode pipeline. Remuxing preserves the codec and cannot solve unsupported 4K H.264 decoding. If no compatible rendition exists, generating one on a more capable upstream machine is a separate architectural option.
