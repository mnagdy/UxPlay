"""Deployment safeguards, using local fixtures and mocked SSH only."""

import importlib.machinery
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/pi-dev"
loader = importlib.machinery.SourceFileLoader("pi_dev", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
dev = importlib.util.module_from_spec(spec)
loader.exec_module(dev)


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pi dev test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo with spaces"
        self.root.mkdir()
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        (self.root / ".gitignore").write_text(".pi-dev.json\n.pi-dev/\nbuild/\n")
        (self.root / "main.c").write_text("old code\n")
        (self.root / "deleted.c").write_text("remove me\n")
        subprocess.run(["git", "-C", str(self.root), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=Test",
                        "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture"], check=True)
        self.addCleanup(patch.stopall)
        patch.object(dev, "ROOT", self.root).start()

    def make_snapshot(self, name):
        target = Path(self.temp.name) / name
        target.mkdir()
        return target, dev.snapshot(target, "test-release")

    def test_snapshot_includes_local_edits_and_new_files_but_not_deleted_or_private_files(self):
        (self.root / "main.c").write_text("edited code\n")
        (self.root / "new file.c").write_text("new code\n")
        (self.root / "deleted.c").unlink()
        (self.root / ".pi-dev.json").write_text('{"host":"private-host"}')
        (self.root / "build").mkdir()
        (self.root / "build/secret").write_text("do not sync")
        target, metadata = self.make_snapshot("snapshot")
        self.assertEqual((target / "main.c").read_text(), "edited code\n")
        self.assertTrue((target / "new file.c").exists())
        for name in ["deleted.c", ".pi-dev.json", "build", ".git"]:
            self.assertFalse((target / name).exists())
        self.assertTrue(metadata["dirty"])
        self.assertEqual(json.loads((target / ".pi-build.json").read_text()), metadata)

    def test_snapshot_fingerprint_changes_without_a_commit_and_is_frozen(self):
        first, before = self.make_snapshot("first")
        (self.root / "main.c").write_text("different code\n")
        _, after = self.make_snapshot("second")
        self.assertEqual(before["revision"], after["revision"])
        self.assertNotEqual(before["source_sha256"], after["source_sha256"])
        self.assertEqual((first / "main.c").read_text(), "old code\n")

    def test_external_symlink_is_rejected(self):
        outside = Path(self.temp.name) / "private"
        outside.write_text("not source")
        (self.root / "link.c").symlink_to(outside)
        with self.assertRaises(ValueError):
            self.make_snapshot("snapshot")

    def test_build_failure_never_activates_and_releases_lock(self):
        calls = []

        class FakePi:
            def remote(self, action, *args, **kwargs):
                calls.append(action)
                if action == "build":
                    raise subprocess.CalledProcessError(1, "build")

            def sync(self, source):
                calls.append("sync")

        with self.assertRaises(subprocess.CalledProcessError):
            dev.run_workflow(FakePi(), "deploy")
        self.assertEqual(calls, ["prepare", "sync", "build", "finish"])

    def test_mpv_build_is_explicit_and_does_not_activate(self):
        calls = []

        class FakePi:
            def remote(self, action, *args, **kwargs):
                calls.append(action)

            def sync(self, source):
                calls.append("sync")

        dev.run_workflow(FakePi(), "build", with_mpv=True)
        self.assertEqual(calls, ["prepare", "sync", "build-mpv", "finish"])

    def test_failed_lock_acquisition_does_not_release_another_lock(self):
        calls = []

        class LockedPi:
            def remote(self, action, *args, **kwargs):
                calls.append(action)
                raise subprocess.CalledProcessError(1, "prepare")

        with self.assertRaises(subprocess.CalledProcessError):
            dev.run_workflow(LockedPi(), "deploy")
        self.assertEqual(calls, ["prepare"])


class HostTests(unittest.TestCase):
    def test_valid_hosts(self):
        for value in ["projector-dev", "user@projector.local", "user@192.168.1.124"]:
            self.assertEqual(dev.validate_host(value), value)

    def test_shell_and_ssh_option_injection_rejected(self):
        for value in ["-oProxyCommand=bad", "user@host;bad", "host$(bad)", "host`bad`", "host path", ""]:
            with self.assertRaises(ValueError):
                dev.validate_host(value)


if __name__ == "__main__":
    unittest.main()
