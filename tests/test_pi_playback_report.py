"""Privacy, correlation and bounds for the read-only Pi report exporter."""
import importlib.machinery
import importlib.util
import json
import os
import fcntl
from pathlib import Path
import subprocess
import sys
import unittest
import tempfile
from types import SimpleNamespace
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/pi-playback-report"
loader = importlib.machinery.SourceFileLoader("pi_playback_report", str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
report = importlib.util.module_from_spec(spec)
loader.exec_module(report)


def entry(message, boot="a" * 32, pid="17", stamp=100):
    return {"MESSAGE": message, "_BOOT_ID": boot, "_PID": pid,
            "__REALTIME_TIMESTAMP": str(stamp), "__MONOTONIC_TIMESTAMP": str(stamp),
            "_SYSTEMD_INVOCATION_ID": "b" * 32,
            "_CMDLINE": "PRIVATE_SENTINEL", "EXTRA_HEADER": "PRIVATE_SENTINEL"}


class DiagnosticTests(unittest.TestCase):
    def test_typed_known_record(self):
        value, error = report.parse_journal_entry(entry(
            "MPV playback: session=10 state=playing codec=hevc size=1280x720 position_known=1 position=0.000 "
            "cache_bytes=32 decoder_drops=-1 file_error_code=-13 audio_rate=48000"))
        self.assertIsNone(error)
        self.assertEqual(value["fields"]["size"], {"width": 1280, "height": 720})
        self.assertEqual(value["fields"]["position"], 0.0)
        self.assertEqual(value["fields"]["file_error_code"], -13)
        self.assertEqual(value["pid"], 17)
        self.assertEqual(value["monotonic_us"], 100)
        self.assertNotIn("PRIVATE_SENTINEL", json.dumps(value))

    def test_known_prefix_does_not_bypass_privacy(self):
        payloads = [
            "session=1 state=playing url=https://PRIVATE_SENTINEL.invalid/?token=PRIVATE_SENTINEL",
            "session=1 state=https://PRIVATE_SENTINEL.invalid",
            "session=1 state=playing codec=PRIVATE_SENTINEL",
            "session=1 state=playing Authorization=PRIVATE_SENTINEL",
            "session=1 state=playing Cookie=PRIVATE_SENTINEL",
            "session=1 state=playing position=1\nAuthorization: PRIVATE_SENTINEL",
            "session=1 state=playing\rPRIVATE_SENTINEL",
            "session=1 state=playing session=2",
            "session=1 state=playing source=/PRIVATE_SENTINEL",
            "session=1 state=playing position=NaN",
            "session=1 state=playing position=inf",
            "session=1 state=playing paused=2",
            "session=1 state=playing size=99999x720",
            "session=18446744073709551616 state=playing",
            "session=1 state=playing codec=\ud800",
            "session=1 state=playing schema=99",
            "session=1 state=playing http_status_known=1 http_status=0",
        ]
        for payload in payloads:
            with self.subTest(payload=payload):
                value, reason = report.parse_journal_entry(entry("MPV playback: " + payload))
                self.assertIsNone(value)
                self.assertIsNotNone(reason)
                self.assertNotIn("PRIVATE_SENTINEL", reason)

    def test_other_prefixes_are_typed(self):
        for text in ["MPV control: session=2 action=seek accepted=1 position=4.5",
                     "Playback session: session=2 event=accepted route=direct-http backend=mpv",
                     "*** ERROR: MPV playback: session=2 request=rejected"]:
            self.assertIsNotNone(report.parse_diagnostic(text)[0])
        self.assertIsNone(report.parse_diagnostic("MPV control: session=2 action=PRIVATE_SENTINEL")[0])
        self.assertIsNone(report.parse_diagnostic("Playback session: session=2 event=accepted source=PRIVATE_SENTINEL")[0])

    def test_current_cpp_trace_fields_all_have_schemas(self):
        # A formatter addition must not silently disable every exported record.
        source = (SCRIPT.parents[1] / "uxplay.cpp").read_text()
        function = source.split("static void format_mpv_trace(", 1)[1].split("static void trace_mpv_playback", 1)[0]
        import re
        keys = set(re.findall(r'\b([a-z][a-z0-9_]*)=(?:%|2\b)', function))
        self.assertTrue(keys)
        self.assertFalse(keys - report.PLAYBACK.keys(), "Trace fields missing from exporter schema")
        text = ("MPV playback: session=2 state=playing schema=2 monotonic_ms=100 "
                "actual_paused_known=1 actual_paused=0 cache_eof_known=1 cache_eof=1 "
                "selected_video_known=1 selected_video_id=1 video_decoder=hevc "
                "audio_decoder=aac pixel_format=yuv420p hwdec=no vo=gpu ao=alsa "
                "diagnostic_reason=http-client-error http_status_known=1 http_status=403 "
                "video_observation_age_ms=-1 video_reader_pts_known=1 video_reader_pts=0.0")
        self.assertIsNotNone(report.parse_diagnostic(text)[0])
        self.assertIsNotNone(report.parse_diagnostic("MPV control: session=2 action=playlist-pause accepted=1 value=0.0 monotonic_ms=100")[0])
        self.assertIsNotNone(report.parse_diagnostic("Playback session: session=2 event=receiver-rebuilt elapsed_ms=2 monotonic_ms=100")[0])

    def test_packet_capture_fields_survive_typed_export(self):
        expected = {
            "session": 11, "state": "playing", "schema": 2,
            "packet_capture_enabled": 1, "packet_capture_active": 0, "packet_capture_complete": 1,
            "packet_capture_started_ms": 1000, "packet_capture_ended_ms": 16000, "packet_capture_errors": 0, "packet_log_rejected": 2,
            "audio_packets": 0, "video_packets": 375, "audio_packet_bytes": 0, "video_packet_bytes": 1234567,
            "audio_packets_with_pts": 0, "video_packets_with_pts": 375,
            "audio_packet_first_at_ms": 0, "audio_packet_last_at_ms": 0,
            "video_packet_first_at_ms": 1040, "video_packet_last_at_ms": 15960,
            "audio_packet_first_pts": 0.0, "audio_packet_last_pts": 0.0,
            "video_packet_first_pts": -0.04, "video_packet_last_pts": 14.96,
            "last_warning_stage": "demux", "last_warning_reason": "unknown", "last_warning_at_ms": 15500,
            "packet_corrupt_warnings": 1, "pes_mismatch_warnings": 2, "demux_read_warnings": 3,
        }
        message = "MPV playback: " + " ".join(f"{key}={value}" for key, value in expected.items())
        raw = json.dumps(entry(message)).encode()
        records, counts = report.collect_records(raw, 0, 200, limit=10)
        self.assertEqual(counts["omitted_by_reason"], {})
        encoded = report.encode_report({"schema": report.SCHEMA, "journal": counts, "records": records}, 8000)
        result = json.loads(encoded)
        self.assertEqual(result["records"][0]["fields"], expected)
        self.assertNotIn("PRIVATE_SENTINEL", encoded.decode())

    def test_fixed_packet_warning_reasons_and_stages_are_accepted(self):
        reasons = ("pes-size-mismatch", "packet-corrupt", "demux-read-error", "hls-expired-segments", "hls-sequence-change",
                   "virtual-terminal-unavailable", "frame-present-failure", "drm-display-failure")
        for reason in reasons:
            with self.subTest(reason=reason):
                value, error = report.parse_diagnostic(
                    f"MPV playback: session=11 state=playing diagnostic_reason={reason} last_warning_reason={reason}")
                self.assertIsNone(error)
                self.assertEqual(value["fields"]["last_warning_reason"], reason)
                self.assertEqual(value["fields"]["diagnostic_reason"], reason)
        for stage in ("video-decode", "audio-decode", "video-output", "audio-output", "demux", "source", "unclassified", "unknown"):
            with self.subTest(stage=stage):
                self.assertIsNotNone(report.parse_diagnostic(
                    f"MPV playback: session=11 state=playing last_warning_stage={stage}")[0])

    def test_packet_capture_fields_reject_untyped_or_nonfinite_values(self):
        invalid = {
            "packet_capture_enabled": "2", "packet_capture_active": "-1", "packet_capture_complete": "true",
            "packet_capture_started_ms": "-1", "packet_capture_ended_ms": "NaN", "packet_capture_errors": "-1", "packet_log_rejected": "-1",
            "audio_packets": "18446744073709551616", "video_packets": "-1", "audio_packet_bytes": "1.5",
            "video_packet_bytes": "inf", "audio_packets_with_pts": "-1", "video_packets_with_pts": "1e3",
            "audio_packet_first_at_ms": "-1", "audio_packet_last_at_ms": "NaN",
            "video_packet_first_at_ms": "-1", "video_packet_last_at_ms": "1.1",
            "audio_packet_first_pts": "NaN", "audio_packet_last_pts": "inf",
            "video_packet_first_pts": "-inf", "video_packet_last_pts": "1e999",
            "last_warning_stage": "https://PRIVATE_SENTINEL", "last_warning_reason": "PRIVATE_SENTINEL",
            "last_warning_at_ms": "-1", "packet_corrupt_warnings": "-1", "pes_mismatch_warnings": "NaN",
            "demux_read_warnings": "18446744073709551616", "packet_text": "PRIVATE_SENTINEL",
        }
        for key, value in invalid.items():
            with self.subTest(key=key):
                record, reason = report.parse_diagnostic(f"MPV playback: session=11 state=playing {key}={value}")
                self.assertIsNone(record)
                self.assertIsNotNone(reason)
                self.assertNotIn("PRIVATE_SENTINEL", reason)
        self.assertIsNone(report.parse_diagnostic(
            "MPV playback: session=11 state=playing last_warning_reason=unknown\nPRIVATE_SENTINEL")[0])

    def test_unknown_missing_metadata_and_oversized_are_omitted(self):
        self.assertEqual(report.parse_diagnostic("Raw private https://PRIVATE_SENTINEL")[1], "unrecognized")
        self.assertEqual(report.parse_diagnostic("A" * (report.LINE_LIMIT + 1))[1], "oversized")
        row = entry("MPV playback: session=1 state=playing")
        row["_BOOT_ID"] = "PRIVATE_SENTINEL"
        self.assertEqual(report.parse_journal_entry(row)[1], "invalid_correlation")
        row = entry("MPV playback: session=1 state=playing")
        del row["__MONOTONIC_TIMESTAMP"]
        self.assertEqual(report.parse_journal_entry(row)[1], "invalid_correlation")


class LimitTests(unittest.TestCase):
    def test_window_record_limit_and_omissions(self):
        rows = [entry("MPV playback: session=1 state=playing", stamp=n) for n in [105, 104, 103, 102, 1]]
        rows += [entry("Unrecognized PRIVATE_SENTINEL"), entry("MPV playback: session=1 state=PRIVATE_SENTINEL")]
        raw = b"\n".join(json.dumps(x).encode() for x in rows) + b"\ninvalid-private-JSON"
        records, counts = report.collect_records(raw, 100, 110, limit=2)
        self.assertEqual([x["realtime_us"] for x in records], [104, 105])
        self.assertEqual(counts["omitted_by_reason"], {"record_limit": 2, "outside_window": 1, "unrecognized": 1, "invalid_known_record": 1, "invalid_json": 1})
        self.assertNotIn("PRIVATE_SENTINEL", json.dumps([records, counts]))

    def test_same_generation_different_pid_or_boot_stays_separate(self):
        rows = [entry("MPV playback: session=10 state=playing", pid="17"),
                entry("MPV playback: session=10 state=failed", pid="18"),
                entry("MPV playback: session=10 state=paused", boot="c" * 32, pid="17")]
        records = [report.parse_journal_entry(x)[0] for x in rows]
        self.assertEqual(len(report.summarize_attempts(records)), 3)

    def test_last_state_uses_monotonic_time_after_wall_clock_adjustment(self):
        early = entry("MPV playback: session=1 state=playing", stamp=200)
        early["__MONOTONIC_TIMESTAMP"] = "100"
        late = entry("MPV playback: session=1 state=failed", stamp=100)
        late["__MONOTONIC_TIMESTAMP"] = "200"
        records = [report.parse_journal_entry(x)[0] for x in [late, early]]
        self.assertEqual(report.summarize_attempts(records)[0]["last_observed_state"], "failed")

    def test_terminal_report_survives_later_idle_and_record_limit(self):
        rows = [entry("MPV playback: session=10 state=idle terminal_report=0 child_generation=10", stamp=200),
                entry("MPV playback: session=10 state=playing terminal_report=0", stamp=180),
                entry("MPV playback: session=10 state=failed terminal_report=1 child_generation=10 child_pid=42 error_code=5 end_reason=error", stamp=160)]
        records, counts = report.collect_records(b"\n".join(json.dumps(x).encode() for x in rows), 0, 300, limit=2)
        attempt = report.summarize_attempts(records)[0]
        self.assertEqual(attempt["last_observed_state"], "idle")
        self.assertEqual(attempt["final_state"], "failed")
        self.assertEqual(attempt["final_child_pid"], 42)
        self.assertEqual(counts["omitted_by_reason"]["record_limit"], 1)
        different = report.parse_journal_entry(entry("MPV playback: session=11 state=starting child_generation=10 terminal_report=0"))[0]
        self.assertEqual(different["fields"]["child_generation"], 10)
        self.assertEqual(len(report.summarize_attempts(records + [different])), 2)

    def test_byte_limit_prioritizes_terminal_evidence(self):
        records = [report.parse_journal_entry(entry("MPV playback: session=1 state=failed terminal_report=1", stamp=1))[0]]
        records += [report.parse_journal_entry(entry("MPV playback: session=1 state=idle terminal_report=0", stamp=n))[0] for n in range(2, 100)]
        encoded = report.encode_report({"schema": report.SCHEMA, "journal": {}, "records": records}, 4000)
        value = json.loads(encoded)
        self.assertTrue(any(x["fields"].get("terminal_report") for x in value["records"]))
        self.assertEqual(value["attempts"][0]["final_state"], "failed")
        self.assertEqual(value["journal"]["export_terminal_records_omitted"], 0)

    def test_export_byte_limit_preserves_newest_and_counts_drops(self):
        records = [report.parse_journal_entry(entry("MPV playback: session=1 state=playing", stamp=n))[0] for n in range(100)]
        obj = {"schema": report.SCHEMA, "journal": {}, "records": records}
        encoded = report.encode_report(obj, 4000)
        self.assertLessEqual(len(encoded), 4000)
        value = json.loads(encoded)
        self.assertEqual(value["records"][-1]["realtime_us"], 99)
        self.assertGreater(value["journal"]["export_records_omitted"], 0)
        self.assertEqual(len(value["records"]) + value["journal"]["export_records_omitted"], 100)

    def test_raw_entry_and_line_limit(self):
        raw = b"\n".join(json.dumps(entry("MPV playback: session=1 state=playing")).encode() for _ in range(4))
        with patch.object(report, "RAW_ENTRY_LIMIT", 2):
            values, counts = report.collect_records(raw, 0, 1000)
        self.assertEqual(len(values), 2)
        self.assertTrue(counts["entry_limit_reached"])
        values, counts = report.collect_records(b"A" * (report.LINE_LIMIT + 1), 0, 1000)
        self.assertFalse(values)
        self.assertEqual(counts["omitted_by_reason"]["oversized_journal_entry"], 1)


class ExecutionTests(unittest.TestCase):
    def test_no_host_shell_or_option_injection(self):
        for host in [None, "", "-oProxyCommand=bad", "user@host;bad", "host$(bad)", "host`bad`", "host path"]:
            with self.assertRaises(ValueError):
                report.validate_host(host)
        self.assertEqual(report.validate_host("mo@192.168.1.124"), "mo@192.168.1.124")

    def test_child_output_and_runtime_are_bounded(self):
        status, data, _ = report.capture([sys.executable, "-c", "print('x'*10000)"], byte_limit=128)
        self.assertEqual(status, "output-limit")
        self.assertEqual(len(data), 128)
        status, _, _ = report.capture([sys.executable, "-c", "import time; time.sleep(5)"], timeout=.1)
        self.assertEqual(status, "timeout")

    def test_child_stderr_is_never_exported(self):
        status, data, _ = report.capture([sys.executable, "-c", "import sys; print('PRIVATE_SENTINEL',file=sys.stderr); print('safe')"])
        self.assertEqual(status, "ok")
        self.assertEqual(data, b"safe\n")

    def test_missing_host_does_not_run_ssh(self):
        with patch.object(report, "capture") as capture:
            self.assertEqual(report.main([]), 1)
        capture.assert_not_called()


class RetainedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=Path(tempfile.gettempdir()).resolve())
        self.directory = Path(self.temp.name) / ".local/state/uxplay"
        # Match the writer's 0700 mkdirat traversal. mkdir(parents=True)
        # gives intermediates the ambient umask, which is 0002 on the Pi.
        # Those group-writable fixtures correctly fail the privacy boundary.
        for component in (self.directory.parent.parent, self.directory.parent, self.directory):
            component.mkdir(mode=0o700)
        self.uid = os.getuid()
        self.account = patch.object(report.pwd, "getpwuid", return_value=SimpleNamespace(pw_dir=self.temp.name))
        self.account.start()
        self.write_file(".playback-trace.lock", b"")

    def tearDown(self):
        self.account.stop()
        self.temp.cleanup()

    def write_file(self, name, data):
        path = self.directory / name
        path.write_bytes(data)
        path.chmod(0o600)
        return path

    def collect(self, limit=report.DEFAULT_RECORD_LIMIT):
        return report.retained_history({"uid": self.uid}, limit)

    def test_older_boot_terminal_history_ignores_journal_window(self):
        old = entry("MPV playback: session=1 state=failed terminal_report=1 failure_stage=video-output history_write_failures=2", boot="c" * 32, stamp=1)
        new = entry("MPV playback: session=2 state=playing", stamp=5000)
        self.write_file("playback-trace.previous.jsonl", json.dumps(old).encode() + b"\n")
        self.write_file("playback-trace.jsonl", json.dumps(new).encode() + b"\n")
        rows, stats = self.collect()
        self.assertEqual(len(rows), 2)
        self.assertEqual(stats["oldest_retained_realtime_us"], 1)
        self.assertEqual(stats["retained_boot_count"], 2)
        self.assertEqual(rows[0]["fields"]["history_write_failures"], 2)
        self.assertEqual(len(report.summarize_attempts(rows)), 2)

    def test_retained_privacy_and_torn_line(self):
        good = entry("MPV playback: session=1 state=failed terminal_report=1")
        bad = entry("MPV playback: session=1 state=playing url=https://PRIVATE_SENTINEL")
        self.write_file("playback-trace.jsonl", json.dumps(good).encode() + b"\n" + json.dumps(bad).encode() + b"\n" + json.dumps(good).encode())
        rows, stats = self.collect()
        self.assertEqual(len(rows), 1)
        self.assertEqual(stats["incomplete_lines_omitted"], 1)
        self.assertEqual(stats["omitted_by_reason"]["invalid_known_record"], 1)
        self.assertNotIn("PRIVATE_SENTINEL", json.dumps([rows, stats]))

    def test_duplicate_event_keeps_journal_correlation_and_sources(self):
        text = "MPV playback: session=1 state=failed terminal_report=1 monotonic_ms=50"
        journal = report.parse_journal_entry(entry(text, stamp=50100))[0]
        retained = report.parse_journal_entry(entry(text, stamp=50050))[0]
        rows, stats = report.merge_records([journal], [retained], 10)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["sources"], ["journal", "retained"])
        self.assertEqual(stats["duplicate_records"], 1)
        self.assertIn("invocation_id", rows[0])

    def test_merge_record_bound_keeps_old_terminal(self):
        old = report.parse_journal_entry(entry("MPV playback: session=1 state=failed terminal_report=1", stamp=1))[0]
        recent = [report.parse_journal_entry(entry("MPV playback: session=2 state=playing", stamp=n))[0] for n in range(10, 15)]
        rows, stats = report.merge_records(recent, [old], 2)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["fields"]["state"], "failed")
        self.assertEqual(rows[-1]["realtime_us"], 14)
        self.assertEqual(stats["record_limit_omitted"], 4)

    def test_symlink_hardlink_mode_and_size_are_rejected(self):
        target = self.write_file("private-target", b"PRIVATE_SENTINEL")
        current = self.directory / "playback-trace.jsonl"
        current.symlink_to(target)
        self.assertFalse(self.collect()[0])
        current.unlink()
        os.link(target, current)
        self.assertFalse(self.collect()[0])
        current.unlink()
        self.write_file(current.name, b"PRIVATE_SENTINEL").chmod(0o644)
        self.assertFalse(self.collect()[0])
        self.write_file(current.name, b"PRIVATE_SENTINEL")
        with patch.object(report, "RETAINED_FILE_LIMIT", 4):
            rows, stats = self.collect()
        self.assertFalse(rows)
        self.assertNotIn("PRIVATE_SENTINEL", json.dumps(stats))
        current.unlink()
        os.mkfifo(current, 0o600)
        self.assertFalse(self.collect()[0])
        self.directory.chmod(0o755)
        self.assertFalse(self.collect()[0])
        self.directory.chmod(0o700)

    def test_symlink_directory_and_busy_lock_are_rejected(self):
        with (self.directory / ".playback-trace.lock").open("rb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertEqual(self.collect()[1]["query_status"], "busy")
        alternate = self.directory.with_name("alternate")
        self.directory.rename(alternate)
        self.directory.symlink_to(alternate, target_is_directory=True)
        self.assertEqual(self.collect()[1]["query_status"], "unsafe-or-unavailable")

    def test_unknown_service_user_does_not_search_ssh_user_home(self):
        with patch.object(report.pwd, "getpwuid") as lookup:
            self.assertEqual(report.retained_history({})[1]["query_status"], "unavailable")
        lookup.assert_not_called()


if __name__ == "__main__":
    unittest.main()
