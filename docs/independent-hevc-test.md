# HEVC playback without UHF

On 10 September 2026, both independent trials played visible moving video and
audible sound on the Pi. The user confirmed picture and tone for each trial.

The test sender is `scripts/airplay-fixture-send`. It serves a trusted local file
from the Mac over the LAN, then submits a binary-plist AirPlay `/play` request to
the existing receiver. The receiver uses its normal direct HTTP video route,
mpv, HDMI and ALSA. This exercises direct AirPlay URL playback; it is not an
iPhone sender compatibility test, screen mirroring or the YouTube FCUP cache.
It replaces the current video and stops its test session when the trial ends.

## Observed results

The generated 45-second fixture contains moving `testsrc2` video and a quiet
440 Hz sine tone: HEVC Main, 1280×720, 25 fps, 8-bit YUV420; AAC-LC, 48 kHz,
stereo. Full software decoding of the MP4 on the Pi exited successfully. Both
containers independently probed as 1,125 video packets and 2,111 audio packets.
The MPEG-TS version was remuxed without re-encoding.

| Trial | User observation | Playback after 40 seconds | Startup video/audio packets |
| --- | --- | --- | --- |
| MP4 over HTTP | Picture and tone both work | 38.08 seconds | 1,125 / 2,111 |
| Paced MPEG-TS over HTTP | Picture and tone both work | 38.30 seconds | 798 / 1,488 |

Both startup packet captures completed successfully. Both trials reported HEVC
software decoding, AAC stereo through ALSA, zero audio/video decoding errors,
zero decoder drops and one output frame drop. Both also recorded one startup
video-output error; visible playback still worked. TS had not reached EOF at
the last playing snapshot. Play and stop requests returned HTTP 200. The same
receiver PID 7006 and release `20260910T185438654187Z-52c2997e3800-dirty` remained
active with no restarts or configuration changes. Sessions were 6 and 7.

TS pacing uses average bitrate with an initial two-second probe allowance. It
tests a continuous HTTP response with no advertised content length; it does not
reproduce a broadcaster's exact timing, program changes or corruption.

This establishes successful 720p HEVC plus audio through the deployed playback
path without UHF. It does not qualify 4K, Main10, HDR, hardware HEVC decoding,
long-duration stability or the failing broadcaster's media. It does not prove
UHF is defective: source content and demux/track differences remain possible.
The next useful comparison is the same verified fixture through UHF, or the
failing source delivered independently with any necessary source authorization.

## Repeat locally

The generated files and reports are in `.pi-dev/independent-hevc/`. Discover the
current AirPlay port first; the advertised port can change after a restart:

```sh
dns-sd -L Projector _airplay._tcp local.
```

Stop discovery with Ctrl-C after the result. From the UxPlay directory, substitute
the current Pi address and advertised port:

```sh
python3 scripts/airplay-fixture-send --host 192.168.1.124 --port 41023 \
  --file .pi-dev/independent-hevc/hevc-720p.mp4 --seconds 40 \
  --output .pi-dev/independent-hevc/mp4-repeat.json

python3 scripts/airplay-fixture-send --host 192.168.1.124 --port 41023 \
  --file .pi-dev/independent-hevc/hevc-720p.ts --kind ts --duration 45 --seconds 40 \
  --output .pi-dev/independent-hevc/ts-repeat.json
```

The Mac and Pi must be mutually reachable on the LAN. Only the supplied file is
served, under a random path; the temporary HTTP server closes when the trial
ends. No service installation or sudo is needed. Use this helper with a receiver
that supports binary-plist direct video and session-scoped controls, as this
fork does. It is not a general-purpose authenticated AirPlay client.

Evidence: `fixture-metadata.json`, `mp4-result.json`, `ts-result.json`,
`combined-report.json`, and `receiver-summary.json` in the fixture directory.
