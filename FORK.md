# Raspberry Pi UHF compatibility branch

This fork starts from upstream UxPlay **v1.73.7**, adds direct HTTP/HTTPS video
compatibility, and contains the projector receiver work. The upstream
[README](README.md), platform helpers and licenses are retained.

## Recorded status — 11 September 2026

The latest documented Pi release is
`20260910T213612844711Z-52c2997e3800-dirty`. It uses the optional mpv backend for
direct AirPlay video and its audio, with `pi4-hevc-experimental` decoding and
the `fast` render profile. GStreamer remains the build default and handles
mirroring and audio-only AirPlay. This is the last saved device result, not a
live-device verification by the repository cleanup.

The user confirmed YouTube/iPlayer picture and sound and fast YouTube video
switching. An independent two-minute 4K60 Main 10 HEVC trial had smooth picture
and tone, clean stop, and no steady-playback output drops. Short receiver checks
also passed HEVC and H.264 playback/stop. The HDMI display was **720p60**: these
results concern decoding 4K source material, not native 4K HDMI output.

Still open:

- **UHF combined video/audio playback.** Audio-only AirPlay works, but some
  video sessions are silent or stall. Captured GStreamer fragments had no audio
  samples; a later mpv capture had 1,250 selected video packets and zero selected
  audio packets. On 11 September, three complete Sky News fragments from the
  phone export contained 750 video samples and zero audio samples, despite sound
  inside UHF. A later UHF log records exporter write errors. These narrow that
  sample to an export-writing problem; the exact cause and fix remain open.
  See [the direct export investigation](docs/diagnostics/2026-09-11-sky-news-export.md).
- **Channel 4 compatibility.** A malformed/missing AES-128 key-download bug was
  reproduced in GStreamer 1.26.2 and an isolated patch was tested. Channel 4's
  actual encryption scheme and key response were not established. The patch
  does not add DRM support and ordinary receiver builds do not install it.
- **Initial YouTube startup.** Switching improved and the old item stays paused,
  but one initial/resumed start still took about eight seconds after most cache
  preparation was already complete.
- **Qualification and diagnostics.** Longer real UHF/HEVC playback, HEVC seeking,
  mirroring/audio handovers, both phones, and remaining GStreamer/RAOP observation
  gaps need verification. See the findings and logging records below.

## Documentation

| Record | Purpose |
| --- | --- |
| [Playback findings](docs/playback-findings.md) | Consolidated resolved bugs, Pi/GStreamer/HEVC quirks, unresolved work and independent test reruns |
| [Development and rollback](docs/development-on-pi.md) | Maintained `scripts/pi-dev` build, deployment, recovery and regression workflow |
| [Original installation](docs/raspberry-pi-uhf.md) | Historical tagged GStreamer baseline and initial service setup |
| [mpv and HDMI configuration](docs/mpv-screen-development.md) | Current options, backend ownership, screen behavior and implementation constraints |
| [HEVC qualification suite](docs/hevc-4k-test-plan.md) | Preserved fixtures, staged reruns, display-plane findings and acceptance gates |
| [HEVC receiver trial](docs/hevc-airplay-trial.md) | Exact hardware policy, short receiver results and rollback history |
| [Startup and switching](docs/startup-switch-regression.md) | Latest changes, measured startup stages and remaining delay |
| [Playback logging audit](docs/playback-logging-audit.md) | Captured evidence, interpretation limits and outstanding probes |
| [GStreamer dependency patch](patches/README.md) | AES-128 key validation and encrypted regression tests |

The [dated investigations](docs/diagnostics/) preserve the detailed UHF evidence.
Generated media, reports and selected experimental helpers remain in `.pi-dev/`;
the versioned [evidence index](docs/local-evidence.md) distinguishes historical observations
from open issues. The raw reports and media are local and ignored by Git. Keep media, HLS segments,
playlists and fixture metadata together when backing them up.

All maintained `scripts/` and `tests/` files are retained. A successful software
test or active service does not establish visible picture, audible sound or
safe hardware teardown; device acceptance is recorded separately.

## Provenance and recovery history

- Upstream base: `df67c212a433cf6dda3676dd40c097900d24e645` (`v1.73.7`).
- Direct-video backport: `6f9250414ee33a4e008b6a7638ac448ace0395c9` and
  routing follow-up `2ece5790c0cd834291006f0ceb5b6b0e6a7abbee`.
- Initial reproducible compatibility tag: `v1.73.7-uhf.1`; development branch:
  `pi-uhf`. The executable version alone remains `1.73.7` and cannot identify
  later builds.
- Verified 9 September baseline commit:
  `52c2997e380084a44a0bea95846bbbd5e444013d`, preserving release
  `20260909T101349217073Z-cd33afca3ada-dirty`.
  Executable SHA-256:
  `0b2e79b31ead385e07eef59e2012117042d5a104870d99e393bd0a9ea46ab8a2`.
  Source fingerprint:
  `907c7d7bde908bd5a6bc9bb682777b4a4b9744b0dd8bd1bba371de4d3d7a9621`.
- The separate stable `/usr/local/bin/uxplay` and original
  `/usr/local/bin/uxplay-uhf-test` installations are historical rollback paths.
  Later development releases use the managed service override and rollback
  described in the development guide; do not confuse the two override schemes.

The cleanup preserves the existing uncommitted source and test changes. Local
publication-result JSON files record later publication checkpoints; the checkout's
HEAD alone does not identify the deployed receiver. Check source and binary
fingerprints before making a new deployment.
