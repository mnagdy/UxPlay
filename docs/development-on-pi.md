# Develop on the Mac, run on the Pi

Use the Mac as the source checkout and the Pi as the native build/test machine. The helper sends a snapshot of local edits over SSH, builds incrementally on the Pi, then switches the existing receiver service. GitHub is for commits and sharing; pushing a commit is not required for each test.

The development helper is separate from UxPlay's playback implementation. Hardware playback and successful discovery must still be checked on the actual Pi.

## One-time preparation

On the Pi, install the small additions used by the development helper:

```bash
sudo apt install rsync ccache python3
```

The C++/CMake/GStreamer build dependencies should already be installed from the [setup guide](raspberry-pi-uhf.md). The helper retains `Release`, `NO_X11_DEPS=ON` and two build jobs for an initial comparable baseline. It uses ccache if available. No kernel upgrade is part of this setup.

On the Mac, open a terminal in this repository. Python 3, SSH and rsync must be available. Create `.pi-dev.json`, replacing the example user and hostname with the Pi's SSH login:

```json
{"host": "youruser@projector.local"}
```

This file is ignored by Git and excluded from transfers. If `.local` discovery is unreliable, use the current IP shown by `hostname -I` on the Pi. A router DHCP reservation can keep that address stable. An existing SSH alias also works.

Check the connection and installed tools:

```bash
./scripts/pi-dev check
```

You can override the saved host for any command:

```bash
./scripts/pi-dev check --host youruser@192.168.1.124
```

The IP above is an example, not discovery of your Pi. Use normal SSH host-key verification. An SSH key makes unattended build commands convenient; establish ordinary `ssh youruser@host` access first. Agent runs use SSH batch mode and cannot answer password prompts. Interactive deployment can ask for the Pi's sudo password when switching the service. No password is stored and no sudoers rules are installed.

## The iteration loop

1. Edit locally with Codex.
2. Run the deploy command from the Mac:

```bash
./scripts/pi-dev deploy
```

3. Select the receiver in UHF and test the change.
4. Inspect recent service logs from the Mac:

```bash
./scripts/pi-dev logs
```

The first build compiles the whole project. Later builds reuse the same CMake build directory and only rebuild affected code. Actual build times need measurement on the Pi. The receiver remains running during compilation, though compilation competes for CPU and can disturb playback; measure video performance after the build ends. A successful deployment restarts the service and interrupts the current stream.

To compile without switching the receiver:

```bash
./scripts/pi-dev build
```

To return to the previous development release:

```bash
./scripts/pi-dev rollback
```

To restore the receiver command used before these development deployments:

```bash
./scripts/pi-dev stable
```

The latter removes only `90-uxplay-dev.conf`. An existing `50-uhf-test.conf` remains in place, so the pre-development UHF build stays available. Existing installed binaries and original configuration files are not overwritten. The name `stable` describes restoring that baseline; it does not certify its playback reliability.

## What is transferred and saved

Tracked files and nonignored new files are copied into a local snapshot. Deleted files remain deleted; local settings, Git internals and build products are excluded. Source symlinks are rejected. Avoid editing the disposable source directory on the Pi: the next transfer replaces its contents.

```text
~/uxplay-dev/
  src/             source mirror; the only rsync deletion boundary
  build/Release/   persistent native build cache
  releases/        separate binaries, source archives and build metadata
  state/           deployment state and lock
```

Each release has a timestamp, Git revision and dirty marker, plus a fingerprint of the transferred source. This identifies uncommitted test builds even though the executable's existing version string still says 1.73.7. Activation snapshots the selected configuration for that release.

The helper checks the existing service account and command before writing its dedicated override. Custom command-line arguments are rejected rather than silently discarded. It checks that the intended executable remains running with a stable process ID and restart count, and attempts to restore the previous override if activation fails. These process checks do not prove that AirPlay discovery, audio or video works.

## Recovery and validation

If a new build fails to compile, the running service stays unchanged. If a build runs but has a playback regression, use `rollback` or `stable` and retain the failing release's logs and source archive.

If SSH is interrupted, inspect `~/uxplay-dev/state/lock` and check for an active build/deployment before removing a stale lock. Do not clear it while another deployment is running. Neither lock recovery nor old-release deletion is automatic.

After every relevant playback change, test discovery, sound/picture, pause/resume, channel change, stop/reconnect and switching phones. Record startup time and frame behaviour using the same sample streams. Logs can contain media URLs or tokens; review them before sharing publicly.

Run the local deployment-helper tests without a Pi:

```bash
python3 -m unittest discover -s tests -p 'test_pi*.py'
```

The playback code also has optional C/GStreamer tests. They use generated video and headless sinks, so they do not take over the projector. In a Linux build environment with the existing development dependencies and GStreamer's base/good plugins:

```bash
cmake -S . -B build/tests -DNO_X11_DEPS=ON -DUXPLAY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tests --parallel 2
ctest --test-dir build/tests --output-on-failure
```

The tests cover buffering and user intent, safe time conversion, the production renderer's startup/control path, stream replacement, live progress, and diagnostics through actual playbin3 playback. `tests/Dockerfile` provides a Debian Trixie environment for running them on a Mac with Docker; this checks Linux code but does not emulate the Pi's video hardware.

YouTube cache tests also cover interrupted collection and complete-cache reuse. When Python 3 and FFmpeg are installed at configuration time, CTest adds an HTTP HLS test using generated H.264 video and a separate AAC audio playlist. It checks advancing playback from zero and from a 165-second resume point. A mixed H.264/VP9 master reproduces the GStreamer 1.26.2 demux failure; the test then runs the production Pi 4 playlist filter and verifies that both initial playback and resume advance. FFmpeg needs the libx264, libvpx-vp9 and AAC encoders. The test runtime needs GStreamer's HLS, MPEG-TS and codec plugins; the Dockerfile includes these dependencies. All media and HTTP serving stay local to the test environment.

For the first playback iteration, deploy and test a 1080p stream followed by the problematic 4K stream. Collect `Direct playback:` entries using `./scripts/pi-dev logs`, together with the seconds from selecting the receiver to visible video. Also test pause/resume, seeking past 40 minutes in a series, stop/reconnect and channel changes. Current diagnostics observe first video-sink buffer arrival, not physical presentation; they do not yet distinguish manifest download from media-segment download.

Once native incremental builds are measured, consider cross-compilation or CI if compilation is the bottleneck. A Mac build itself cannot validate Linux GStreamer/DRM behaviour. CMake supports reusable build directories through [its configure/build commands](https://cmake.org/cmake/help/latest/manual/cmake.1.html); [rsync](https://download.samba.org/pub/rsync/rsync.1) handles incremental transfer.

## Raspberry Pi 4 YouTube compatibility

Add `hls-pi4` on its own line in the receiver configuration (or pass `-hls-pi4` on the command line) to enable HLS and keep cached YouTube video within the tested H.264/AAC-LC path. The profile retains available variants with declared H.264 and AAC-LC codecs, a positive resolution no larger than 1920×1080, and no declared frame rate above 60 fps. Adaptive quality selection remains available among compatible variants. If none qualify, playback is rejected with a diagnostic message.

This selects from declared playlist metadata; it does not transcode media or guarantee that every H.264 profile/bit depth is hardware-decodable. Ordinary HTTP streams from UHF use their existing route. Removing `hls-pi4` restores the default playlist choices. Keep the existing `hls` line if HLS should remain enabled after removing the profile.
