"""Display trial preparation and restoration safeguards without Pi hardware."""
import argparse
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/pi-display-trial"
loader = importlib.machinery.SourceFileLoader("display_trial", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
trial = importlib.util.module_from_spec(spec)
loader.exec_module(trial)


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="display trial ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        release = self.root / "release"
        release.mkdir()
        for name in ("uxplay", "weston", "seatd", "drm-backend.so", "kiosk-shell.so"):
            path = release / name
            path.write_text("fixture " + name)
            path.chmod(0o700)
        (release / "build.json").write_text(json.dumps({"binary_sha256": trial.digest(release / "uxplay"),
                                                       "mpv_backend": True}))
        self.original = ('# Retain this comment and receiver identity.\nn "Living Room TV"\n'
                         'pin 1234\nas "alsasink device=hw:1,0"\nmpv-audio-device alsa/hdmi\n'
                         'hls-pi4\nfs\nvs "kmssink driver-name=vc4"\n'
                         'mpv-decode pi4-hevc-experimental\nmpv-vo gpu\n'
                         'mpv-gpu-context drm\nmpv-gpu-api opengl\n'
                         'mpv-drm-device /dev/dri/card1\nmpv-drm-connector HDMI-A-1\n'
                         'mpv-render-profile fast\nscreen-info debug\n')
        source = self.root / "original.conf"
        source.write_text(self.original)
        self.args = argparse.Namespace(release=str(release), config=str(source), output=str(self.root / "prepared"),
                                       weston=str(release / "weston"), seatd=str(release / "seatd"),
                                       weston_backend=str(release / "drm-backend.so"),
                                       weston_shell=str(release / "kiosk-shell.so"), library_dir=[], output_name="HDMI-A-1")

    def prepare(self):
        trial.prepare(self.args)
        return Path(self.args.output)

    def test_preserves_identity_audio_pairing_and_unrelated_config(self):
        output = self.prepare()
        config = (output / "receiver.conf").read_text()
        self.assertEqual(Path(self.args.config).read_text(), self.original)
        for line in self.original.splitlines(keepends=True):
            if trial.config_key(line) not in trial.REPLACED:
                self.assertIn(line, config)
        self.assertTrue(config.endswith(trial.TRIAL_OPTIONS))
        self.assertNotIn("kmssink", config)
        self.assertNotIn("mpv-gpu-context", config)
        self.assertNotIn("mpv-drm-", config)
        self.assertNotIn("fullscreen=", config)
        self.assertNotIn("fs\n", config)
        self.assertEqual(trial.trial_config(config), config)
        for path in (output, output / "receiver.conf", output / "trial.json"):
            self.assertEqual(path.stat().st_mode & 0o077, 0)
        plan = trial.load_plan(output)
        self.assertEqual(plan["seatd_socket"], "seatd.sock")

    def test_exact_keys_only_and_no_final_newline(self):
        config = trial.trial_config('# -vs kmssink\n -vs "kmssink"\nmpv-audio-device alsa/test\nn PiPlay')
        self.assertIn('# -vs kmssink\n', config)
        self.assertNotIn(' -vs "kmssink"', config)
        self.assertIn("mpv-audio-device alsa/test\nn PiPlay\n", config)

    def test_blank_lines_are_preserved(self):
        source = "# Device settings\n\n   \n\t\nvs kmssink\nn Projector\n"
        self.assertEqual(trial.trial_config(source),
                         "# Device settings\n\n   \n\t\nn Projector\n" + trial.TRIAL_OPTIONS)

    def test_changed_release_cannot_be_prepared(self):
        (Path(self.args.release) / "uxplay").write_text("changed executable")
        with self.assertRaisesRegex(trial.Failure, "immutable release"):
            self.prepare()
        self.assertFalse(Path(self.args.output).exists())

    def test_preparation_does_not_overwrite_an_existing_trial(self):
        output = self.prepare()
        original = (output / "trial.json").read_bytes()
        with self.assertRaises(FileExistsError):
            self.prepare()
        self.assertEqual((output / "trial.json").read_bytes(), original)

    def test_config_changes_and_executable_changes_are_rejected(self):
        output = self.prepare()
        config = output / "receiver.conf"
        original = config.read_bytes()
        config.write_text(config.read_text() + "mpv-gpu-context drm\n")
        with self.assertRaisesRegex(trial.Failure, "configuration has changed"):
            trial.load_plan(output)
        config.write_bytes(original)
        Path(self.args.weston).write_text("different Weston")
        with self.assertRaisesRegex(trial.Failure, "module has changed"):
            trial.load_plan(output)

    def test_symlinks_and_public_trial_files_are_rejected(self):
        output = self.prepare()
        (output / "trial.json").chmod(0o644)
        with self.assertRaisesRegex(trial.Failure, "private"):
            trial.load_plan(output)
        (output / "trial.json").chmod(0o600)
        target = Path(self.args.weston)
        saved = target.with_name("weston-real")
        target.rename(saved)
        target.symlink_to(saved)
        with self.assertRaisesRegex(trial.Failure, "symbolic link"):
            trial.load_plan(output)

    def add_private_mpv(self):
        executable = self.root / "private mpv"
        executable.write_bytes(b"private mpv fixture")
        executable.chmod(0o700)
        self.args.mpv_executable = str(executable)
        return executable

    def test_optional_mpv_is_pinned_and_replaces_only_trial_executable(self):
        source = Path(self.args.config)
        source.write_text(self.original + "mpv-executable /old/mpv\n -mpv-executable /other/mpv\n")
        original = source.read_bytes()
        executable = self.add_private_mpv()
        output = self.prepare()
        plan = trial.load_plan(output)
        config = (output / "receiver.conf").read_text()
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(plan["files"]["mpv"], {"path": str(executable), "sha256": trial.digest(executable)})
        self.assertEqual(config.count("mpv-executable"), 1)
        self.assertTrue(config.endswith(trial.TRIAL_OPTIONS + f'mpv-executable "{executable}"\n'))
        self.assertEqual(trial.trial_config(config, str(executable)), config)

    def test_without_override_existing_mpv_setting_is_unchanged(self):
        source = Path(self.args.config)
        source.write_text(self.original + 'mpv-executable "/baseline mpv"\n')
        output = self.prepare()
        plan = trial.load_plan(output)
        self.assertNotIn("mpv", plan["files"])
        self.assertNotIn("libinput_quirks_dir", plan)
        self.assertNotIn("LIBINPUT_QUIRKS_DIR", trial.dependency_environment(plan))
        self.assertIn('mpv-executable "/baseline mpv"\n', (output / "receiver.conf").read_text())

    def test_changed_pinned_mpv_is_rejected_before_preflight_commands(self):
        executable = self.add_private_mpv()
        output = self.prepare()
        plan = trial.load_plan(output)
        executable.write_bytes(b"changed private mpv")
        with self.assertRaisesRegex(trial.Failure, "executable or module has changed"):
            trial.load_plan(output)
        with patch.object(trial, "capture") as capture, \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            with self.assertRaisesRegex(trial.Failure, "pinned mpv executable"):
                trial.preflight(plan)
        capture.assert_not_called()

    def test_pinned_mpv_configuration_tamper_is_rejected_even_with_new_config_hash(self):
        executable = self.add_private_mpv()
        output = self.prepare()
        config = output / "receiver.conf"
        config.write_text(config.read_text().replace(str(executable), "/another/mpv"))
        manifest = output / "trial.json"
        plan = json.loads(manifest.read_text())
        plan["config_sha256"] = trial.digest(config)
        manifest.write_text(json.dumps(plan))
        with self.assertRaisesRegex(trial.Failure, "configuration has changed"):
            trial.load_plan(output)

    def test_optional_mpv_preflight_checks_pinned_output_and_libraries(self):
        executable = self.add_private_mpv()
        Path(self.args.seatd).write_bytes(b"fixture seatd.sock\0")
        output = self.prepare()
        plan = trial.load_plan(output)
        markers = "-display-owner pi4-wayland-experimental -check-startup weston -u -n fullscreen dmabuf-wayland"
        with patch.object(trial, "capture", return_value=(0, markers)) as capture, \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            trial.preflight(plan)
        commands = [call.args[0] for call in capture.call_args_list]
        self.assertTrue(any(argv[-3:] == [str(executable), "--no-config", "--vo=help"] for argv in commands))
        self.assertTrue(any(argv[-2:] == ["/usr/bin/ldd", str(executable)] for argv in commands))

    def startup_check_plan(self):
        self.add_private_mpv()
        Path(self.args.seatd).write_bytes(b"fixture seatd.sock\0")
        return trial.load_plan(self.prepare())

    def test_receiver_startup_check_precedes_remaining_checks_without_live_display_environment(self):
        plan = self.startup_check_plan()
        markers = "-display-owner pi4-wayland-experimental -check-startup weston -u -n fullscreen dmabuf-wayland"
        with patch.object(trial, "capture", return_value=(0, markers)) as capture, \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            trial.preflight(plan)
        commands = [call.args[0] for call in capture.call_args_list]
        receiver = plan["files"]["receiver"]["path"]
        self.assertEqual(commands[0][-4:], [receiver, "-rc", plan["config_path"], "-h"])
        self.assertEqual(commands[1][-4:], [receiver, "-rc", plan["config_path"], "-check-startup"])
        self.assertEqual(commands[2][-2:], [plan["files"]["weston"]["path"], "--version"])
        self.assertFalse(any(item.startswith(("WAYLAND_DISPLAY=", "XDG_RUNTIME_DIR=")) for item in commands[1]))

    def test_receiver_without_startup_check_capability_is_rejected(self):
        plan = self.startup_check_plan()
        with patch.object(trial, "capture", return_value=(0, "-display-owner pi4-wayland-experimental")) as capture, \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            with self.assertRaisesRegex(trial.Failure, "receiver capability check failed"):
                trial.preflight(plan)
        self.assertEqual(capture.call_count, 1)

    def test_receiver_startup_rejection_prevents_service_and_display_actions(self):
        plan = self.startup_check_plan()
        host = FakeHost()

        def preflight():
            host.event("preflight")
            trial.preflight(plan)

        host.preflight = preflight
        responses = [(0, "-display-owner pi4-wayland-experimental -check-startup"),
                     (1, "PRIVATE_SENTINEL: Error parsing option osc")]
        with patch.object(trial, "capture", side_effect=responses) as capture, \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            with self.assertRaisesRegex(trial.Failure, "receiver startup capability check failed") as raised:
                trial.trial_lifecycle(host, 10)
        self.assertEqual(capture.call_count, 2)
        self.assertEqual(host.events, ["preflight", "cleanup"])
        self.assertNotIn("PRIVATE_SENTINEL", str(raised.exception))

    def test_unrepresentable_mpv_config_paths_are_rejected(self):
        for path in ('/mpv"quoted', '/mpv\\escaped', '/mpv\nsecond-option'):
            with self.subTest(path=path), self.assertRaisesRegex(trial.Failure, "cannot be represented"):
                trial.trial_config(self.original, path)

    def test_optional_quirks_directory_is_saved_and_used_by_preflight(self):
        quirks = self.root / "private quirks"
        quirks.mkdir()
        self.args.libinput_quirks_dir = str(quirks)
        self.add_private_mpv()
        Path(self.args.seatd).write_bytes(b"fixture seatd.sock\0")
        output = self.prepare()
        plan = trial.load_plan(output)
        self.assertEqual(plan["libinput_quirks_dir"], str(quirks))
        markers = "-display-owner pi4-wayland-experimental -check-startup weston -u -n fullscreen dmabuf-wayland"
        with patch.object(trial, "capture", return_value=(0, markers)) as capture, \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            trial.preflight(plan)
        for call in capture.call_args_list:
            self.assertIn("LIBINPUT_QUIRKS_DIR=" + str(quirks), call.args[0])

    def test_invalid_quirks_directory_is_rejected_before_preparation(self):
        for path in (self.root / "missing", Path(self.args.seatd)):
            with self.subTest(path=path):
                self.args.libinput_quirks_dir = str(path)
                with self.assertRaisesRegex(trial.Failure, "libinput quirks path"):
                    self.prepare()
                self.assertFalse(Path(self.args.output).exists())

    def test_missing_or_symlinked_quirks_directory_is_rejected_at_load_and_preflight(self):
        quirks = self.root / "quirks"
        quirks.mkdir()
        self.args.libinput_quirks_dir = str(quirks)
        output = self.prepare()
        plan = trial.load_plan(output)
        saved = quirks.with_name("quirks-saved")
        quirks.rename(saved)
        for symlink in (False, True):
            with self.subTest(symlink=symlink):
                if symlink:
                    quirks.symlink_to(saved)
                with self.assertRaisesRegex(trial.Failure, "libinput quirks path"):
                    trial.load_plan(output)
                with patch.object(trial, "capture") as capture:
                    with self.assertRaisesRegex(trial.Failure, "libinput quirks path"):
                        trial.preflight(plan)
                capture.assert_not_called()

    def private_plugin_plan(self):
        candidate = self.root / "candidate.so"
        candidate.write_bytes(b"patched waylandsink fixture")
        self.args.wayland_plugin = str(candidate)
        installed = self.root / "installed-plugins"
        installed.mkdir()
        self.installed_wayland = installed / "libgstwaylandsink.so"
        self.installed_wayland.write_bytes(b"stock waylandsink fixture")
        (installed / "libgstvideotestsrc.so").write_bytes(b"other installed plugin")
        return self.startup_check_plan()

    def plugin_response(self, argv):
        markers = "-display-owner pi4-wayland-experimental -check-startup weston -u -n fullscreen dmabuf-wayland"
        if argv[-1] == "waylandsink":
            paths = [item.split("=", 1)[1] for item in argv if item.startswith("GST_PLUGIN_PATH_1_0=")]
            path = Path(paths[0]) / "libgstwaylandsink.so" if paths else self.installed_wayland
            return 0, f"Plugin Details:\n  Filename  {path}\n  Version  1.26.2\nfullscreen\n"
        if len(argv) >= 2 and argv[-2] == "/usr/bin/ldd":
            return 0, "libgstwayland-1.0.so.0 => /usr/lib/fixture/libgstwayland-1.0.so.0 (0x1234)\n"
        return 0, markers

    def test_private_plugin_snapshot_excludes_stock_duplicate_and_uses_fresh_registry(self):
        plan = self.private_plugin_plan()
        self.assertEqual(plan["files"]["wayland_plugin"]["sha256"], trial.digest(self.args.wayland_plugin))
        with patch.object(trial, "capture", side_effect=self.plugin_response), \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            selection = trial.stage_wayland_plugin(plan, self.root / "isolated-gst")
        env = selection["environment"]
        self.assertEqual(selection["candidate"].read_bytes(), Path(self.args.wayland_plugin).read_bytes())
        self.assertEqual(list(Path(env["GST_PLUGIN_PATH"]).iterdir()), [selection["candidate"]])
        filtered = Path(env["GST_PLUGIN_SYSTEM_PATH"])
        self.assertFalse((filtered / "libgstwaylandsink.so").exists())
        self.assertEqual((filtered / "libgstvideotestsrc.so").resolve(),
                         self.installed_wayland.parent / "libgstvideotestsrc.so")
        for key in ("GST_PLUGIN_PATH", "GST_PLUGIN_SYSTEM_PATH", "GST_REGISTRY"):
            self.assertEqual(env[key], env[key + "_1_0"])
        self.assertFalse(Path(env["GST_REGISTRY"]).exists())

    def test_private_plugin_preflight_scrubs_overrides_and_probes_fullscreen_without_display(self):
        plan = self.private_plugin_plan()
        inherited = {key: "PRIVATE_SENTINEL" for key in ("GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0",
                     "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0", "GST_REGISTRY", "GST_REGISTRY_1_0")}
        with patch.dict(os.environ, inherited), \
                patch.object(trial, "capture", side_effect=self.plugin_response) as capture, \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            trial.preflight(plan)
        commands = [call.args[0] for call in capture.call_args_list]
        self.assertNotIn("PRIVATE_SENTINEL", repr(commands))
        startup = next(argv for argv in commands if argv[-1] == "-check-startup")
        probe = next(argv for argv in commands if argv[-1] == trial.WAYLAND_NULL_PROBE)
        for argv in (startup, probe):
            self.assertTrue(any(item.startswith("GST_PLUGIN_PATH_1_0=") for item in argv))
            self.assertTrue(any(item.startswith("GST_REGISTRY_1_0=") for item in argv))
            self.assertFalse(any(item.startswith(("WAYLAND_DISPLAY=", "XDG_RUNTIME_DIR=")) for item in argv))
        self.assertIn("G_DEBUG=fatal-criticals", probe)
        self.assertNotIn("gst_element_set_state", trial.WAYLAND_NULL_PROBE)

    def test_private_plugin_rejects_missing_or_changed_binary(self):
        plan = self.private_plugin_plan()
        candidate = Path(self.args.wayland_plugin)
        original = candidate.read_bytes()
        candidate.write_bytes(b"changed")
        with self.assertRaisesRegex(trial.Failure, "pinned executable or module"):
            trial.load_plan(self.args.output)
        with self.assertRaisesRegex(trial.Failure, "pinned Wayland plugin changed"):
            trial.preflight(plan)
        candidate.write_bytes(original)
        candidate.unlink()
        with self.assertRaisesRegex(trial.Failure, "required pinned file"):
            trial.load_plan(self.args.output)

    def test_private_plugin_rejects_stale_registry_before_capability_commands(self):
        plan = self.private_plugin_plan()
        with patch.object(trial, "capture", side_effect=self.plugin_response), \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            selection = trial.stage_wayland_plugin(plan, self.root / "isolated-gst")
        Path(selection["environment"]["GST_REGISTRY"]).write_bytes(b"stale stock registry")
        with patch.object(trial, "capture") as capture:
            with self.assertRaisesRegex(trial.Failure, "registry must be fresh"):
                trial.preflight(plan, selection)
        capture.assert_not_called()

    def test_private_plugin_rejects_stock_selection_wrong_abi_or_fullscreen_failure(self):
        plan = self.private_plugin_plan()
        for failure, message in (("selection", "did not select"), ("abi", "installed GStreamer Wayland"),
                                 ("fullscreen", "fullscreen property check")):
            with self.subTest(failure=failure):
                def response(argv):
                    private = any(item.startswith("GST_PLUGIN_PATH_1_0=") for item in argv)
                    if failure == "selection" and private and argv[-1] == "waylandsink":
                        return 0, f"Filename {self.installed_wayland}\nVersion 1.26.2\nfullscreen\n"
                    if failure == "abi" and private and argv[-2] == "/usr/bin/ldd":
                        return 0, "libgstwayland-1.0.so.0 => /private/wrong/libgstwayland-1.0.so.0 (0x1234)\n"
                    if failure == "fullscreen" and argv[-1] == trial.WAYLAND_NULL_PROBE:
                        return -11, "PRIVATE_SENTINEL"
                    return self.plugin_response(argv)
                with patch.object(trial, "capture", side_effect=response), \
                        patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
                    with self.assertRaisesRegex(trial.Failure, message) as raised:
                        trial.preflight(plan)
                self.assertNotIn("PRIVATE_SENTINEL", str(raised.exception))


class FakeHost:
    def __init__(self, active=True, failure=None, interrupt=False):
        self.active = active
        self.failure = failure
        self.interrupt = interrupt
        self.events = []

    def event(self, name):
        self.events.append(name)
        if self.failure == name:
            raise trial.Failure("fixture " + name)

    def preflight(self):
        self.event("preflight")

    def service_active(self):
        self.event("service_active")
        return self.active

    def start_seatd(self):
        self.event("seatd")

    def stop_service(self):
        self.event("stop")

    def start_weston(self):
        self.event("weston")

    def start_receiver(self):
        self.event("receiver")

    def monitor(self, seconds):
        self.event("monitor")
        if self.interrupt:
            raise KeyboardInterrupt

    def cleanup(self):
        self.event("cleanup")
        if self.failure == "unreaped":
            raise trial.RecoveryRequired("fixture unreaped")

    def restore_service(self):
        self.event("restore")


class LifecycleTests(unittest.TestCase):
    def test_success_stops_only_after_preflight_and_restores_after_cleanup(self):
        host = FakeHost()
        trial.trial_lifecycle(host, 10)
        self.assertEqual(host.events, ["preflight", "service_active", "seatd", "stop", "weston",
                                       "receiver", "monitor", "cleanup", "restore"])

    def test_inactive_service_is_not_started_on_exit(self):
        host = FakeHost(active=False)
        trial.trial_lifecycle(host, 10)
        self.assertNotIn("stop", host.events)
        self.assertNotIn("restore", host.events)

    def test_failed_checks_and_seatd_never_stop_service(self):
        for phase in ("preflight", "service_active", "seatd"):
            with self.subTest(phase=phase):
                host = FakeHost(failure=phase)
                with self.assertRaises(trial.Failure):
                    trial.trial_lifecycle(host, 10)
                self.assertNotIn("stop", host.events)
                self.assertNotIn("restore", host.events)
                self.assertEqual(host.events[-1], "cleanup")

    def test_stop_or_launch_or_cleanup_failure_still_restores_service(self):
        for phase in ("stop", "weston", "receiver", "monitor", "cleanup"):
            with self.subTest(phase=phase):
                host = FakeHost(failure=phase)
                with self.assertRaises(trial.Failure):
                    trial.trial_lifecycle(host, 10)
                self.assertEqual(host.events[-2:], ["cleanup", "restore"])
                self.assertEqual(host.events.count("stop"), 1)

    def test_keyboard_interrupt_restores_service(self):
        host = FakeHost(interrupt=True)
        with self.assertRaises(KeyboardInterrupt):
            trial.trial_lifecycle(host, 10)
        self.assertEqual(host.events[-2:], ["cleanup", "restore"])

    def test_unreaped_trial_child_does_not_start_competing_receiver(self):
        host = FakeHost(failure="unreaped")
        with self.assertRaises(trial.RecoveryRequired):
            trial.trial_lifecycle(host, 10)
        self.assertEqual(host.events[-1], "cleanup")
        self.assertNotIn("restore", host.events)

    def test_duration_is_bounded_before_mutation(self):
        for seconds in (0, -1, 601):
            host = FakeHost()
            with self.assertRaises(trial.Failure):
                trial.trial_lifecycle(host, seconds)
            self.assertEqual(host.events, [])


class ProcessTests(unittest.TestCase):
    def test_user_launcher_uses_argv_without_a_shell_or_inherited_environment(self):
        plan = {"user": "testuser", "home": "/home/testuser"}
        with patch.object(trial.os, "geteuid", return_value=0), \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            argv = trial.user_command(plan, ["/path/has spaces/receiver", "-rc", "/path/$(literal);file"],
                                      {"XDG_RUNTIME_DIR": "/private/runtime"})
        self.assertEqual(argv[:6], ["/usr/bin/runuser", "-u", "testuser", "--", "/usr/bin/env", "-i"])
        self.assertEqual(argv[-3:], ["/path/has spaces/receiver", "-rc", "/path/$(literal);file"])
        self.assertNotIn("-c", argv)
        self.assertNotIn("--pty", argv)

    def test_capture_caps_output_and_timeout(self):
        # macOS's desktop sandbox denies killpg(..., 0). Exercise capture's
        # independent bounds here; Linux below exercises actual group cleanup.
        def reap(process):
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=3)
        with patch.object(trial, "terminate_group", side_effect=reap):
            with self.assertRaisesRegex(trial.Failure, "output limit"):
                trial.capture([sys.executable, "-c", "import sys;sys.stdout.write('x'*300000)"], timeout=2)
            with self.assertRaisesRegex(trial.Failure, "timed out"):
                trial.capture([sys.executable, "-c", "import time;time.sleep(30)"], timeout=0.1)

    @unittest.skipUnless(sys.platform == "linux", "Linux process-group semantics are required")
    def test_group_cleanup_does_not_kill_an_unrelated_process(self):
        owned = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(30)"], start_new_session=True)
        other = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(30)"], start_new_session=True)
        try:
            trial.terminate_group(owned)
            self.assertIsNotNone(owned.poll())
            self.assertIsNone(other.poll())
        finally:
            trial.terminate_group(other)

    def test_lock_prevents_overlapping_trials(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trial.lock"
            with trial.trial_lock(path):
                with self.assertRaisesRegex(trial.Failure, "already running"):
                    with trial.trial_lock(path):
                        self.fail("overlapping lock admitted")
            with trial.trial_lock(path):
                pass


class HostEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="display-evidence-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()
        self.directory.chmod(0o711)
        seatd = self.directory / "seatd"
        seatd.write_bytes(b"pinned seatd fixture")
        root = self.directory / "trial"
        root.mkdir(mode=0o700)
        receiver_uid = 65534 if os.geteuid() == 0 else os.getuid()
        receiver_gid = 65534 if os.geteuid() == 0 else os.getgid()
        plan = {"uid": receiver_uid, "gid": receiver_gid, "library_dirs": [],
                "files": {"seatd": {"path": str(seatd), "sha256": trial.digest(seatd)}}}
        with patch.object(trial.tempfile, "mkdtemp", return_value=str(root)):
            self.host = trial.Host(plan)
        self.addCleanup(self.close_logs)

    def close_logs(self):
        for stream in self.host.logs:
            stream.close()

    def launch(self):
        with patch.object(trial.subprocess, "Popen"):
            return self.host.launch("receiver", ["unused-fixture-command"])

    def test_live_logs_are_private_and_owned_through_open_descriptor(self):
        with patch.object(trial.os, "fchown", wraps=os.fchown) as chown:
            self.launch()
        stream = self.host.logs[0]
        chown.assert_called_once_with(stream.fileno(), self.host.plan["uid"], self.host.plan["gid"])
        stream.write(b"live diagnostic\n")
        log = self.host.evidence / "receiver.log"
        self.assertEqual(log.read_bytes(), b"live diagnostic\n")
        self.assertEqual(log.stat().st_mode & 0o777, 0o600)
        self.assertEqual(log.stat().st_uid, self.host.plan["uid"])
        for directory in (self.host.root, self.host.evidence):
            self.assertEqual(directory.stat().st_mode & 0o777, 0o711)
            self.assertEqual(directory.stat().st_uid, os.geteuid())

    def test_existing_log_symlink_is_not_opened_or_changed(self):
        target = self.directory / "unrelated"
        target.write_bytes(b"unchanged")
        (self.host.evidence / "receiver.log").symlink_to(target)
        with patch.object(trial.subprocess, "Popen") as launch:
            with self.assertRaises(FileExistsError):
                self.host.launch("receiver", ["unused-fixture-command"])
        launch.assert_not_called()
        self.assertEqual(target.read_bytes(), b"unchanged")
        self.assertEqual(self.host.logs, [])

    def test_failed_cleanup_retains_root_control_and_readable_logs(self):
        self.launch()
        with patch.object(trial, "terminate_group", side_effect=trial.RecoveryRequired("fixture")), \
                patch.object(trial.os, "chown", wraps=os.chown) as chown:
            with self.assertRaises(trial.RecoveryRequired):
                self.host.cleanup()
        chown.assert_not_called()
        self.assertTrue(self.host.logs[0].closed)
        self.assertTrue(self.host.seatd_snapshot.exists())
        for directory in (self.host.root, self.host.evidence):
            self.assertEqual(directory.stat().st_mode & 0o777, 0o711)
            self.assertEqual(directory.stat().st_uid, os.geteuid())

    def test_successful_cleanup_hands_over_only_private_directories(self):
        self.launch()
        ownership_changes = []
        actual_chown = os.chown

        def chown(path, uid, gid):
            ownership_changes.append(path)
            self.assertEqual(path.stat().st_mode & 0o777, 0o700)
            actual_chown(path, uid, gid)

        with patch.object(trial, "terminate_group"), patch.object(trial.os, "chown", side_effect=chown):
            self.host.cleanup()
        self.assertEqual(ownership_changes, [self.host.evidence, self.host.root])
        self.assertFalse(self.host.runtime.exists())
        self.assertFalse(self.host.seat.exists())
        self.assertFalse(self.host.seatd_snapshot.exists())
        self.assertTrue(self.host.logs[0].closed)
        for directory in (self.host.root, self.host.evidence):
            self.assertEqual(directory.stat().st_uid, self.host.plan["uid"])

    def test_quirks_environment_reaches_user_processes_but_never_root_seatd(self):
        root = self.directory / "quirks-trial"
        root.mkdir(mode=0o700)
        plan = dict(self.host.plan, libinput_quirks_dir=str(self.directory), user="receiver",
                    home=str(self.directory), output="HDMI-A-1", config_path="/unused/receiver.conf")
        plan["files"] = dict(plan["files"], **{key: {"path": "/unused/" + key}
                                             for key in ("weston", "shell", "receiver")})
        with patch.object(trial.tempfile, "mkdtemp", return_value=str(root)):
            host = trial.Host(plan)
        try:
            with patch.object(trial.subprocess, "Popen") as launch, \
                    patch.object(host, "wait_socket"), \
                    patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
                host.start_seatd()
                host.start_weston()
                host.start_receiver()
            seatd, weston, receiver = launch.call_args_list
            self.assertEqual(seatd.kwargs["env"], trial.SAFE_ENV)
            self.assertFalse(any("LIBINPUT_QUIRKS_DIR=" in item for item in seatd.args[0]))
            for call in (weston, receiver):
                self.assertIn("LIBINPUT_QUIRKS_DIR=" + str(self.directory), call.args[0])
        finally:
            for stream in host.logs:
                stream.close()

    def test_host_reuses_preflight_plugin_selection_for_receiver_only(self):
        environment = {"GST_PLUGIN_PATH": "/private/plugins", "GST_PLUGIN_PATH_1_0": "/private/plugins",
                       "GST_REGISTRY": "/private/registry", "GST_REGISTRY_1_0": "/private/registry"}
        self.host.plugin_selection = {"environment": environment}
        self.host.environment.update(environment)
        self.host.files["receiver"] = "/unused/receiver"
        self.host.plan.update(user="receiver", home=str(self.directory), config_path="/unused/config")
        with patch.object(trial, "preflight") as preflight:
            self.host.preflight()
        preflight.assert_called_once_with(self.host.plan, self.host.plugin_selection)
        with patch.object(trial.subprocess, "Popen") as launch, patch.object(self.host, "wait_socket"), \
                patch.object(trial, "system_tool", side_effect=lambda name: "/usr/bin/" + name):
            self.host.start_seatd()
            self.host.start_receiver()
        seatd, receiver = launch.call_args_list
        self.assertEqual(seatd.kwargs["env"], trial.SAFE_ENV)
        self.assertFalse(any(item.startswith("GST_") for item in seatd.args[0]))
        for key, value in environment.items():
            self.assertIn(key + "=" + value, receiver.args[0])

    @unittest.skipUnless(sys.platform == "linux" and os.geteuid() == 0,
                         "Linux root is required to verify GStreamer discovery as another user")
    def test_root_staged_plugins_are_discoverable_by_receiver_user(self):
        inspect = shutil.which("gst-inspect-1.0", path=trial.SAFE_ENV["PATH"])
        if not inspect:
            self.skipTest("GStreamer inspection tool is unavailable")
        code, details = trial.capture([inspect, "waylandsink"],
                                      env=dict(trial.SAFE_ENV, **trial.registry_environment(self.directory / "lookup.bin")))
        self.assertEqual(code, 0)
        installed = Path(trial.plugin_detail(details, "Filename"))
        account = pwd.getpwuid(self.host.plan["uid"])
        plan = dict(self.host.plan, user=account.pw_name, home=account.pw_dir)
        plan["files"] = {"wayland_plugin": {"path": str(installed), "sha256": trial.digest(installed)}}
        selection = trial.stage_wayland_plugin(plan, self.directory / "real-gstreamer")
        for key in ("GST_PLUGIN_PATH", "GST_PLUGIN_SYSTEM_PATH"):
            path = Path(selection["environment"][key])
            self.assertEqual(path.stat().st_uid, 0)
            self.assertEqual(path.stat().st_mode & 0o777, 0o755)
        code, details = trial.capture(trial.user_command(plan, [inspect, "waylandsink"], selection["environment"]))
        self.assertEqual(code, 0)
        self.assertEqual(trial.plugin_detail(details, "Filename"), str(selection["candidate"]))
        code, details = trial.capture(trial.user_command(plan, [inspect, "videotestsrc"], selection["environment"]))
        self.assertEqual(code, 0)
        self.assertIn("Video test source", details)

    @unittest.skipUnless(sys.platform == "linux" and os.geteuid() == 0,
                         "Linux root is required to verify access from separate user identities")
    def test_live_log_access_and_name_protection_across_users(self):
        self.launch()
        self.host.logs[0].write(b"live diagnostic\n")
        log = self.host.evidence / "receiver.log"

        def as_user(uid, operation):
            return subprocess.run([sys.executable, "-c", operation, str(log)], user=uid, group=uid,
                                  extra_groups=[], cwd="/", capture_output=True, timeout=5)

        read = "import pathlib,sys;sys.stdout.buffer.write(pathlib.Path(sys.argv[1]).read_bytes())"
        result = as_user(self.host.plan["uid"], read)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"live diagnostic\n")
        self.assertNotEqual(as_user(65533, read).returncode, 0)
        for operation in ("import os,sys;os.unlink(sys.argv[1])",
                          "import os,sys;os.rename(sys.argv[1],sys.argv[1]+'.moved')",
                          "import os,sys;os.rename(os.path.dirname(sys.argv[1]),sys.argv[1]+'.moved')"):
            self.assertNotEqual(as_user(self.host.plan["uid"], operation).returncode, 0)
        self.assertEqual(log.read_bytes(), b"live diagnostic\n")


if __name__ == "__main__":
    unittest.main()
