#!/usr/bin/env python3
"""Verify real HTTP response, delayed-body and error diagnostics without hardware."""
import http.server
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


def main():
    binary = str(Path(sys.argv[1]).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="uxplay-http-diagnostics-") as directory:
        fixture = Path(directory) / "fixture.webm"
        subprocess.run([binary, "--fixture", str(fixture)], check=True, timeout=10)
        media = fixture.read_bytes()

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                if self.path.startswith("/missing/"):
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Type", "video/webm; private=SECRET_NOT_TO_LOG")
                self.send_header("Content-Length", str(len(media)))
                self.send_header("Set-Cookie", "SECRET_NOT_TO_LOG")
                self.end_headers()
                if self.path.startswith("/slow/"):
                    time.sleep(7)
                try:
                    self.wfile.write(media)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                for path, expected in (("good", "ok"), ("missing", "error"), ("slow", "slow")):
                    url = f"http://127.0.0.1:{server.server_port}/{path}/SECRET_NOT_TO_LOG?token=SECRET_NOT_TO_LOG"
                    result = subprocess.run([binary, "--http", url, expected], capture_output=True, text=True, timeout=20)
                    # Check output before displaying it, including stderr.
                    output = result.stdout + result.stderr
                    if "SECRET_NOT_TO_LOG" in output:
                        raise AssertionError("HTTP diagnostics leaked private fixture metadata")
                    print(output, end="")
                    if result.returncode:
                        raise AssertionError(f"HTTP diagnostics scenario {path} failed")
                    if expected != "error":
                        assert "stage=first-source-data" in output
                        assert "stage=first-video-sink-buffer" in output
            finally:
                server.shutdown()
                thread.join(timeout=5)
    print("HTTP diagnostics: playback, 404 and a silent delayed body passed.")


if __name__ == "__main__":
    main()
