# Local playback evidence

Consolidated 10 September 2026 from the eleven experiment/audit notes formerly
in `.pi-dev/`. This index is versioned; its raw reports and media remain local
and ignored by Git. These are historical observations, not a fresh Pi check.
Use [playback findings](playback-findings.md), [startup and switching](startup-switch-regression.md)
and [FORK.md](../FORK.md) for the maintained explanation and last recorded release.
Old JSON files deliberately retain the configuration and acceptance limits of
their own run; an old `HEVC hardware disabled` or `pending` field is not current policy.

## Latest direct-export evidence — 11 September

[Sky News media inspection](diagnostics/2026-09-11-sky-news-export.md) subsequently
confirmed that three complete phone-exported fragments contained 750 HEVC video
samples and zero audio samples over 30 seconds, despite working sound in UHF.
That omission exists before Pi parsing/decoding. A later [UHF debug log analysis](diagnostics/2026-09-11-uhf-debug-analysis.md)
found AAC selected, input-audio timestamps and repeated exporter write errors;
the exact internal cause remains unproved. The observations belong to separate
attempts. Local summaries are `.pi-dev/sky-news-raw-export-20260911.json` and
`.pi-dev/sky-news-playback-report-20260911.json`; the redacted log is
`.pi-dev/2026-09-11-uhf-debug-redacted.txt`. Raw captures remain outside Git.

## Earlier unresolved UHF audio/video failure

The strongest 10 September selected-queue capture is
playback-report-hevc-packet-trial.json (`.pi-dev/playback-report-hevc-packet-trial.json`)
and its follow-up (`.pi-dev/playback-report-hevc-packet-trial-followup.json`): receiver
release `20260910T185438654187Z-52c2997e3800-dirty`, PID 7006, generation 2,
mpv child 7089, boot `04aa65be68a44e78abcf1d19ee98299f`.

- A complete 30.068-second capture observed **1,250 selected video-queue packet
  insertions and zero audio insertions**. Video bytes totalled 9,238,633; all
  video PTS were valid, spanning 9.16–59.12 seconds. These are demux observations,
  not network packets, decoder output or displayed frames.
- HEVC 1280×720 used software decoding. AAC track 1 was selected but never
  established audio rate, channels, output or audio-cache timestamps. Position
  stayed zero while video cache grew to 239.76 seconds. Pause was false,
  core-idle true, underrun true and EOF false. A short CPU sample was mostly idle.
- The FIFO completion marker was observed; rejected packet grammar, log/event
  overflow and capture errors were zero. A read-budget yield retained its data.
  Positive video records establish that zero audio was a working measurement.
- This locates the gap before selected AAC decoder input. It does **not** tell
  whether the source lacks audio, the wrong track is selected, or FFmpeg fails
  to extract it. Nor does it establish that all buffered video would render.
  Twenty-one recurring unclassified demux warnings remain unexplained.

The earlier IMG2689 report (`.pi-dev/playback-report-IMG2689.json`) and
HEVC failure baseline (`.pi-dev/mpv-hevc720-failure-baseline.json`) preserve the grey first
picture/no-sound observation before packet instrumentation. The initial
missing-reference error could explain corruption, but it did not recur during
the long stall. A startup video-output error also occurred in working sessions;
the virtual-terminal notice alone does not prove failed display presentation.
The comparator H.264 session's HLS reload failure occurred after its explicit
pause and must not be attributed to the following HEVC attempt.

recovery-final-verification.json (`.pi-dev/recovery-final-verification.json`) separately
records the earlier GStreamer UHF case: the user confirmed video without audio;
no compressed, decoded or sink-stage audio was observed, and YouTube worked
afterward. Audio-only AirPlay uses RAOP/GStreamer; its success cannot establish
AAC delivery inside a direct mpv video stream.

The next discriminating capture is audio declarations/track selection and
packet availability before the selected-queue boundary, plus a bounded safe
classification of the repeated demux warning. Successful generated HEVC
playback and later hardware qualification do not resolve this UHF source gap.

## Controlled audio-starvation experiment and diagnostic method

audio-starvation-results.json (`.pi-dev/audio-starvation-results.json`) records five
generated-only cases using mpv 0.40.0, FFmpeg 7.1.5, software decode and null
outputs. The fixture is 30 seconds of 720p25 HEVC Main with AAC-LC 48 kHz stereo.
Removing only PID 257 AAC TS packets leaves the advertised audio track, video,
PAT/PMT and video PCR intact: 750 video packets remain; intact audio has 1,408
demux packets. The live source adds two-second segments without ENDLIST.

