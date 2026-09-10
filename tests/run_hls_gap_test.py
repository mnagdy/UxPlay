#!/usr/bin/env python3
"""Verify direct HLS playback when a declared AAC track has missing fragments.

Generate legal test video/tone with FFmpeg, or pass --fixtures with a previously
generated directory to run on a device without FFmpeg. The HTTP manifest grows
from three fragments so the player starts at the beginning of a live stream;
it never receives ENDLIST. Real audio samples must resume at original PTS.
Replacement cases cover pending downloads and terminal HTTP failures.
Buffering comparisons explicitly select --pi4 or --pi4-direct; an executable
wrapper may set decoder environment variables but must not prepend a profile.
"""

import argparse
import functools
import http.server
from pathlib import Path
import re
import select
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlsplit

from hls_gap_fixture import (SCENARIOS, SEGMENT_COUNT, SEGMENT_SECONDS,
                             STARTUP_SCENARIOS, STARTUP_SEGMENT_COUNT, STARTUP_SEGMENT_SECONDS,
                             BUFFERING_SCENARIOS, BUFFERING_SEGMENT_COUNT, BUFFERING_SEGMENT_SECONDS,
                             boxes, fragment_tracks, initialization_tracks,
                             make_fixture, playlist, validate_fixture)


REPLACEMENT_CASES = ("replace-absent", "replace-stalled", "replace-error")


class FixtureServer(http.server.ThreadingHTTPServer):
    # A deliberately slow response must not make fixture cleanup wait for its
    # worker after a failed assertion, process timeout, or cancellation.
    daemon_threads = True
    block_on_close = False

    def __init__(self, address, handler):
        super().__init__(address, handler)
        self.fixture_lock = threading.Lock()
        self.fixture_started = {}
        self.fixture_stopping = threading.Event()
        self.stall_started = threading.Event()
        self.stall_cancelled = threading.Event()
        self.stall_cancel_ms = -1
        self.failure_requested = threading.Event()


class LiveFixtureHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/startup-single-multimoof/fragment-000.m4s":
            self.delayed_final_audio()
            return
        if path == "/late-delayed/fragment-003.m4s":
            self.delayed_audio_fragment()
            return
        if path == "/stalled/stream.m3u8":
            self.stalled_manifest()
            return
        if path == "/failed/stream.m3u8":
            # A definite HTTP failure exercises the real source/bus error path.
            self.server.failure_requested.set()
            self.send_response(404)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        scenario = path.strip("/").split("/")[0]
        if scenario in (*SCENARIOS, *STARTUP_SCENARIOS, *BUFFERING_SCENARIOS) and path == f"/{scenario}/stream.m3u8":
            with self.server.fixture_lock:
                started = self.server.fixture_started.setdefault(scenario, time.monotonic())
            if scenario in BUFFERING_SCENARIOS:
                visible = min(BUFFERING_SEGMENT_COUNT, 1 + int(
                    (time.monotonic() - started) / BUFFERING_SEGMENT_SECONDS))
                body = playlist(visible, BUFFERING_SEGMENT_SECONDS).encode("utf-8")
            elif scenario in STARTUP_SCENARIOS:
                # Two ten-second bodies are immediately available; waiting for
                # the third to infer the first audio gap would add ten seconds.
                initial = 1 if scenario.startswith("startup-single-") else 2
                visible = min(STARTUP_SEGMENT_COUNT, initial + int(
                    (time.monotonic() - started) / STARTUP_SEGMENT_SECONDS))
                body = playlist(visible, STARTUP_SEGMENT_SECONDS).encode("utf-8")
            else:
                visible = min(SEGMENT_COUNT, 3 + int((time.monotonic() - started) / SEGMENT_SECONDS))
                body = playlist(visible).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.apple.mpegurl")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()


    def delayed_final_audio(self):
        location = Path(self.directory) / "startup-single-multimoof"
        data = (location / "fragment-000.m4s").read_bytes()
        tracks = initialization_tracks((location / "init.mp4").read_bytes())
        audio_id = next(key for key, value in tracks.items() if value["handler"] == "soun")
        moof = [box for box in boxes(data) if box.kind == b"moof"][-1]
        audio = fragment_tracks(data[moof.start:])[audio_id]
        audio_start = moof.start + min(start for start, _ in audio["spans"])
        self.send_response(200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data[:audio_start])
            self.wfile.flush()
            with self.server.fixture_lock:
                started = self.server.fixture_started["startup-single-multimoof"]
            if not self.server.fixture_stopping.wait(max(0, started + 4 - time.monotonic())):
                self.wfile.write(data[audio_start:])
        except (BrokenPipeError, ConnectionResetError):
            pass


    def delayed_audio_fragment(self):
        # The first actual AAC fragment is withheld until video has had time
        # to start using the proven missing-audio intervals. This exercises
        # restoration of normal audio buffering after playback is underway.
        source = self.send_head()
        if source is None:
            return
        try:
            with self.server.fixture_lock:
                started = self.server.fixture_started["late-delayed"]
            delay = max(0, started + 4 - time.monotonic())
            if not self.server.fixture_stopping.wait(delay):
                self.copyfile(source, self.wfile)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            source.close()


    def stalled_manifest(self):
        body = playlist(3).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.apple.mpegurl")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.flush()
        started = time.monotonic()
        self.server.stall_started.set()
        # The sender accepted the connection and announced a body, then stalls.
        # Observe socket EOF directly so success requires cancelling real I/O.
        while time.monotonic() - started < 8:
            if self.server.fixture_stopping.wait(0.05):
                return
            try:
                readable, _, _ = select.select([self.connection], [], [], 0)
                if readable and not self.connection.recv(1, socket.MSG_PEEK):
                    self.server.stall_cancel_ms = int((time.monotonic() - started) * 1000)
                    self.server.stall_cancelled.set()
                    return
            except OSError:
                self.server.stall_cancel_ms = int((time.monotonic() - started) * 1000)
                self.server.stall_cancelled.set()
                return
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def observation(output):
    matches = re.findall(r"^TEST observation (.+)$", output, re.MULTILINE)
    if len(matches) != 1:
        raise AssertionError(f"Expected one playback observation, got {len(matches)}")
    return {key: int(value) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", matches[0])}


def timing(output, name):
    matches = re.findall(rf"^TEST {name}=(\d+)$", output, re.MULTILINE)
    if len(matches) != 1:
        raise AssertionError(f"Expected one {name} result, got {len(matches)}")
    return int(matches[0])


def check_replacement(scenario, result, server):
    observed = observation(result.stdout)
    start_ms = timing(result.stdout, "start_call_ms")
    replacement_ms = timing(result.stdout, "replacement_call_ms")
    if start_ms >= 500 or replacement_ms >= 1500:
        raise AssertionError(f"{scenario}: blocked control calls: start={start_ms}ms replace={replacement_ms}ms")
    # The original channels have no real audio, so audio here proves the new
    # channel started; timestamps and video counts also require actual progress.
    if (observed["video_buffers"] < 10 or observed["audio_buffers"] < 10
            or observed["last_video_ms"] < 1500 or observed["last_audio_ms"] < 1500):
        raise AssertionError(f"{scenario}: replacement did not produce picture and sound: {observed}")
    if scenario == "replace-stalled":
        if not server.stall_started.is_set():
            raise AssertionError("Slow response was never requested; cancellation was not exercised")
        if not server.stall_cancelled.wait(1) or server.stall_cancel_ms >= 2500:
            raise AssertionError(f"Old slow response was not promptly cancelled: {server.stall_cancel_ms}ms")
    if scenario == "replace-error":
        if not server.failure_requested.is_set():
            raise AssertionError("Failing manifest was never requested; HTTP error recovery was not exercised")
        failure_ms = timing(result.stdout, "terminal_failure_ms")
        if failure_ms >= 10000:
            raise AssertionError(f"HTTP error did not terminate the failed stream promptly: {failure_ms}ms")
        expected = ("state=NULL pending=VOID_PENDING failed=1 ready=0 likely=0 empty=1 full=0 "
                    "rate=0 duration=0 position=0 seek_start=0 seek_duration=0")
        snapshots = re.findall(r"^TEST terminal_failure (.+)$", result.stdout, re.MULTILINE)
        if snapshots != [expected]:
            raise AssertionError(f"Failed stream did not provide a stopped, empty playback-info snapshot: {snapshots}")
        if "stage=failed action=stop" not in result.stdout:
            raise AssertionError("HTTP error did not enter the renderer's terminal failure path")
        readiness = re.findall(r"^TEST replacement_readiness (.+)$", result.stdout, re.MULTILINE)
        if readiness != ["ready=1 likely=1 failed=0 rate=1"]:
            raise AssertionError(f"Replacement did not restore the phone's playing status: {readiness}")
        observed["terminal_failure_ms"] = failure_ms
    return {"start_call_ms": start_ms, "replacement_call_ms": replacement_ms, **observed}


def check_observation(scenario, observed):
    if observed["video_buffers"] < 50 or observed["last_video_ms"] < 10000:
        raise AssertionError(f"{scenario}: video did not keep advancing: {observed}")
    if scenario == "absent":
        if observed["audio_buffers"] != 0:
            raise AssertionError(f"{scenario}: receiver invented real audio samples: {observed}")
        return
    if observed["audio_buffers"] < 20 or observed["last_audio_ms"] < 10000:
        raise AssertionError(f"{scenario}: real audio did not arrive/recover: {observed}")
    if scenario in ("late", "late-delayed"):
        expected_first = 6000 if scenario == "late-delayed" else 4000
        if not expected_first - 200 <= observed["first_audio_ms"] <= expected_first + 300:
            raise AssertionError(f"{scenario}: late audio timestamps changed: {observed}")
    elif not 0 <= observed["first_audio_ms"] <= 300:
        raise AssertionError(f"{scenario}: initial real audio was lost: {observed}")
    if scenario == "middle":
        if not 3800 <= observed["max_audio_gap_ms"] <= 4400:
            raise AssertionError(f"{scenario}: audio gap/recovery was not preserved: {observed}")
    elif observed["max_audio_gap_ms"] > 100:
        raise AssertionError(f"{scenario}: receiver dropped present audio: {observed}")


def check_startup(scenario, observed, output):
    if scenario == "startup-single-multimoof":
        if observed["first_video_wall_ms"] < 3500 or "stage=gap-repair" in output:
            raise AssertionError(f"{scenario}: incomplete HTTP body was prematurely classified as empty audio: {observed}")
        if (observed["video_buffers"] < 80 or observed["last_video_ms"] < 9500
                or observed["audio_buffers"] < 50 or observed["last_audio_ms"] < 9500
                or not 7800 <= observed["first_audio_ms"] <= 8400
                or observed["max_audio_gap_ms"] > 100):
            raise AssertionError(f"{scenario}: delayed AAC inside the same fragment was not preserved: {observed}")
        return
    if not 0 <= observed["first_video_wall_ms"] < 3000:
        raise AssertionError(f"{scenario}: receiver waited for a later HTTP fragment: {observed}")
    if observed["video_buffers"] < 10 or observed["last_video_ms"] < 1500:
        raise AssertionError(f"{scenario}: first video frame did not lead to continued playback: {observed}")
    repaired = "stage=gap-repair" in output
    if scenario.endswith("absent"):
        if observed["audio_buffers"] != 0 or not repaired:
            raise AssertionError(f"{scenario}: missing-audio startup was not repaired correctly: {observed}")
    elif (observed["audio_buffers"] < 10 or observed["last_audio_ms"] < 1500
          or not 0 <= observed["first_audio_ms"] <= 300
          or observed["max_audio_gap_ms"] > 100 or repaired):
        raise AssertionError(f"{scenario}: present AAC audio was lost or unnecessarily repaired: {observed}")


def check_buffering_profile(scenario, observed, output):
    first = observed["first_video_wall_ms"]
    if scenario == "buffering-default" and first < 5500:
        raise AssertionError(f"{scenario}: cached/default buffering behavior unexpectedly changed: {observed}")
    if scenario == "buffering-direct" and not 0 <= first < 3000:
        raise AssertionError(f"{scenario}: direct HTTP profile waited for the second six-second fragment: {observed}")
    if (observed["video_buffers"] < 10 or observed["last_video_ms"] < 1500
            or observed["audio_buffers"] < 10 or observed["last_audio_ms"] < 1500
            or not 0 <= observed["first_audio_ms"] <= 300 or observed["max_audio_gap_ms"] > 100
            or "stage=gap-repair" in output):
        raise AssertionError(f"{scenario}: real video/audio changed while applying the buffering profile: {observed}")


def run_cases(binary, directory, cases):
    handler = functools.partial(LiveFixtureHandler, directory=str(directory))
    with FixtureServer(("127.0.0.1", 0), handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            base = f"http://127.0.0.1:{server.server_port}"
            for scenario in cases:
                with server.fixture_lock:
                    server.fixture_started.clear()
                server.failure_requested.clear()
                if scenario in REPLACEMENT_CASES:
                    source = {"replace-absent": "absent", "replace-stalled": "stalled",
                              "replace-error": "failed"}[scenario]
                    mode = "--replace-after-error" if scenario == "replace-error" else "--replace"
                    arguments = [binary, mode, f"{base}/{source}/stream.m3u8", f"{base}/normal/stream.m3u8"]
                elif scenario in BUFFERING_SCENARIOS:
                    profile = "--pi4-direct" if scenario == "buffering-direct" else "--pi4"
                    arguments = [binary, profile, "--observe", f"{base}/{scenario}/stream.m3u8", "2"]
                else:
                    seconds = "2" if scenario in STARTUP_SCENARIOS else "12"
                    if scenario == "startup-single-multimoof":
                        seconds = "9.7"
                    arguments = [binary, "--observe", f"{base}/{scenario}/stream.m3u8", seconds]
                result = subprocess.run(arguments,
                                        capture_output=True, text=True, timeout=45)
                if result.returncode:
                    raise AssertionError(f"{scenario}: playback failed:\n{result.stdout}\n{result.stderr}")
                if scenario in REPLACEMENT_CASES:
                    try:
                        observed = check_replacement(scenario, result, server)
                    except AssertionError as error:
                        raise AssertionError(f"{error}\n{result.stdout}\n{result.stderr}") from error
                    description = ("terminal HTTP failure, empty phone status, and picture/sound on replacement passed"
                                   if scenario == "replace-error"
                                   else "prompt cancellation and picture/sound on replacement passed")
                    print(f"{scenario}: {description}: {observed}")
                    continue
                observed = observation(result.stdout)
                try:
                    if scenario in BUFFERING_SCENARIOS:
                        check_buffering_profile(scenario, observed, result.stdout)
                        print(f"{scenario}: buffering profile and real AAC/video preserved: {observed}")
                        continue
                    if scenario in STARTUP_SCENARIOS:
                        check_startup(scenario, observed, result.stdout)
                        description = ("delayed same-fragment AAC preserved" if scenario == "startup-single-multimoof"
                                       else "first frame under three seconds with correct audio")
                        print(f"{scenario}: {description}: {observed}")
                        continue
                    check_observation(scenario, observed)
                    repaired = "stage=gap-repair" in result.stdout
                    if repaired != (scenario != "normal"):
                        raise AssertionError(f"{scenario}: unexpected missing-audio repair status {repaired}")
                    if scenario == "late-delayed":
                        transitions = re.findall(r"stage=gap-only-audio active=([01])", result.stdout)
                        if "1" not in transitions or "0" not in transitions[transitions.index("1") + 1:]:
                            raise AssertionError("Late audio did not restore normal buffering after video-only playback")
                except AssertionError as error:
                    raise AssertionError(f"{error}\n{result.stdout}\n{result.stderr}") from error
                print(f"{scenario}: video continues and original audio samples/timing are preserved: {observed}")
        finally:
            server.fixture_stopping.set()
            server.shutdown()
            thread.join(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", help="Compiled test_direct_renderer_hls")
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"))
    parser.add_argument("--generate-only", type=Path, help="Write and validate fixtures without playback")
    parser.add_argument("--fixtures", type=Path, help="Use previously generated synthetic fixtures")
    all_cases = [*SCENARIOS, *REPLACEMENT_CASES, *STARTUP_SCENARIOS, *BUFFERING_SCENARIOS]
    parser.add_argument("--case", action="append", choices=all_cases, dest="cases")
    args = parser.parse_args()
    if args.generate_only:
        if not args.ffmpeg:
            parser.error("--ffmpeg is required to generate synthetic fixtures")
        summary = make_fixture(args.generate_only.resolve(), args.ffmpeg)
        print(f"Synthetic fixtures validated: {summary}")
        return
    if not args.binary:
        parser.error("a test executable is required unless --generate-only is used")
    binary = str(Path(args.binary).resolve(strict=True))
    if args.fixtures:
        directory = args.fixtures.resolve(strict=True)
        validate_fixture(directory)
        run_cases(binary, directory, args.cases or all_cases)
    else:
        if not args.ffmpeg:
            parser.error("ffmpeg is required; use --fixtures to reuse generated media")
        with tempfile.TemporaryDirectory(prefix="uxplay-hls-gap-") as temporary:
            directory = Path(temporary)
            make_fixture(directory, args.ffmpeg)
            run_cases(binary, directory, args.cases or all_cases)


if __name__ == "__main__":
    main()
