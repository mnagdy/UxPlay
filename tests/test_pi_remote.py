"""Pi helper recovery and validation tests; no systemd or sudo is executed."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/pi_remote.py"
spec = importlib.util.spec_from_file_location("pi_remote", SCRIPT)
remote = importlib.util.module_from_spec(spec)
spec.loader.exec_module(remote)


class RemoteTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="uxplay-remote-test-")
        self.addCleanup(temp.cleanup)
        # macOS /var and /tmp are aliases; fixtures should use their real paths.
        self.temp = Path(temp.name).resolve()
        root = self.temp / "uxplay-dev"
        values = {
            "ROOT": root, "MARKER": root / ".managed-by-uxplay-pi-dev",
            "STATE": root / "state", "LOCK": root / "state/lock",
            "SOURCE": root / "src", "BUILD": root / "build/Release",
            "RELEASES": root / "releases", "DROPIN": self.temp / "service/90-uxplay-dev.conf",
        }
        for name, value in values.items():
            mock = patch.object(remote, name, value)
            mock.start()
            self.addCleanup(mock.stop)

    def test_existing_unmanaged_directory_is_never_adopted(self):
        remote.ROOT.mkdir()
        existing = remote.ROOT / "important"
        existing.write_text("preserve")
        with self.assertRaises(remote.Failure):
            remote.prepare("ours")
        self.assertEqual(existing.read_text(), "preserve")
        self.assertFalse(remote.MARKER.exists())

    def test_only_owner_can_release_lock(self):
        remote.prepare("owner")
        with self.assertRaises(remote.Failure):
            remote.finish("different")
        self.assertTrue(remote.LOCK.exists())
        remote.finish("owner")
        self.assertFalse(remote.LOCK.exists())

    def test_source_symlink_prevents_sync_preparation(self):
        remote.prepare("owner")
        remote.finish("owner")
        remote.SOURCE.rmdir()
        other = self.temp / "other"
        other.mkdir()
        remote.SOURCE.symlink_to(other)
        with self.assertRaises(remote.Failure):
            remote.prepare("new")

    def test_live_operation_prevents_lock_release_after_disconnect(self):
        remote.prepare("owner")
        remote.write_json(remote.LOCK / "operation.json", {"pid": remote.os.getpid()})
        with self.assertRaisesRegex(remote.Failure, "still running"):
            remote.finish("owner")
        self.assertTrue(remote.LOCK.exists())
        (remote.LOCK / "operation.json").unlink()
        remote.finish("owner")

    def test_dead_operation_record_can_be_recovered_by_its_owner(self):
        remote.prepare("owner")
        remote.write_json(remote.LOCK / "operation.json", {"pid": 12345})
        with patch.object(remote, "process_alive", return_value=False):
            remote.finish("owner")
        self.assertFalse(remote.LOCK.exists())

    def test_failed_health_restores_exact_previous_override(self):
        remote.prepare("owner")
        previous = {"active": "old", "previous": None}
        remote.write_json(remote.STATE / "deployment.json", previous)
        with patch.object(remote, "put_override") as put, \
             patch.object(remote, "restart") as restart, \
             patch.object(remote, "healthy", side_effect=[remote.Failure("crash"), None]):
            with self.assertRaises(remote.Failure):
                remote.switch("new text", ["new"], "exact old text", ["old"], {"active": "new"})
        self.assertEqual([call.args for call in put.call_args_list], [("new text",), ("exact old text",)])
        self.assertEqual(restart.call_count, 2)
        self.assertEqual(json.loads((remote.STATE / "deployment.json").read_text()), previous)

    def test_first_deploy_failure_restores_absence_of_override(self):
        remote.prepare("owner")
        with patch.object(remote, "put_override") as put, \
             patch.object(remote, "restart"), \
             patch.object(remote, "healthy", side_effect=[remote.Failure("bad"), None]):
            with self.assertRaises(remote.Failure):
                remote.switch("new text", ["new"], None, ["baseline"], {"active": "new"})
        self.assertEqual(put.call_args_list[-1].args, (None,))

    def test_failed_rollback_is_reported(self):
        remote.prepare("owner")
        with patch.object(remote, "put_override"), patch.object(remote, "restart"), \
             patch.object(remote, "healthy", side_effect=[remote.Failure("bad"), remote.Failure("also bad")]):
            with self.assertRaisesRegex(remote.Failure, "ROLLBACK ALSO FAILED"):
                remote.switch("new", ["new"], "old", ["old"], {})

    def test_unrecognized_override_is_preserved(self):
        remote.prepare("owner")
        remote.DROPIN.parent.mkdir()
        remote.DROPIN.write_text("custom settings")
        with self.assertRaises(remote.Failure):
            remote.existing_override()
        self.assertEqual(remote.DROPIN.read_text(), "custom settings")

    def test_custom_service_arguments_are_rejected(self):
        username = remote.pwd.getpwuid(remote.os.getuid()).pw_name
        text = ("LoadState=loaded\nUser=" + username + "\nExecStart={ path=/usr/local/bin/uxplay ; "
                "argv[]=/usr/local/bin/uxplay -rc /home/test/config -d ; ignore_errors=no ; pid=1 ; }\n")
        result = subprocess.CompletedProcess([], 0, stdout=text, stderr="")
        with patch.object(remote, "run", return_value=result):
            with self.assertRaisesRegex(remote.Failure, "custom arguments"):
                remote.service()

    def test_restart_loop_fails_health_check_even_when_service_active(self):
        base = {"ActiveState": "active", "SubState": "running", "command": ["/expected"], "NRestarts": "0"}
        states = [dict(base, MainPID="100"), dict(base, MainPID="101", NRestarts="1")]
        with patch.object(remote, "service", side_effect=states), \
             patch.object(remote.os, "readlink", return_value="/expected"), \
             patch.object(remote.time, "sleep"):
            with self.assertRaisesRegex(remote.Failure, "restarted"):
                remote.healthy(["/expected"])

    def test_mpv_build_selection_is_recorded_and_default_clears_cached_option(self):
        remote.prepare("owner")
        (remote.BUILD / "uxplay").write_text("test executable")
        for enabled in (True, False):
            release_id = "mpv-on" if enabled else "mpv-off"
            remote.write_json(remote.SOURCE / ".pi-build.json", {
                "release_id": release_id, "revision": "abc123", "dirty": True})
            with patch.object(remote, "run") as run:
                remote.build("owner", release_id, enable_mpv=enabled)
            configure = run.call_args_list[0].args[0]
            self.assertIn("-DUXPLAY_ENABLE_MPV=" + ("ON" if enabled else "OFF"), configure)
            metadata = remote.read_json(remote.RELEASES / release_id / "build.json")
            self.assertEqual(metadata["mpv_backend"], enabled)
            self.assertTrue((remote.RELEASES / release_id / "source.tar.gz").is_file())
        self.assertFalse(remote.DROPIN.exists())

    def test_privileged_commands_keep_the_authentication_terminal(self):
        with patch.object(remote, "run") as run, patch.object(remote.sys.stdin, "isatty", return_value=True):
            remote.authenticate()
            remote.sudo("systemctl", "restart", remote.SERVICE)
        self.assertTrue(all(call.kwargs["interactive"] for call in run.call_args_list))
        self.assertEqual(run.call_args_list[0].args[0], ["sudo", "-v"])
        self.assertEqual(run.call_args_list[1].args[0][:2], ["sudo", "-n"])


if __name__ == "__main__":
    unittest.main()
