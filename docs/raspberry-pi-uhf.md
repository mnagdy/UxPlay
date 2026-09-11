# Original UHF installation and recovery baseline

This installs the historical `v1.73.7-uhf.1` GStreamer build, not the later mpv
receiver or its HEVC/startup fixes. For the current documented receiver, start
with [the fork overview](../FORK.md) and use [the managed development workflow](development-on-pi.md).

These instructions target a Raspberry Pi 4 with HDMI output and an existing UxPlay service. Start from [upstream installation instructions](../README.md) if UxPlay has not been installed before. Use the same Linux account as the original service.

Run one block at a time. If any command reports an error, stop and retain the output before proceeding.

The build uses tag `v1.73.7-uhf.1`: upstream 1.73.7 plus two upstream direct-video changes. Its startup version remains `1.73.7`; the separate binary name is `uxplay-uhf-test`.

## Dependencies

If you already compiled UxPlay 1.73.7, these should be installed. Otherwise:

```bash
sudo apt update
sudo apt install -y --no-install-recommends \
  git ca-certificates build-essential cmake pkg-config \
  libssl-dev libplist-dev libavahi-compat-libdnssd-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-libav gstreamer1.0-alsa alsa-utils avahi-daemon
```

## Download and compile

This creates a new directory and stops on failure. If the directory already exists, inspect it before proceeding. Do not apply the older manual backport instructions to this source: the fixes are already included.

```bash
(
set -e
mkdir -p ~/src
git clone --depth 1 --branch v1.73.7-uhf.1 \
  https://github.com/mnagdy/UxPlay.git ~/src/uxplay-uhf-fork
cd ~/src/uxplay-uhf-fork
git rev-parse HEAD
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNO_X11_DEPS=ON
cmake --build build --parallel 2
)
```

## Install separately

Finish any current stream first. This preserves the original `/usr/local/bin/uxplay` binary. Stop the service before replacing a previously installed test binary:

```bash
sudo systemctl stop uxplay
sudo install -m 755 ~/src/uxplay-uhf-fork/build/uxplay \
  /usr/local/bin/uxplay-uhf-test
/usr/local/bin/uxplay-uhf-test -v
```

If installation fails, retain the error and restart the existing service with `sudo systemctl start uxplay`.

## Configuration and hardware access

Inspect the original service with `systemctl cat uxplay`. Preserve its existing user, configuration and device permissions. The service account needs access to the audio, video and render devices. If those memberships are missing, add them and log out/back in before a foreground test:

```bash
sudo usermod -aG audio,video,render "$USER"
sudo systemctl enable --now avahi-daemon
```

Copy the existing configuration only if a test configuration does not already exist:

```bash
cp -n ~/.uxplayrc ~/.uxplayrc.uhf-test
nano ~/.uxplayrc.uhf-test
```

A minimal HDMI configuration contains:

```text
n Projector
nh
s 1920x1080
fps 30
vs kmssink
as alsasink
hls
nohold
nofreeze
```

Keep exactly one `hls` line. `hls` selects playbin3. The dimensions and frame rate apply to screen mirroring, not direct-stream downscaling. The direct-video renderer uses ALSA's default output; an explicit `as alsasink` alone does not set playbin's audio device. Preserve the working `~/.asoundrc` and check audio with `aplay -l` and `speaker-test -D default -c 2 -t wav -l 1`.

The original HDMI audio repair set the receiver user's ALSA default to the
verified `vc4hdmi0` output. This was the working `~/.asoundrc` recipe:

```text
pcm.!default {
    type plug
    slave.pcm "hw:CARD=vc4hdmi0,DEV=0"
}
ctl.!default {
    type hw
    card vc4hdmi0
}
```

Recreate it only if needed after checking the actual audio card and preserving
any existing custom configuration. mpv uses its own `mpv-audio-device` setting;
working audio-only AirPlay does not establish audio packets in a UHF video stream.

## Foreground test

The service must remain stopped while this copy is running:

```bash
/usr/local/bin/uxplay-uhf-test -rc "$HOME/.uxplayrc.uhf-test"
```

Use UHF's own AirPlay button to select Projector. Verify sound and picture before trying channel changes and reconnects. Keep the phone awake for the first test; evaluate locking it separately. Press Ctrl+C to stop.

If testing fails, retain the terminal output. Restart the existing service, or use the rollback procedure below if it was already configured to use the test binary.

## Start at boot

This assumes the existing `uxplay.service` runs as the currently logged-in user. Check `systemctl cat uxplay` first. If it uses a different account or custom command-line flags, adapt the override to preserve those choices.

Only proceed after a successful foreground test. The shell expands the current account's home directory into the service configuration:

```bash
sudo mkdir -p /etc/systemd/system/uxplay.service.d
sudo tee /etc/systemd/system/uxplay.service.d/50-uhf-test.conf >/dev/null <<EOF
[Service]
ExecStart=
ExecStart=/usr/local/bin/uxplay-uhf-test -rc "$HOME/.uxplayrc.uhf-test"
EOF
sudo systemctl daemon-reload
sudo systemctl enable uxplay
sudo systemctl restart uxplay
sudo systemctl status uxplay --no-pager
```

Check the running command uses `uxplay-uhf-test`. Reboot and confirm the receiver works before logging in through SSH. Test both phones, video/audio, stop/reconnect and projector off/on. An active service alone does not establish media playback success.

## If the receiver disappears

Restore the normal `hls` line in the test configuration. Then:

```bash
sudo systemctl restart avahi-daemon
sudo systemctl restart uxplay
systemctl is-active uxplay avahi-daemon
```

Expect `active` twice. If not, inspect:

```bash
sudo journalctl -u uxplay -b -n 40 --no-pager
sudo journalctl -u avahi-daemon -b -n 20 --no-pager
```

If both are active, verify the Pi's current address with `hostname -I`, its Wi-Fi state with `nmcli dev`, and that the phone is on the same non-isolated network. `sudo nmtui` provides a keyboard-driven network connection menu.

## Rollback

Stop any manually running test with Ctrl+C, then remove only this override:

```bash
sudo rm -f /etc/systemd/system/uxplay.service.d/50-uhf-test.conf
sudo systemctl daemon-reload
sudo systemctl restart uxplay
```

This restores the original service command and original binary, including their original compatibility limits. It leaves the source and test binary available for further investigation.

## Before performance changes

Measure startup time with a 1080p stream, then compare 4K. Note CPU use (`top`), power/thermal flags (`vcgencmd get_throttled`), temperature (`vcgencmd measure_temp`), GStreamer version and the actual decoder selected. Comparing Ethernet with Wi-Fi can help isolate the network path. Decoder availability from `gst-inspect-1.0` does not prove that a particular decoder is used.

See [the retained playback findings](playback-findings.md) for later startup,
H.264 and HEVC results and the remaining UHF failures. Those results do not
apply to the original tagged build installed by this guide.
