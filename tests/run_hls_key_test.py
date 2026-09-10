#!/usr/bin/env python3
"""Real AES-128 HLS key validation over HTTP, using synthetic video and tone.

Generate fixtures on a machine with FFmpeg and OpenSSL, then pass --fixtures
on a device without those generators. --gst-plugin-path selects an isolated
GStreamer plugin candidate only for these headless software-decoding processes.
No installed receiver, global plugin configuration, or service is modified.
"""

import argparse
from collections import Counter
import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading
from urllib.parse import urlsplit


# Public deterministic test material, never used outside these generated files.
TEST_KEY = bytes(range(16))
CASES = ("valid", "empty", "not-found", "short", "long", "redirect")
SEGMENT_SECONDS = 2
SEGMENT_COUNT = 8


def generate_fixture(directory, ffmpeg, openssl):
    directory = Path(directory)
    media = directory / "media"
    media.mkdir(parents=True, exist_ok=True)
    summary = {"kind": "synthetic-aes128-key-validation", "segment_seconds": SEGMENT_SECONDS,
               "segments": []}
    with tempfile.TemporaryDirectory(prefix="uxplay-key-plaintext-") as temporary:
        plain = Path(temporary)
        subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
             "testsrc2=size=160x90:rate=10", "-f", "lavfi", "-i",
             "sine=frequency=440:sample_rate=48000", "-t", str(SEGMENT_SECONDS * SEGMENT_COUNT),
             "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-preset", "ultrafast",
             "-pix_fmt", "yuv420p", "-g", "20", "-keyint_min", "20", "-sc_threshold", "0",
             "-bf", "0", "-c:a", "aac", "-b:a", "48k", "-ac", "2", "-fflags", "+bitexact",
             "-flags:v", "+bitexact", "-flags:a", "+bitexact", "-map_metadata", "-1",
             "-f", "hls", "-hls_time", str(SEGMENT_SECONDS), "-hls_list_size", "0",
             "-hls_playlist_type", "vod", "-hls_segment_filename", str(plain / "segment-%03d.ts"),
             str(plain / "stream.m3u8")], check=True, timeout=60,
        )
        for index in range(SEGMENT_COUNT):
            name = f"segment-{index:03d}.ts"
            source = plain / name
            original = source.read_bytes()
            if len(original) % 188 or any(original[offset] != 0x47 for offset in range(0, len(original), 188)):
                raise AssertionError("Generated fixture is not a complete MPEG transport stream")
            encrypted = media / name
            iv = index.to_bytes(16, "big").hex()
            # HLS without an explicit IV uses the media sequence number as a
            # 128-bit big-endian IV. OpenSSL adds the required PKCS#7 padding.
            arguments = [openssl, "enc", "-aes-128-cbc", "-nosalt", "-K", TEST_KEY.hex(), "-iv", iv]
            subprocess.run([*arguments, "-in", str(source), "-out", str(encrypted)], check=True, timeout=10)
            restored = subprocess.run([*arguments, "-d", "-in", str(encrypted)],
                                      capture_output=True, check=True, timeout=10).stdout
            if restored != original:
                raise AssertionError("AES encryption/decryption changed generated media")
            summary["segments"].append({"name": name, "plaintext_sha256": hashlib.sha256(original).hexdigest(),
                                        "encrypted_sha256": hashlib.sha256(encrypted.read_bytes()).hexdigest()})
    (directory / "fixture.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    validate_fixture(directory)
    return summary


def validate_fixture(directory):
    directory = Path(directory)
    summary = json.loads((directory / "fixture.json").read_text(encoding="utf-8"))
    if (summary.get("kind") != "synthetic-aes128-key-validation"
            or summary.get("segment_seconds") != SEGMENT_SECONDS
            or len(summary.get("segments", [])) != SEGMENT_COUNT):
        raise AssertionError("Unexpected AES fixture metadata")
    for index, segment in enumerate(summary["segments"]):
        name = f"segment-{index:03d}.ts"
        if segment["name"] != name:
            raise AssertionError("Unexpected fixture segment filename")
        data = (directory / "media" / name).read_bytes()
        if (len(data) < 1024 or len(data) % 16
                or hashlib.sha256(data).hexdigest() != segment["encrypted_sha256"]):
            raise AssertionError("Encrypted fixture is truncated or changed")


def manifest(case):
    lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2",
             "#EXT-X-MEDIA-SEQUENCE:0", "#EXT-X-PLAYLIST-TYPE:VOD",
             f'#EXT-X-KEY:METHOD=AES-128,URI="/key/{case}"']
    for index in range(SEGMENT_COUNT):
        lines.extend(("#EXTINF:2.000000,", f"/media/segment-{index:03d}.ts"))
    return "\n".join([*lines, "#EXT-X-ENDLIST", ""])


class KeyFixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, address, handler):
        super().__init__(address, handler)
        self.lock = threading.Lock()
        self.requests = Counter()


class KeyFixtureHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def respond(self, status, body, content_type="application/octet-stream", location=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if location:
            self.send_header("Location", location)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        path = urlsplit(self.path).path
        if path.startswith("/key/"):
            case = path.removeprefix("/key/")
            if case not in CASES:
                self.respond(404, b"")
                return
            with self.server.lock:
                self.server.requests[case] += 1
            if case == "redirect":
                self.respond(302, b"", location="/key/valid")
            elif case == "empty":
                self.respond(200, b"")
            elif case == "not-found":
                # Even a correctly sized body containing the correct bytes is
                # not usable key material when the HTTP request failed.
                self.respond(404, TEST_KEY)
            elif case == "short":
                self.respond(200, TEST_KEY[:-1])
            elif case == "long":
                self.respond(200, TEST_KEY + b"\x00")
            else:
                self.respond(200, TEST_KEY)
            return
        for case in CASES:
            if path == f"/{case}/stream.m3u8":
                self.respond(200, manifest(case).encode("utf-8"), "application/vnd.apple.mpegurl")
                return
        super().do_GET()


def observation(output):
    matches = re.findall(r"^TEST observation (.+)$", output, re.MULTILINE)
    if len(matches) != 1:
        raise AssertionError(f"Expected one playback observation, got {len(matches)}")
    return {key: int(value) for key, value in re.findall(r"([a-z_]+)=(-?\d+)", matches[0])}


def check_result(case, result, requests):
    output = result.stdout + "\n" + result.stderr
    if result.returncode < 0 or any(marker in output for marker in (
            "CRITICAL", "assertion failed", "Bail out!", "ERROR: AddressSanitizer", "runtime error:")):
        raise AssertionError(f"{case}: key failure caused an assertion, crash, or sanitizer error:\n{output}")
    observed = observation(result.stdout)
    if requests[case] < 1:
        raise AssertionError(f"{case}: test never requested its key")
    if case in ("valid", "redirect"):
        if (result.returncode != 0 or observed["video_buffers"] < 50 or observed["audio_buffers"] < 100
                or observed["last_video_ms"] < 10000
                or observed["last_audio_ms"] - observed["first_audio_ms"] < 10000):
            raise AssertionError(f"{case}: valid AES key did not decode real video and audio:\n{output}")
        if requests["valid"] != 1 or (case == "redirect" and requests["redirect"] != 1):
            raise AssertionError(f"{case}: successful key was not reused across media segments: {requests}")
    else:
        # GST_STREAM_ERROR_DECRYPT_NOKEY is 13; DECRYPT (unsupported encryption)
        # is a different error. Reject arbitrary decoding failures as success.
        if (result.returncode != 1 or not re.search(
                r"stage=error factory=hlsdemux2 .*domain=stream code=13(?: |$)", result.stdout)
                or observed["video_buffers"] != 0 or observed["audio_buffers"] != 0):
            raise AssertionError(f"{case}: expected a clean missing-key error and no decoded media:\n{output}")
    return observed


def inspect_hls_plugin(env):
    inspect = shutil.which("gst-inspect-1.0")
    if not inspect:
        raise AssertionError("gst-inspect-1.0 is required to verify the private plugin")
    result = subprocess.run([inspect, "hlsdemux2"], env=env, capture_output=True,
                            text=True, timeout=20, check=True)
    filename = re.search(r"^\s*Filename\s+(.+)$", result.stdout, re.MULTILINE)
    version = re.search(r"^\s*Version\s+(.+)$", result.stdout, re.MULTILINE)
    if not filename or not version:
        raise AssertionError("gst-inspect did not identify the HLS plugin filename and version")
    return Path(filename[1].strip()), version[1].strip()


def isolate_plugin(env, plugin_path, temporary):
    """Exclude duplicate system adaptivedemux2 registrations in child processes.

    A fresh registry can register both same-name plugins despite GST_PLUGIN_PATH
    precedence. Symlink other system plugins into a temporary search directory,
    then verify the selected filename before any media is opened.
    """
    candidate = plugin_path / "libgstadaptivedemux2.so"
    if not candidate.is_file():
        raise AssertionError(f"Private plugin is missing: {candidate}")
    env = dict(env)
    system_path = env.get("GST_PLUGIN_SYSTEM_PATH_1_0", env.get("GST_PLUGIN_SYSTEM_PATH"))
    if system_path is None:
        # Discover the installed plugin directory from the runtime itself, so
        # no development package or hardcoded CPU architecture is required.
        installed_env = dict(env)
        installed_env.pop("GST_PLUGIN_PATH", None)
        installed_env.pop("GST_PLUGIN_PATH_1_0", None)
        installed_registry = str(temporary / "installed-registry.bin")
        installed_env["GST_REGISTRY"] = installed_registry
        installed_env["GST_REGISTRY_1_0"] = installed_registry
        installed, _ = inspect_hls_plugin(installed_env)
        directories = [installed.parent]
    else:
        directories = [Path(item).resolve(strict=True) for item in system_path.split(os.pathsep) if item]
    filtered = []
    for index, source in enumerate(directories):
        if not source.is_dir():
            raise AssertionError(f"System plugin search path is not a directory: {source}")
        destination = temporary / f"system-plugins-{index}"
        destination.mkdir()
        for entry in source.rglob("*"):
            if (entry.is_file() and entry.suffix in (".so", ".dylib")
                    and entry.name != candidate.name):
                link = destination / entry.relative_to(source)
                link.parent.mkdir(parents=True, exist_ok=True)
                link.symlink_to(entry.absolute())
        filtered.append(str(destination))
    env["GST_PLUGIN_PATH"] = str(plugin_path)
    env["GST_PLUGIN_PATH_1_0"] = str(plugin_path)
    env["GST_PLUGIN_SYSTEM_PATH"] = os.pathsep.join(filtered)
    env["GST_PLUGIN_SYSTEM_PATH_1_0"] = os.pathsep.join(filtered)
    registry = str(temporary / "candidate-registry.bin")
    env["GST_REGISTRY"] = registry
    env["GST_REGISTRY_1_0"] = registry
    selected, version = inspect_hls_plugin(env)
    if selected != candidate:
        raise AssertionError(f"Wrong HLS plugin selected: expected {candidate}, got {selected}")
    print(f"Verified private hlsdemux2 plugin: {selected}; version={version}", flush=True)
    return env


def run_cases(binary, directory, cases, plugin_path=None):
    validate_fixture(directory)
    handler = functools.partial(KeyFixtureHandler, directory=str(directory))
    with tempfile.TemporaryDirectory(prefix="uxplay-key-registry-") as registry:
        env = dict(os.environ)
        env["LC_ALL"] = "C"
        env["G_DEBUG"] = "fatal-warnings"
        env["GST_PLUGIN_FEATURE_RANK"] = "v4l2h264dec:0,v4l2slh265dec:0"
        env["GST_REGISTRY"] = str(Path(registry) / "registry.bin")
        env["GST_REGISTRY_1_0"] = env["GST_REGISTRY"]
        if plugin_path:
            env = isolate_plugin(env, plugin_path, Path(registry))
        with KeyFixtureServer(("127.0.0.1", 0), handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                base = f"http://127.0.0.1:{server.server_port}"
                for case in cases:
                    with server.lock:
                        server.requests.clear()
                    uri = f"{base}/{case}/stream.m3u8"
                    command = ([binary, "--pi4-direct", "--observe", uri, "12"]
                               if case in ("valid", "redirect") else [binary, "--pi4-direct", uri, "0"])
                    result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=35)
                    with server.lock:
                        requests = server.requests.copy()
                    observed = check_result(case, result, requests)
                    print(f"{case}: key validation passed; requests={dict(requests)}; media={observed}")
            finally:
                server.shutdown()
                thread.join(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", help="Headless test_direct_renderer_hls executable")
    parser.add_argument("--fixtures", type=Path)
    parser.add_argument("--generate-only", type=Path)
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"))
    parser.add_argument("--openssl", default=shutil.which("openssl"))
    parser.add_argument("--gst-plugin-path", type=Path)
    parser.add_argument("--case", action="append", choices=CASES, dest="cases")
    args = parser.parse_args()
    if args.generate_only:
        if not args.ffmpeg or not args.openssl:
            parser.error("FFmpeg and OpenSSL are required to generate fixtures")
        generate_fixture(args.generate_only.resolve(), args.ffmpeg, args.openssl)
        print("Generated and verified eight AES-128 encrypted synthetic H264/AAC segments.")
        return
    if not args.binary:
        parser.error("a test executable is required unless --generate-only is used")
    binary = str(Path(args.binary).resolve(strict=True))
    plugin_path = args.gst_plugin_path.resolve(strict=True) if args.gst_plugin_path else None
    if plugin_path and not plugin_path.is_dir():
        parser.error("--gst-plugin-path must be a plugin directory")
    if args.fixtures:
        run_cases(binary, args.fixtures.resolve(strict=True), args.cases or CASES, plugin_path)
    else:
        if not args.ffmpeg or not args.openssl:
            parser.error("FFmpeg and OpenSSL are required; use --fixtures to reuse generated media")
        with tempfile.TemporaryDirectory(prefix="uxplay-aes128-hls-") as temporary:
            directory = Path(temporary)
            generate_fixture(directory, args.ffmpeg, args.openssl)
            run_cases(binary, directory, args.cases or CASES, plugin_path)


if __name__ == "__main__":
    main()