| Controlled case | Observed outcome |
| --- | --- |
| Intact live or finite VOD | Audio initializes; timeline advances |
| Starved live, audio selected | Position zero, core idle, no audio format, growing video cache |
| Same starved live with audio deselected | Timeline advances |
| Starved finite VOD with ENDLIST | Timeline advances despite missing initialized audio |

This reproduces the telemetry pattern, not the user's corrupt first picture or
the upstream cause. It does not justify disabling audio as a general fix.
audio-starvation-trace-results.json (`.pi-dev/audio-starvation-trace-results.json`)
positively observed 397 video / 746 audio inserts for intact live playback and
397 video / zero audio for both starved controls. Keep the
unmatched negative control (`.pi-dev/audio-starvation-trace-unmatched.json`): the initial
`demux=trace` / `demux/lavf` observer was wrong, so its zeros mean unavailable.

The working grammar uses `lavf=trace` and `request_log_messages("terminal-default")`.
Only anchored numeric audio/video append messages are counted; raw text is
discarded. Zero PTS is valid; mpv's missing timestamp sentinel is not a valid
PTS. The 30-second recorder stops through a `print-text` FIFO marker before
freezing totals; command failures, overflow, rejected grammar or a packet after
the marker invalidate completeness. This avoids mistaking log-buffer loss for
missing audio. A successful subscription with all-zero packets is unverified.

