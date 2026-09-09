#!/usr/bin/env python3
"""Generate legal local media and exercise direct playback over real HTTP HLS.

Pass the compiled test_direct_renderer_hls executable. Requires ffmpeg with
libx264/libvpx-vp9/AAC encoders and GStreamer's HLS and codec plugins at runtime.
The 180-second fixture is small, low-resolution, generated test video and tone;
playback need only advance a few seconds at zero and at a nonzero resume point.
"""

import functools
import http.server
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading


class QuietFixtureHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass


def make_fixture(directory):
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "testsrc2=size=160x90:rate=10", "-t", "180", "-an", "-c:v", "libx264",
         "-preset", "ultrafast", "-g", "20", "-keyint_min", "20", "-sc_threshold",
         "0", "-f", "hls", "-hls_time", "2", "-hls_list_size", "0",
         "-hls_playlist_type", "vod", "-hls_segment_filename",
         str(directory / "video-%03d.ts"), str(directory / "video.m3u8")],
        check=True, timeout=60,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "testsrc2=size=160x90:rate=10", "-t", "180", "-an", "-c:v", "libvpx-vp9",
         "-deadline", "realtime", "-cpu-used", "8", "-g", "20", "-f", "hls",
         "-hls_time", "2", "-hls_list_size", "0", "-hls_playlist_type", "vod",
         "-hls_segment_type", "fmp4", "-hls_fmp4_init_filename", "vp9-init.mp4",
         "-hls_segment_filename", str(directory / "vp9-%03d.m4s"),
         str(directory / "vp9.m3u8")],
        check=True, timeout=60,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "sine=frequency=440:sample_rate=44100", "-t", "180", "-vn", "-c:a",
         "aac", "-b:a", "48k", "-f", "hls", "-hls_time", "2", "-hls_list_size",
         "0", "-hls_playlist_type", "vod", "-hls_segment_filename",
         str(directory / "audio-%03d.ts"), str(directory / "audio.m3u8")],
        check=True, timeout=60,
    )
    (directory / "master.m3u8").write_text(
        '#EXTM3U\n#EXT-X-VERSION:3\n'
        '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="English",LANGUAGE="en",'
        'AUTOSELECT=YES,DEFAULT=YES,URI="audio.m3u8"\n'
        '#EXT-X-STREAM-INF:BANDWIDTH=500000,AVERAGE-BANDWIDTH=300000,'
        'RESOLUTION=160x90,FRAME-RATE=10.0,CODECS="avc1.42c00a,mp4a.40.2",'
        'AUDIO="audio"\nvideo.m3u8\n', encoding="utf-8",
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: run_hls_renderer_test.py TEST_DIRECT_RENDERER_HLS")
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg is required to generate the HTTP HLS fixture")
    binary = str(Path(sys.argv[1]).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="uxplay-hls-") as temporary:
        directory = Path(temporary)
        make_fixture(directory)
        handler = functools.partial(QuietFixtureHandler, directory=str(directory))
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                base = f"http://127.0.0.1:{server.server_port}"
                uri = f"{base}/master.m3u8"
                for start in (0, 165):
                    subprocess.run([binary, uri, str(start)], check=True, timeout=45)
                master = (directory / "master.m3u8").read_text(encoding="utf-8")
                mixed = master.replace('URI="audio.m3u8"', f'URI="{base}/audio.m3u8"')
                mixed = mixed.replace("\nvideo.m3u8\n", f"\n{base}/video.m3u8\n")
                mixed += (
                    '#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=160x90,'
                    'FRAME-RATE=10.0,CODECS="vp09.00.10.08,mp4a.40.2",AUDIO="audio"\n'
                    f'{base}/vp9.m3u8\n'
                )
                mixed_path = directory / "mixed-master.m3u8"
                filtered_path = directory / "pi4-master.m3u8"
                mixed_path.write_text(mixed, encoding="utf-8")
                baseline = subprocess.run(
                    [binary, f"{base}/mixed-master.m3u8", "0"],
                    capture_output=True, text=True, timeout=45,
                )
                # GStreamer 1.26.2 cannot construct common video caps for this
                # master. A future fixed GStreamer may accept it; the profile
                # checks below must still pass and retain hardware-friendly AVC.
                if baseline.returncode:
                    if (baseline.returncode != 1
                            or "stage=error factory=hlsdemux2" not in baseline.stdout
                            or "flow_reason=error" not in baseline.stdout):
                        raise AssertionError(f"Unexpected mixed-codec failure:\n{baseline.stdout}\n{baseline.stderr}")
                    print("Unfiltered mixed H264/VP9 master reproduced the GStreamer demux error.")
                else:
                    print("This GStreamer version supports the unfiltered mixed-codec master.")
                subprocess.run(
                    [binary, "--prepare-pi4", str(mixed_path), str(filtered_path), base],
                    check=True, timeout=10,
                )
                filtered = filtered_path.read_text(encoding="utf-8")
                assert "avc1." in filtered and "/video.m3u8" in filtered
                assert "vp09" not in filtered and "/vp9.m3u8" not in filtered
                assert "/audio.m3u8" in filtered
                for start in (0, 165):
                    subprocess.run([binary, f"{base}/pi4-master.m3u8", str(start)], check=True, timeout=45)
            finally:
                server.shutdown()
                thread.join(timeout=5)
    print("HTTP HLS with separate audio: initial playback, resume and production Pi 4 profile passed.")


if __name__ == "__main__":
    main()
