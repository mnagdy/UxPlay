# PiPlay display startup service

This installs the display stack qualified on 12 September into the existing
`uxplay.service`. The original unit, development override, receiver binary and
configuration remain the rollback target. It is the device integration step;
it is not an OS image or a firmware/early-boot splash.

## What runs

A small root-owned supervisor starts the pinned private seat broker, then
Weston, then UxPlay. Only the supervisor and seat broker run as root. Weston,
UxPlay, mpv and all capability probes run as the original receiver account.
GStreamer still handles mirroring and audio-only AirPlay; mpv handles direct
video. Receiver settings and identity come from the accepted R5 configuration.

A receiver exit is handled inside the supervisor. It terminates the complete
receiver/player process group before restarting the receiver, leaving the same
Weston process in control of HDMI. Three rapid receiver retries are allowed;
further failure restarts the whole stack. A seat broker or compositor exit
also restarts the stack. systemd limits repeated stack starts to three per
minute. An exhausted limit leaves the service failed; use the installed
rollback command rather than starting a second receiver.

The supervisor uses `KillMode=mixed`: normal stop gives it time to reap its
children; systemd kills remaining service processes if cleanup cannot finish.
Child output goes to the journal, not the projector console. Copied executable
code (the private plugin and seat broker) is staged under the root-owned
`/var/cache/piplay-display` service cache. `/run/piplay-display` holds only
health and Wi-Fi restoration state: Raspberry Pi OS mounts `/run` with
`noexec`, which blocks both shared-library mapping and binary execution.
Preparation and startup reject a non-executable cache filesystem. Startup
removes abandoned private cache workspaces under the display lock after
systemd has reaped the previous service cgroup. The original
service already starts at boot, so changing its dedicated override selects
this stack for subsequent boots. The compositor will still be recreated when
the whole service or machine restarts; no claim is made about console exposure
during early boot or an injected compositor failure.

The optional `--wifi-interface wlan0` setting keeps Wi-Fi power saving off only
while the display service runs. A root-owned receipt records the prior setting
before changing it, and `ExecStopPost` restores and verifies it even if the
supervisor is killed. This reproduces the setting in the one accepted R5 audio
run; it does not establish power saving as the cause of earlier choppiness.
No NetworkManager connection profile is edited. Receiver restarts retain the
setting; a full service restart restores it and records it again.

## Prepare and inspect

On the Pi, stage `scripts/pi-display-service` and `scripts/pi-display-trial`
together. Use the tested trial plan as the input:

```sh
python3 scripts/pi-display-service prepare \
  --trial /home/mo/uxplay-dev/display-trial/prepared-r5-audio \
  --output /home/mo/uxplay-dev/display-service-staging/prepared-startup \
  --wifi-interface wlan0
python3 scripts/pi-display-service check \
  /home/mo/uxplay-dev/display-service-staging/prepared-startup
```

Preparation/checking do not stop the receiver, change Wi-Fi or install a unit.
The private package contains the configuration, pinned broker, both launchers,
hashes, the exact rollback command and hashes of its unit/configuration files.
`95-piplay-display.conf` shows the proposed override. Capability checks exercise
the real receiver/mpv startup handshake and the private Wayland plugin.

Installation copies the launchers, broker and configuration into a root-owned
versioned directory under `/opt/piplay/display/releases`. The qualified receiver,
Weston/mpv build prefixes and their private dependencies remain at their
existing paths. Their pinned executable/module hashes are rechecked at startup;
this is not yet a self-contained distributable package. Keep these build
prefixes until a later packaging milestone replaces them.

## Activate and roll back

```sh
sudo python3 scripts/pi-display-service install \
  /home/mo/uxplay-dev/display-service-staging/prepared-startup
```

The installer refuses a changed baseline or an existing PiPlay override. It
seals and rechecks the package, stops the old service, selects the new override,
and requires stable supervisor/child process health. If activation fails, it
stops all display processes, removes only its own override, restarts the exact
original command and checks that it remains running. If child termination or
the rollback files cannot be verified, it stops with an error rather than
starting a competing receiver. Activation process health is not a physical
ready-screen or playback test.

The successful installer prints the exact rollback command:

```sh
sudo python3 /opt/piplay/display/releases/RELEASE/pi-display-service rollback
```

Rollback does not require the candidate's external decoder/compositor binaries
to remain usable. It verifies the installed control package and original
service files, stops the new stack, and restores the original command. The
original unit and `90-uxplay-dev.conf` are never rewritten. Do not use `pi-dev
activate` while the supervisor owns `uxplay.service`; its existing account/
command checks intentionally reject this root-supervised service. Roll back
first or prepare a subsequent display package.

Use `journalctl --all -u uxplay -b` when reading audio evidence. Without
`--all`, carriage-return progress records appear as blob data and can hide
adjacent diagnostic messages.

## Verification boundary

Run the Python lifecycle suites on the Pi:

```sh
python3 -m unittest discover -s tests -p 'test_display_*.py'
```

Tests cover ordered cleanup/rollback, missing candidate rollback, concurrent
override rejection, receiver-only recovery, crash-loop bounds and preservation
of the Wi-Fi restoration receipt. The Linux process test terminates a real
receiver/player process group. The shared trial suite retains its permission,
plugin-selection and existing bounded-trial checks. Root-only cases must also
be exercised where a privileged Linux test environment is available.

Before accepting startup integration, observe the ready screen, one real
mirroring/direct/audio handover, receiver-only recovery with an unchanged
compositor PID, complete-stack recovery, and a reboot returning to ready.
Repeat audio starts and a longer audio run; the prior single passing audio
session is not a long-run stability result. Then explicitly exercise the
installed rollback and reactivation path.

Systemd behavior references: [process-group stop behavior](https://github.com/systemd/systemd/blob/main/man/systemd.kill.xml)
and [runtime directory lifecycle](https://github.com/systemd/systemd/blob/main/man/systemd.exec.xml).
