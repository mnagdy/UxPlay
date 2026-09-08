# Raspberry Pi UHF compatibility branch

This branch starts from upstream UxPlay **v1.73.7** and backports the existing direct HTTP/HTTPS video playback implementation. It is a small compatibility branch, not an official UxPlay release.

The original upstream README and licenses are retained. General upstream documentation is in [README.md](README.md).

## Provenance

- Base: `df67c212a433cf6dda3676dd40c097900d24e645` (`v1.73.7`).
- Direct video handling: `6f9250414ee33a4e008b6a7638ac448ace0395c9`.
- Follow-up routing change: `2ece5790c0cd834291006f0ceb5b6b0e6a7abbee`.
- Both changes were cherry-picked with their original authors and upstream commit references.

In stock 1.73.7, a direct URL such as `http://phone-address:port/stream.m3u8` is rejected by the YouTube-oriented URL handling. This branch forwards ordinary HTTP/HTTPS locations to the existing GStreamer playback path. It does not depend on the playlist being named `master.m3u8`.

## Installation

See [the Raspberry Pi guide](docs/raspberry-pi-uhf.md). The reproducible starting point is tag **`v1.73.7-uhf.1`**; development continues on **`pi-uhf`**. The executable still reports `1.73.7`, so retain the source revision and distinguish the installed binary by its filename.

## Evidence and remaining work

The equivalent backport was built on a Raspberry Pi 4 running Raspberry Pi OS Lite. Its user reported successful UHF live TV and series playback. This is a limited device report, not an exhaustive compatibility test.

Open investigation items:

- Roughly 20 seconds from selecting the AirPlay receiver to video starting.
- Low frame rate with some 4K content; the selected decoder and power/thermal state have not been measured.
- Receiver discovery became unavailable during later troubleshooting; cause and recovery are unconfirmed.
- Channel changes, reconnects, phone lock/background behaviour, two-phone handover, extended playback and long seeks need recorded verification.

Keep playbin3 (`hls`) as the initial configuration. A playbin2 (`hls 2`) experiment is not a validated improvement.

Potential development areas are startup timing instrumentation, live-stream playback status, buffering/pause handling and error recovery. Establish measurements before changing these behaviours. This branch does not add DRM support, HEVC hardware drivers, or a proven low-latency configuration.
