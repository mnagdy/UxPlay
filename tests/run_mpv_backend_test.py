#!/usr/bin/env python3
"""Exercise actual mpv/FFmpeg over local HTTP without acquiring output devices.

Usage: run_mpv_backend_test.py /path/to/test_mpv_backend
Requires mpv and ffmpeg (with libx264/libx265) in PATH. This establishes software
demux/decode/control compatibility only, never Pi hardware or audible sound.
"""
import argparse
import http.server
import mimetypes
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading
import urllib.parse


def run(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(f"Fixture command failed ({command[0]}):\n{result.stderr[-3000:]}")
    return result.stdout.strip()


def make_fixtures(folder):
    common = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y"]
    run(common + ["-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24",
                  "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                  "-t", "8", "-c:v", "libx264", "-preset", "ultrafast",
                  "-pix_fmt", "yuv420p", "-g", "48", "-sc_threshold", "0",
                  "-c:a", "aac", "-movflags", "+faststart", str(folder / "av.mp4")])
    run(common + ["-i", str(folder / "av.mp4"), "-c:v", "libx265", "-preset", "ultrafast",
                  "-x265-params", "pools=1:frame-threads=1:log-level=error", "-c:a", "copy",
                  "-movflags", "+faststart", str(folder / "hevc.mp4")])
    variants = [
        ("h264", ["-c", "copy"]),
        ("video", ["-an", "-c:v", "copy"]),
        ("audio", ["-vn", "-c:a", "copy"]),
    ]
    for name, options in variants:
        run(common + ["-i", str(folder / "av.mp4")] + options +
            ["-hls_time", "2", "-hls_playlist_type", "vod",
             "-hls_segment_filename", str(folder / f"{name}-%03d.ts"),
             str(folder / f"{name}.m3u8")])
    run(common + ["-i", str(folder / "av.mp4"), "-c", "copy", "-hls_time", "2",
                  "-hls_playlist_type", "vod", "-hls_segment_type", "fmp4",
                  "-hls_fmp4_init_filename", "init.mp4",
                  "-hls_segment_filename", str(folder / "fmp4-%03d.m4s"),
                  str(folder / "fmp4.m3u8")])
    (folder / "separate-audio.m3u8").write_text(
        '#EXTM3U\n#EXT-X-VERSION:3\n'
        '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="Test",DEFAULT=YES,'
        'AUTOSELECT=YES,URI="audio.m3u8"\n'
        '#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=320x180,'
        'CODECS="avc1.42c00c,mp4a.40.2",AUDIO="audio"\nvideo.m3u8\n',
        encoding="utf-8")
    (folder / "local-reference.m3u8").write_text(
        '#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:8\n#EXTINF:8,\n'
        + (folder / "h264-000.ts").as_uri() + '\n#EXT-X-ENDLIST\n', encoding="utf-8")
    (folder / "local-reference.m3u").write_text(
        '#EXTM3U\n' + (folder / "av.mp4").as_uri() + '\n', encoding="utf-8")


def handler_for(folder):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def do_HEAD(self):
            self.serve(False)

        def do_GET(self):
            self.serve(True)

        def serve(self, body):
            path = (folder / urllib.parse.unquote(urllib.parse.urlsplit(self.path).path).lstrip("/")).resolve()
            if not path.is_relative_to(folder) or not path.is_file():
                self.send_error(404)
                return
            length = path.stat().st_size
            start, end = 0, length - 1
            header = self.headers.get("Range")
            partial = False
            if header:
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", header)
                if not match:
                    self.send_error(416)
                    return
                start = int(match[1])
                end = min(int(match[2]), end) if match[2] else end
                if start > end or start >= length:
                    self.send_error(416)
                    return
                partial = True
            self.send_response(206 if partial else 200)
            mime = {".m3u8": "application/vnd.apple.mpegurl", ".ts": "video/mp2t",
                    ".m4s": "video/iso.segment"}.get(path.suffix)
            self.send_header("Content-Type", mime or mimetypes.guess_type(path.name)[0] or "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(end - start + 1))
            if partial:
                self.send_header("Content-Range", f"bytes {start}-{end}/{length}")
            self.end_headers()
            if body:
                try:
                    with path.open("rb") as source:
                        source.seek(start)
                        remaining = end - start + 1
                        while remaining:
                            chunk = source.read(min(remaining, 65536))
                            if not chunk:
                                break
                            self.wfile.write(chunk)
                            remaining -= len(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    pass
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_binary", type=Path)
    args = parser.parse_args()
    for name in ("mpv", "ffmpeg"):
        if not shutil.which(name):
            parser.error(f"{name} is required in PATH")
    with tempfile.TemporaryDirectory(prefix="uxplay-mpv-http-") as tmp:
        folder = Path(tmp).resolve()
        make_fixtures(folder)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler_for(folder))
        server.daemon_threads = True
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            for name in ("av.mp4", "h264.m3u8", "fmp4.m3u8", "separate-audio.m3u8", "hevc.mp4"):
                output = run([str(args.test_binary.resolve()), "--real",
                              f"http://127.0.0.1:{server.server_port}/{name}"])
                print(f"{name}: {output}", flush=True)
            for name in ("local-reference.m3u8", "local-reference.m3u"):
                output = run([str(args.test_binary.resolve()), "--real-rejected",
                              f"http://127.0.0.1:{server.server_port}/{name}"])
                print(f"{name}: {output}", flush=True)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    main()
