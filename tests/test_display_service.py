"""Lifecycle/rollback tests for the durable display service, without HDMI."""
from contextlib import contextmanager
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/pi-display-service"
loader = importlib.machinery.SourceFileLoader("display_service", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
service = importlib.util.module_from_spec(spec)
loader.exec_module(service)


class StopMonitor(Exception):
    pass


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.addCleanup(patch.stopall)
        patch.object(service, "RUNTIME", self.root).start()

    def test_receiver_restart_reaps_player_group_and_preserves_compositor(self):
        owner, broker, dead, new = [Mock(pid=p) for p in (101, 102, 103, 104)]
        owner.poll.return_value = broker.poll.return_value = new.poll.return_value = None
        dead.poll.return_value = 1
        host = object.__new__(service.ServiceHost)
        host.processes, host.receiver = [broker, owner, dead], dead
        actions = []
        def start():
            actions.append("start")
            host.receiver = new
            host.processes.append(new)
        host.start_receiver = start
        def health(path, data):
            self.assertEqual(data["weston_pid"], 101)
            self.assertEqual(data["receiver_pid"], 104)
            raise StopMonitor
        with patch.object(service.trial, "terminate_group", side_effect=lambda p: actions.append(("reap", p.pid))), \
                patch.object(service.time, "sleep"), patch.object(service, "atomic_json", side_effect=health):
            with self.assertRaises(StopMonitor):
                host.monitor_service(self.root)
        self.assertEqual(actions, [("reap", 103), "start"])
        self.assertEqual(host.processes, [broker, owner, new])

    def test_unreaped_receiver_never_starts_replacement(self):
        host = object.__new__(service.ServiceHost)
        owner = Mock(); owner.poll.return_value = None
        dead = Mock(); dead.poll.return_value = 1
        host.processes, host.receiver = [owner, owner, dead], dead
        host.start_receiver = Mock()
        with patch.object(service.trial, "terminate_group", side_effect=service.trial.RecoveryRequired("still alive")):
            with self.assertRaises(service.trial.RecoveryRequired):
                host.monitor_service(self.root)
        host.start_receiver.assert_not_called()

    def test_compositor_exit_escalates_to_systemd(self):
        host = object.__new__(service.ServiceHost)
        dead = Mock(); dead.poll.return_value = 1
        host.processes, host.receiver = [dead, dead], Mock()
        with self.assertRaisesRegex(service.Failure, "Display owner"):
            host.monitor_service(self.root)

    def test_receiver_crash_loop_is_bounded(self):
        host = object.__new__(service.ServiceHost)
        live = Mock(pid=1); live.poll.return_value = None
        dead = Mock(pid=2); dead.poll.return_value = 1
        host.processes, host.receiver = [live, live, dead], dead
        def start(): host.processes.append(dead)
        host.start_receiver = Mock(side_effect=start)
        with patch.object(service.trial, "terminate_group"), patch.object(service.time, "sleep"), \
                patch.object(service.time, "monotonic", return_value=10), patch.object(service, "atomic_json"):
            with self.assertRaisesRegex(service.Failure, "repeatedly"):
                host.monitor_service(self.root)
        self.assertEqual(host.start_receiver.call_count, 3)

    @unittest.skipUnless(sys.platform == "linux", "Pi/Linux process-group semantics")
    def test_real_receiver_process_group_including_player_is_terminated(self):
        path = self.root / "player.pid"
        child = "import time; time.sleep(60)"
        parent = """import subprocess,sys,time,signal
from pathlib import Path
p=subprocess.Popen([sys.executable,'-c',sys.argv[2]])
def stop(*args):
    p.wait(timeout=2)
    raise SystemExit(0)
signal.signal(signal.SIGTERM, stop)
Path(sys.argv[1]).write_text(str(p.pid))
time.sleep(60)
"""
        process = subprocess.Popen([sys.executable, "-c", parent, str(path), child], start_new_session=True)
        import time
        try:
            for _ in range(100):
                if path.exists(): break
                time.sleep(0.01)
            self.assertTrue(path.exists())
            service.trial.terminate_group(process)
            self.assertIsNotNone(process.poll())
            with self.assertRaises(ProcessLookupError):
                os.killpg(process.pid, 0)
        finally:
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            process.wait()


class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.events = []
        self.locked = False
        self.addCleanup(patch.stopall)
        self.dropin = self.root / "95-piplay-display.conf"
        patch.object(service, "DROPIN", self.dropin).start()
        @contextmanager
        def lock(*args):
            self.locked = True
            try: yield
            finally: self.locked = False
        patch.object(service.trial, "trial_lock", side_effect=lock).start()
        def put(text): self.events.append("override"); self.dropin.write_text(text)
        patch.object(service, "put_override", side_effect=put).start()
        def ctl(*args):
            if args[0] == "start": self.assertFalse(self.locked)
            self.events.append(args[0])
        patch.object(service, "systemctl", side_effect=ctl).start()
        patch.object(service, "stop_service", side_effect=lambda: self.events.append("stop")).start()
        patch.object(service, "verify_baseline_files", side_effect=lambda _: self.events.append("verify-baseline")).start()
        patch.object(service, "remove_override", side_effect=lambda _: self.events.append("remove")).start()
        patch.object(service, "baseline_health", side_effect=lambda _: self.events.append("baseline-healthy")).start()

    def test_success_releases_display_lock_before_starting_supervisor(self):
        with patch.object(service, "display_health", side_effect=lambda _: self.events.append("healthy")):
            service.activation_transaction(Path('/opt/piplay/test'), {})
        self.assertEqual(self.events, ["override", "daemon-reload", "stop", "reset-failed", "start", "healthy"])

    def test_failed_health_restores_only_after_children_stop(self):
        with patch.object(service, "display_health", side_effect=service.Failure("startup")):
            with self.assertRaisesRegex(service.Failure, "startup"):
                service.activation_transaction(Path('/opt/piplay/test'), {})
        self.assertEqual(self.events[-7:], ["stop", "verify-baseline", "remove", "daemon-reload", "reset-failed", "start", "baseline-healthy"])

    def test_unconfirmed_stop_does_not_remove_override_or_start_baseline(self):
        count = 0
        def stop():
            nonlocal count
            count += 1
            if count == 2: raise service.trial.RecoveryRequired("not reaped")
        with patch.object(service, "stop_service", side_effect=stop), \
                patch.object(service, "display_health", side_effect=service.Failure("startup")):
            with self.assertRaises(service.trial.RecoveryRequired):
                service.activation_transaction(Path('/opt/piplay/test'), {})
        self.assertNotIn("remove", self.events)
        self.assertNotIn("baseline-healthy", self.events)

    def test_failed_override_write_still_recovers_original_service(self):
        with patch.object(service, "put_override", side_effect=service.Failure("write")):
            with self.assertRaisesRegex(service.Failure, "write"):
                service.activation_transaction(Path('/opt/piplay/test'), {})
        self.assertEqual(self.events[-3:], ["reset-failed", "start", "baseline-healthy"])

    def test_changed_baseline_blocks_removal(self):
        with patch.object(service, "display_health", side_effect=service.Failure("startup")), \
                patch.object(service, "verify_baseline_files", side_effect=service.Failure("changed")):
            with self.assertRaisesRegex(service.Failure, "changed"):
                service.activation_transaction(Path('/opt/piplay/test'), {})
        self.assertNotIn("remove", self.events)

    def test_existing_override_is_never_stopped_or_overwritten(self):
        self.dropin.write_text("another deployment")
        with self.assertRaisesRegex(service.Failure, "appeared"):
            service.activation_transaction(Path('/opt/piplay/test'), {})
        self.assertEqual(self.events, [])
        self.assertEqual(self.dropin.read_text(), "another deployment")


class WifiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.addCleanup(patch.stopall)
        patch.object(service, "RUNTIME", self.root).start()
        patch.object(service, "require_root_tree").start()

    def test_receipt_precedes_change_and_stop_restores_original(self):
        for original in ("on", "off"):
            with self.subTest(original=original):
                def change(interface, state):
                    self.assertEqual(json.loads((self.root / 'wifi.json').read_text())["original"], original)
                with patch.object(service, "wifi_state", return_value=original), patch.object(service, "set_wifi", side_effect=change) as setter:
                    service.begin_wifi("wlan0")
                    service.restore_wifi()
                self.assertEqual([c.args for c in setter.call_args_list], [("wlan0", "off"), ("wlan0", original)])
                self.assertFalse((self.root / 'wifi.json').exists())

    def test_failed_change_retains_receipt_for_stop_hook(self):
        with patch.object(service, "wifi_state", return_value="on"), patch.object(service, "set_wifi", side_effect=service.Failure("iw failed")):
            with self.assertRaises(service.Failure): service.begin_wifi("wlan0")
            self.assertTrue((self.root / 'wifi.json').exists())
            with self.assertRaises(service.Failure): service.restore_wifi()
            self.assertTrue((self.root / 'wifi.json').exists())

    def test_stale_receipt_prevents_overwriting_original(self):
        (self.root / 'wifi.json').write_text('{"original":"on"}')
        with patch.object(service, "set_wifi") as setter:
            with self.assertRaisesRegex(service.Failure, "pending"): service.begin_wifi("wlan0")
        setter.assert_not_called()

    def test_invalid_receipt_cannot_invoke_network_command(self):
        (self.root / 'wifi.json').write_text('{"original":"on","interface":"wlan0; bad"}')
        with patch.object(service, "set_wifi") as setter:
            with self.assertRaises(service.Failure): service.restore_wifi()
        setter.assert_not_called()


class PolicyTests(unittest.TestCase):
    def test_service_kills_entire_cgroup_and_always_runs_restore_hook(self):
        text = service.unit_text('/opt/piplay/display/releases/example')
        self.assertIn('KillMode=mixed\n', text)
        self.assertIn(' restore-wifi\n', text)
        self.assertIn('StandardOutput=journal\n', text)
        self.assertIn('/usr/bin/env -i ', text)
        self.assertIn(' /usr/bin/python3 -I ', text)
        self.assertIn('StartLimitBurst=3\n', text)
        self.assertIn('Restart=always\n', text)
        self.assertIn('CacheDirectory=piplay-display\n', text)
        self.assertNotIn('LD_LIBRARY_PATH', text)

    @unittest.skipUnless(hasattr(os, 'ST_NOEXEC'), 'Linux executable mount flags')
    def test_noexec_workspace_is_rejected_before_activation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            with patch.object(service.os, 'statvfs', return_value=Mock(f_flag=os.ST_NOEXEC)) as inspect:
                with self.assertRaisesRegex(service.Failure, 'executable filesystem'):
                    service.executable_filesystem(root / 'future-service-directory')
            inspect.assert_called_once_with(root)
            with patch.object(service.os, 'statvfs', return_value=Mock(f_flag=0)):
                service.executable_filesystem(root)

    def test_private_code_is_staged_on_executable_cache_not_runtime_mount(self):
        with patch.object(service.trial.Host, '__init__', return_value=None) as init:
            service.ServiceHost({'fixture':True})
        init.assert_called_once_with({'fixture':True}, root_parent=str(service.WORK))
        self.assertNotEqual(service.WORK, service.RUNTIME)

    def test_rejects_unit_path_expansion(self):
        for path in ('/tmp/bad path', '/tmp/%h', '/tmp/$HOME', '/tmp/line\nbreak'):
            with self.assertRaises(service.Failure): service.unit_text(path)

    def test_rollback_does_not_validate_candidate_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            (root / '95.conf').write_text(service.unit_text(root))
            from contextlib import nullcontext
            with patch.object(service, 'DROPIN', root / '95.conf'), \
                    patch.object(service, 'read_bundle', return_value=({'baseline':{}}, None)) as read, \
                    patch.object(service, 'verify_baseline_files'), patch.object(service, 'stop_service'), \
                    patch.object(service.trial, 'trial_lock', side_effect=lambda *a: nullcontext()), \
                    patch.object(service, 'remove_override'), patch.object(service, 'systemctl'), patch.object(service, 'baseline_health'):
                service.rollback(root)
            read.assert_called_once_with(root, installed=True, load_runtime=False)

    def test_root_trust_rejects_writable_files_and_symlink_parents(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            path = root / "script"
            path.write_text("fixture")
            path.chmod(0o666)
            with self.assertRaises(service.Failure): service.require_root_tree(path)
            path.chmod(0o644)
            link = root / "link"
            link.symlink_to(path)
            with self.assertRaises(service.Failure): service.require_root_tree(link)

    def test_installed_rollback_can_read_sealed_metadata_without_external_binaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            target = root / '20260913T000000Z-123456789abc'
            target.mkdir()
            for name in service.PAYLOAD: (target / name).write_text('fixture')
            metadata = {'schema':service.SCHEMA, 'installation':str(target),
                        'baseline':{'user':'fixture'}, 'wifi_interface':None,
                        'files':{name:service.trial.digest(target / name) for name in service.PAYLOAD}}
            (target / 'service.json').write_text(json.dumps(metadata))
            with patch.object(service, 'STORE', root), patch.object(service, 'require_root_tree'), \
                    patch.object(service.trial, 'load_plan', side_effect=AssertionError('candidate must not be loaded')):
                actual, plan = service.read_bundle(target, installed=True, load_runtime=False)
            self.assertEqual(actual, metadata)
            self.assertIsNone(plan)


if __name__ == '__main__':
    unittest.main()