Stock mpv's startup gate waits for both selected audio and video to become
ready. Aggregate cache duration can omit an unknown-duration stream, so a large
video cache can coexist with underrun. See mpv 0.40
[playback loop](https://github.com/mpv-player/mpv/blob/v0.40.0/player/playloop.c),
[audio startup](https://github.com/mpv-player/mpv/blob/v0.40.0/player/audio.c),
[demux queues](https://github.com/mpv-player/mpv/blob/v0.40.0/demux/demux.c), and
[log buffer](https://github.com/mpv-player/mpv/blob/v0.40.0/common/msg.c).

Retained validation: mpv-logging-validation.json (`.pi-dev/mpv-logging-validation.json`),
mpv-packet-trial-validation.json (`.pi-dev/mpv-packet-trial-validation.json`), and
logging-roundtrip-validation.json (`.pi-dev/logging-roundtrip-validation.json`).
Generated HTTP MP4, TS HLS, fMP4 HLS, separate audio and HEVC cases validated
positive observations and bounded shutdown; adapter checks covered terminal
draining, replacement, warning classification, overflow and privacy. Null
outputs do not verify HDMI or sound. Packet counts still do not measure
decoder consumption, decoded frames/samples or physical output.

## H.264 and HEVC hardware evidence

| Evidence | Preserved finding and qualification limit |
| --- | --- |
| H.264 headless result (`.pi-dev/h264-hardware-headless-result.json`), inventory (`.pi-dev/h264-hardware-inventory.json`) | Five hardware-copy 1080p23.976 cycles passed progress, exact seek, stop and exit; no software fallback. Null output did not qualify 1080p60 display performance. |
| YouTube choppy report (`.pi-dev/youtube-h264-hardware-choppy.json`), follow-up (`.pi-dev/youtube-h264-hardware-choppy-followup.json`), metrics (`.pi-dev/youtube-h264-hardware-choppy-metrics.json`) | Hardware-copy 1080p60 still dropped about 44.5 frames/s with ample cache; video ran at about 0.905× wall time. The HDMI mode was 720p60. Drops persisted after packet capture stopped. Historical thermal flag `0x80000` was not current throttling. |
| Independent H.264 summary (`.pi-dev/independent-h264/receiver-summary.json`), benchmark (`.pi-dev/independent-h264/benchmark.json`) | User reproduced choppy picture with clear tone on a generated 1080p60 file. The same file exceeded 60 fps through hardware-copy with null output, narrowing the issue to the complete display/scheduling path without proving one bottleneck. |
| First output comparison (`.pi-dev/independent-h264/output-comparison-results.json`), follow-up comparison (`.pi-dev/independent-h264/output-comparison-v2-results.json`) | Overlay removal alone remained choppy. Fast profile helped; direct `v4l2m2m` DRM PRIME output was best. PBO upload and display-resample alternatives did not improve it. An old synthetic overlay label incorrectly said copy-back during the direct trial; observed decoder/format identifies the actual mode. |
| Fast validation (`.pi-dev/mpv-fast-validation.json`), accepted YouTube capture (`.pi-dev/youtube-fast-direct-accepted.json`), regression report (`.pi-dev/independent-h264/fast-regression-report.json`) | Direct `v4l2m2m` plus fast profile was physically accepted for generated 1080p60 and YouTube picture/sound. Drops improved to about 3.36/s and 7.88/s respectively, not perfect 60 fps. HEVC software and H.264 seek/clean-stop regressions passed. |
| Independent HEVC metadata (`.pi-dev/independent-hevc/fixture-metadata.json`), summary (`.pi-dev/independent-hevc/receiver-summary.json`) | Generated 720p25 HEVC MP4 and paced TS distinguish working decode/output from the failing real UHF source. |
| 4K GPU failures (`.pi-dev/hevc-4k/pi-results/20260910T212236-569f32.json`), Main 10 GPU failure (`.pi-dev/hevc-4k/pi-results/20260910T212746-1526f0.json`) | 4K60 GPU presentation dropped about 7.2% for Main and 62.5% for Main 10; Main 10 p95 A/V difference was about 503 ms. Decoder selection did not establish usable presentation. |
| Primary-plane Main (`.pi-dev/hevc-4k/pi-results/20260910T214912-0b1330.json`), Main 10 (`.pi-dev/hevc-4k/pi-results/20260910T214952-fd021e.json`), 120-second loop (`.pi-dev/hevc-4k/pi-results/20260910T215026-06f441.json`), viewer confirmation (`.pi-dev/hevc-4k/pi-results/visual-confirmation-overlay-primary-main10.json`) | Primary-plane video plus separate OSD fixed the tested output arrangement. The Main 10 loop had zero steady decoder/output drops, five startup output drops and 0.5 ms p95 A/V difference; user confirmed smooth picture and tone. This is 4K input scaled to 720p60, not native 4K HDMI or HDR qualification. |
| Integrated final report (`.pi-dev/hevc-airplay-final-report.json`), HEVC HTTP result (`.pi-dev/hevc-airplay-final-main10-result.json`), H.264 HTTP result (`.pi-dev/hevc-mode-final-h264-result.json`) | Receiver PID 12888 generations 2/3 exercised HEVC 4K60 Main 10 and H.264 1080p60 with normal progress/stop, zero decoder drops, and startup output drops only (19 and 5). Short automated integration checks are distinct from physical acceptance. |

Some old `overlay` JSON runs say `passed: true` despite rejected atomic commits:
those verdicts were invalidated by later log review. Warning-level atomic
commit rejection is a display failure even if output-drop counters stay zero.
The corrected `overlay-primary` results are separate. Keep KMS property files
beside the results: they record which planes actually carried video and OSD.
The direct-DRM null-output benchmark never confirmed hardware and provides no
hardware speed estimate. The FFmpeg 4K60 reference submitted 1,200 frames but
reported a V4L2 buffer-creation error, so it remains a failed diagnostic run.
The earlier HEVC kernel shutdown/accurate-seek hazard is still relevant; the
short successes do not close HEVC seeking or long-run recovery qualification.

The capability trial (`.pi-dev/audio-capabilities-verification.json`) failed when a full
`/server-info` mask triggered unsupported HTTP `/fp-setup` before `/play`.
Restoring the previous receiver restored YouTube. This was not a DRM fix and
must not be repeated as an audio-capability cure. Channel 4/protected AirPlay
remains a separate issue; this evidence directory does not prove a title-specific
DRM diagnosis. See the maintained playback findings for the protocol limit.

## Diagnostic gaps to retain

The old logging audits described an earlier checkout. Warning subscriptions,
actual pause/core-idle, track/output metadata, observation ages, bounded
terminal reports, selected packet capture and neutral stalled-playback text
were subsequently added. Malformed-request dumps, HLS upgrade-header dumps
and media-path logging were also removed. Do not re-open those old findings
without checking the current source. The following gaps remain worth tracking:

- **Demux-to-output evidence:** selected queue inserts do not establish decoder
  feed/receive counts, output submission, physical presentation or mpv's exact
  audio synchronization wait. If positive audio still cannot initialize, a
  narrow decoder/sync probe is needed; EAGAIN must remain distinct from failure.
- **HTTP association:** mpv-http-log-capability.json (`.pi-dev/mpv-http-log-capability.json`)
  confirmed fixed status/range/length observations via the existing IPC owner.
  Successful headers do not prove complete response bodies. Overlapping requests
  lack an unambiguous shared ID; do not associate a response with “last URL”.
  Direct HTTP bypasses FCUP/cache and GStreamer probes. IPC receipt time is not
  DNS/connect/TLS/first-byte time. Broad FFmpeg trace is high volume.
- **Correlation/coverage:** retain distinct receiver request, FCUP/cache, child,
  RAOP and renderer lifetimes. GStreamer mirror/RAOP diagnostic bindings and
  output-release completion need validation before treating the record as one
  complete causal chain. Source/segment/init/key completion and filter/track
  relationships are not uniformly observed. Unknown observer data is not zero.
- **GStreamer first-frame stall:** `playback_diagnostics_tick()` stops wait
  snapshots after the first video-sink buffer unless buffering is reported.
  Observers cap at 16 elements and optional pad/source coverage can be absent.
  Decoder/sink QoS drops are combined by maximum, not separate causes. Clock,
  preroll, timestamps and decoder/output deltas remain too coarse for some stalls.
- **RAOP diagnostics:** audio bus errors still log raw error text outside the
  structured diagnostic path. Renderer reuse resets a count baseline but retains
  first-observation metadata/lifetime; later caps/session evidence can be stale.
  Its audio counters do not diagnose direct mpv AAC or establish audibility.
- **Broad-debug privacy:** current `lib/raop.c`, `lib/http_handlers.h`,
  `lib/httpd.c` and legacy FCUP paths still contain raw headers, request/response
  bodies, plist strings, URLs or reverse-response dumps. Structured mpv output
  does not make blanket `-d` safe. Keep only fixed classes/numeric values and
  ephemeral correlation; no credentials, source URLs, keys or playlist contents.
- **Unqualified cases:** preserve tests/future checks for first-frame stalls,
  corrupt video, absent audio, discontinuities, truncated/delayed media,
  HTTP errors/redirects/ranges, separate audio, unavailable outputs, observer
  rejection/overflow, replacement and reused sessions. Synthetic successes do
  not close real UHF failure, HEVC seeking, mirror transitions, initial YouTube
  startup latency or longer real-stream stability.

## Fixture and evidence retention

Keep **all media**, including failed or negative controls, every HLS segment,
playlist and initialization file, plus their manifests and fixture metadata.
These include `audio-starvation-{intact,starved}.ts`, `hevc-probe.mp4`,
`h264-hardware-fixture.mp4`, `independent-h264/h264-1080p60.mp4`,
`independent-hevc/`, and the complete `hevc-4k/fixtures/` matrix. The separate
`hevc-4k/failed-2160p30-main10/video.mp4` is retained, not a validated fixture.
The 4K manifest hashes MP4, TS and both HLS forms; all are SDR BT.709, including
Main 10. Fixture generation and reusable runners are documented in `scripts/`
and the maintained HEVC test guide.

Retain raw JSON and unique failure logs even when reports overlap: captures
can have different time windows, retention truncation or terminal events.
Screenshots in this directory are generated UI previews, not substitute proof
of physical playback. Historical build and activation records identify earlier
releases; use the maintained release documentation before executing a helper.

This index replaces: `IMG2689-analysis.md`, `audio-starvation-report.md`,
`h264-hardware-analysis.md`, `hevc-packet-trial-analysis.md`,
`logging-audit-coordinator.md`, `logging-audit-mpv.md`,
`logging-audit-transport.md`, `mpv-preview-trial.md`,
`mpv-stall-semantics-IMG2689.md`, `youtube-h264-hardware-choppy-analysis.md`,
and `independent-h264/analysis.md`. References in old immutable JSON to those
note names should be interpreted through this index.

## Retained experimental tools

These remain because the maintained tests do not replace their exact experiment:

- `audio-starvation-experiment.py`: intact versus audio-starved controlled media.
- `h264-hardware-headless-check.py`: isolated hardware decoding and clean stop.
- `h264-output-comparison-v2.py` and `run-h264-output-comparison-v2.py`: latest
  direct/copy-back/display comparisons (version 1 was superseded).
- `run_hls_segment_cancel.py`: stall an adaptive media segment after data arrives;
  this differs from the maintained initial-HTTP-response cancellation regression.
- `airplay-fixture-seek-check.py` and `airplay-start-position-test.py`: seek and
  initial-position comparisons absent from the simpler maintained fixture sender.

These scripts may contain historical host addresses/build paths. Inspect their
arguments and update the test environment before a rerun. Some interrupt playback;
they are not read-only collectors. Release-specific activation/preparation scripts
and their copied mock tests were removed after verifying those activations had
completed. Use `scripts/pi-dev` for current build/deployment/managed rollback.
The old software-only test wrapper's recipe is retained in the playback findings.
Generated UI previews and passing build transcripts were removed; failing test
logs, device observations, release receipts and unique reports were retained.
