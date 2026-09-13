#!/usr/bin/env python3
"""Capture real headless Weston pixels across persistent-background handovers.

Linux dependencies: Weston 14 (including weston-screenshooter), mpv, FFmpeg,
python3-gi, gir1.2-gst-plugins-base-1.0, python3-pil and GStreamer waylandsink.
Example: python3 tests/run_wayland_display_test.py --output-dir /tmp/display-qa
Use --shell /absolute/path/kiosk-shell.so to qualify a patched kiosk shell.
Use --gst-plugin-path for the private waylandsink setter repair. The default
requests fullscreen and makes GLib criticals fatal. --gst-fullscreen=no is a
negative control for the old client-size negotiation; --geometry-only runs
the portrait/landscape/rotation checks without mpv or FFmpeg.

The headless backend has no input seat. The private compositor alone enables
Weston's capture/debug extension; never enable that extension on a production
receiver. All media and HTTP requests are synthetic and local. This tests real
Wayland surface visibility with software rendering, not Pi decoding, HDMI,
audio, receiver state decisions, or the absence of flashes between captures.
"""

import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=30, capture_output=True,
                          text=True, **kwargs)


def wait_for(predicate, description, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise RuntimeError(f"Timed out: {description}")


class Player:
    def __init__(self, folder, env, mpv):
        self.log = (folder / "mpv.log").open("w")
        self.path = folder / "mpv.sock"
        self.process = subprocess.Popen([
            mpv, "--no-config", "--vo=wlshm", "--hwdec=no", "--ao=null",
            "--force-window=no", "--fullscreen=yes", "--idle=yes",
            "--osd-level=0", "--input-terminal=no", "--terminal=no",
            "--input-default-bindings=no", f"--input-ipc-server={self.path}"],
            env=env, stdout=self.log, stderr=self.log)
        wait_for(self.path.exists, "mpv IPC socket")
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(8)
        self.sock.connect(str(self.path))
        self.reader = self.sock.makefile("rb")
        self.request_id = 0
        self.events = []

    def command(self, *args):
        self.request_id += 1
        self.sock.sendall(json.dumps({"command": args,
                                     "request_id": self.request_id}).encode() + b"\n")
        while True:
            message = json.loads(self.reader.readline())
            if "event" in message:
                self.events.append(message)
            if message.get("request_id") == self.request_id:
                if args[0] == "get_property" and message.get("error") == "property unavailable":
                    return None
                if message.get("error") != "success":
                    raise RuntimeError(f"mpv command failed: {message}")
                return message.get("data")

    def close(self):
        self.reader.close()
        self.sock.close()
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=5)
        self.log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--weston", default="weston")
    parser.add_argument("--shell", default="kiosk-shell.so")
    parser.add_argument("--screenshooter", default="weston-screenshooter")
    parser.add_argument("--mpv", default="mpv")
    parser.add_argument("--gst-plugin-path", type=Path)
    parser.add_argument("--gst-system-plugin-path", type=Path)
    parser.add_argument("--gst-fullscreen", choices=("yes", "no"), default="yes")
    parser.add_argument("--geometry-only", action="store_true")
    args = parser.parse_args()
    executables = [args.weston, args.screenshooter]
    if not args.geometry_only:
        executables += [args.mpv, "ffmpeg"]
    for executable in executables:
        if not shutil.which(executable):
            parser.error(f"Executable unavailable: {executable}")
    folder = args.output_dir.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    if any(folder.iterdir()):
        parser.error("Use a new empty output directory to preserve evidence.")
    runtime = folder / "runtime"
    runtime.mkdir(mode=0o700)
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY="display-qa",
               G_DEBUG="fatal-criticals", GST_REGISTRY=str(folder / "gst-registry.bin"))
    env["GST_REGISTRY_1_0"] = env["GST_REGISTRY"]
    if args.gst_plugin_path:
        env["GST_PLUGIN_PATH"] = str(args.gst_plugin_path.resolve())
        env["GST_PLUGIN_PATH_1_0"] = env["GST_PLUGIN_PATH"]
    if args.gst_system_plugin_path:
        env["GST_PLUGIN_SYSTEM_PATH"] = str(args.gst_system_plugin_path.resolve())
        env["GST_PLUGIN_SYSTEM_PATH_1_0"] = env["GST_PLUGIN_SYSTEM_PATH"]
    os.environ.update(env)
    import gi
    gi.require_version("Gst", "1.0")
    from gi.repository import Gst
    from PIL import Image, ImageChops
    Gst.init(None)
    output_size = (1280, 720)
    background = player = server = secondary = tertiary = None
    gates = {}
    report = {"scope": "Headless software Wayland visibility; no Pi/HDMI/audio claim",
              "weston": run([args.weston, "--version"]).stdout.strip(),
              "mpv": None if args.geometry_only else run([args.mpv, "--version"]).stdout.splitlines()[0],
              "shell": args.shell, "output_size": output_size,
              "gst_fullscreen": args.gst_fullscreen,
              "gst_plugin": Gst.Registry.get().find_plugin("waylandsink").get_filename(),
              "fatal_criticals": True, "captures": [], "checks": []}
    if args.gst_plugin_path and not Path(report["gst_plugin"]).resolve().is_relative_to(args.gst_plugin_path.resolve()):
        raise RuntimeError("GStreamer selected a different waylandsink plugin")
    colors = {"ready": (0, 255, 0), "loading": (0, 0, 255),
              "error": (255, 255, 0), "red": (254, 0, 0),
              "purple": (127, 0, 254), "mirroring": (0, 255, 255)}

    compositor_log = (folder / "weston-process.log").open("w")
    compositor = subprocess.Popen([
        args.weston, "--backend=headless", "--renderer=pixman",
        f"--shell={args.shell}", "--width=1280", "--height=720",
        "--socket=display-qa", "--idle-time=0", "--no-config", "--debug",
        f"--log={folder / 'weston.log'}"],
        env=env, stdout=compositor_log, stderr=compositor_log)

    def pipeline(color, source_size=output_size):
        width, height = source_size
        item = Gst.parse_launch(
            "videotestsrc name=background is-live=true pattern=solid-color ! "
            f"capsfilter name=source_caps caps=video/x-raw,width={width},height={height},framerate=10/1 ! "
            f"videoconvert ! waylandsink name=video_sink fullscreen={'true' if args.gst_fullscreen == 'yes' else 'false'}")
        r, g, b = colors[color]
        item.get_by_name("background").set_property("foreground-color", 0xff000000 | r << 16 | g << 8 | b)
        if item.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
            raise RuntimeError("GStreamer background failed to start")
        if item.get_state(3 * Gst.SECOND)[1] != Gst.State.PLAYING:
            raise RuntimeError("GStreamer background did not reach PLAYING")
        return item

    def state(color):
        r, g, b = colors[color]
        background.get_by_name("background").set_property(
            "foreground-color", 0xff000000 | r << 16 | g << 8 | b)
        # Allow the 10 Hz source and sink queue to present the changed frame.
        time.sleep(0.35)

    def capture(label, expected, source_size=None):
        index = len(report["captures"])
        directory = folder / f"{index:03d}-{label}"
        directory.mkdir()
        capture_env = dict(env, WAYLAND_DEBUG="client") if index == 0 else env
        captured = run([args.screenshooter], env=capture_env, cwd=directory)
        if index == 0:
            (folder / "capture-registry.log").write_text(captured.stderr)
            no_seat = '"wl_seat"' not in captured.stderr and '"wl_output"' in captured.stderr
            report["checks"].append({"no_input_seat_advertised": no_seat})
            if not no_seat:
                raise RuntimeError("Capture registry does not confirm a compositor without input seats")
        files = list(directory.glob("*.png"))
        if len(files) != 1:
            raise RuntimeError("Compositor capture did not produce one PNG")
        im = Image.open(files[0]).convert("RGB")
        if im.size != output_size:
            raise RuntimeError(f"Unexpected capture dimensions: {im.size}")
        histogram = im.getcolors(im.width * im.height)
        choices = (expected,) if isinstance(expected, str) else expected
        matches = {choice: sum(count for count, rgb in histogram
                               if all(abs(a - b) <= 5 for a, b in zip(rgb, colors[choice])))
                   for choice in choices}
        matched_color = max(matches, key=matches.get)
        matched = matches[matched_color]
        fraction = matched / (im.width * im.height)
        result = {"stage": label, "expected": expected, "matched_color": matched_color,
                  "matched_fraction": fraction,
                  "center_rgb": im.getpixel((im.width // 2, im.height // 2)),
                  "image": str(files[0].relative_to(folder)), "pass": fraction > 0.98}
        if source_size:
            # Check the content's bounding box and padding, not just a center
            # pixel: a cropped, stretched, stale, or rejected portrait must fail.
            scale = min(im.width / source_size[0], im.height / source_size[1])
            width, height = (round(n * scale) for n in source_size)
            x, y = (im.width - width) // 2, (im.height - height) // 2
            target = (x, y, x + width, y + height)
            def color_mask(picture, color):
                channels = ImageChops.difference(picture, Image.new("RGB", picture.size, color)).split()
                difference = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
                return difference.point(lambda value: 255 if value <= 5 else 0)

            mask = color_mask(im, colors[expected])
            bbox = mask.getbbox()
            content = mask.crop((x + 4, y + 4, x + width - 4, y + height - 4))
            content_fraction = content.histogram()[255] / (content.width * content.height)
            black = color_mask(im, (0, 0, 0))
            padding_regions = [(0, 0, x - 4, im.height), (x + width + 4, 0, im.width, im.height),
                               (x, 0, x + width, y - 4), (x, y + height + 4, x + width, im.height)]
            padding = [black.crop(rect) for rect in padding_regions if rect[2] > rect[0] and rect[3] > rect[1]]
            padding_count = sum(region.width * region.height for region in padding)
            black_fraction = sum(region.histogram()[255] for region in padding) / padding_count if padding_count else 1.0
            result.update(source_size=source_size, expected_content_bbox=target,
                          actual_content_bbox=bbox, content_fraction=content_fraction,
                          black_padding_fraction=black_fraction,
                          **{"pass": bool(bbox) and all(abs(a - b) <= 3 for a, b in zip(bbox, target))
                             and content_fraction > 0.98 and black_fraction > 0.98})
        report["captures"].append(result)
        print(f"{label}: {'PASS' if result['pass'] else 'FAIL'} "
              f"{fraction:.1%} {expected}, center={result['center_rgb']}", flush=True)
        return result["pass"]

    def set_source_size(item, size):
        item.get_by_name("source_caps").set_property("caps", Gst.Caps.from_string(
            f"video/x-raw,width={size[0]},height={size[1]},framerate=10/1"))
        sink = item.get_by_name("video_sink")

        def negotiated():
            message = item.get_bus().pop_filtered(Gst.MessageType.ERROR)
            if message:
                raise RuntimeError(f"Geometry pipeline failed: {message.parse_error()[0].message}")
            caps = sink.get_static_pad("sink").get_current_caps()
            if not caps:
                return False
            structure = caps.get_structure(0)
            return (structure.get_value("width"), structure.get_value("height")) == size

        wait_for(negotiated, f"dynamic source dimensions {size}")
        time.sleep(0.35)

    def geometry_sequence():
        nonlocal secondary
        portrait, landscape = (498, 1080), (1920, 1080)
        secondary = pipeline("mirroring", portrait)
        time.sleep(0.35)
        capture("portrait-mismatch-pillarbox", "mirroring", portrait)
        # The same pipeline and sink remain PLAYING throughout caps changes.
        set_source_size(secondary, landscape)
        capture("same-surface-rotate-landscape", "mirroring", landscape)
        set_source_size(secondary, portrait)
        capture("same-surface-rotate-back-portrait", "mirroring", portrait)
        secondary.set_state(Gst.State.NULL)
        secondary = None
        time.sleep(0.2)
        capture("portrait-close-restores-background", "ready")
        secondary = pipeline("mirroring", landscape)
        time.sleep(0.35)
        capture("oversized-landscape-mismatch", "mirroring", landscape)
        secondary.set_state(Gst.State.NULL)
        secondary = None
        time.sleep(0.2)
        capture("landscape-close-restores-background", "ready")

    def loading(path, previous_picture=None):
        state("loading")
        player.command("stop")
        wait_for(lambda: player.command("get_property", "idle-active"), "stopped player idle")
        # A stop reply can precede destruction of mpv's old Wayland surface.
        # Its previous picture is valid during this immediate handover. The
        # gated HTTP captures below must then show the loading background.
        time.sleep(0.1)
        expected = ("loading", previous_picture) if previous_picture else "loading"
        capture(path + "-stopped", expected)
        gates[path] = (threading.Event(), threading.Event())
        player.command("loadfile", f"http://127.0.0.1:{server.server_port}/{path}")
        if not gates[path][0].wait(8):
            raise RuntimeError("mpv did not request gated local media")
        for i in range(3):
            capture(path + f"-before-first-frame-{i}", "loading")
        gates[path][1].set()

    try:
        wait_for((runtime / "display-qa").exists, "Wayland socket")
        if not args.geometry_only:
            for name, color in (("red.mp4", "red"), ("purple.mp4", "0x8000ff")):
                run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                     "-f", "lavfi", "-i", f"color={color}:size=640x360:rate=24",
                     "-t", "20", "-c:v", "libx264", "-preset", "ultrafast",
                     "-movflags", "+faststart", str(folder / name)])

            class Handler(http.server.BaseHTTPRequestHandler):
                def log_message(self, *_):
                    pass

                def do_GET(self):
                    name = self.path.lstrip("/")
                    requested, release = gates[name]
                    requested.set()
                    if not release.wait(15):
                        return
                    if name == "missing.mp4":
                        self.send_error(404)
                        return
                    data = (folder / name).read_bytes()
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(data)))
                    self.send_header("Content-Type", "video/mp4")
                    self.end_headers()
                    try:
                        self.wfile.write(data)
                    except (BrokenPipeError, ConnectionResetError):
                        pass

            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            server.daemon_threads = True
            threading.Thread(target=server.serve_forever, daemon=True).start()
            background = pipeline("ready")
            time.sleep(0.2)
            capture("ready", "ready")
            player = Player(folder, env, args.mpv)
            capture("idle-mpv-does-not-cover-ready", "ready")
            loading("red.mp4")
            wait_for(lambda: player.command("get_property", "video-out-params") is not None,
                     "first video output")
            time.sleep(0.25)
            capture("playing-first-stream", "red")
            loading("purple.mp4", previous_picture="red")
            wait_for(lambda: player.command("get_property", "video-out-params") is not None,
                     "replacement video output")
            time.sleep(0.25)
            capture("playing-replacement", "purple")
            state("ready")
            player.command("stop")
            time.sleep(0.2)
            capture("stop-restores-original-background", "ready")
            loading("missing.mp4")
            wait_for(lambda: player.command("get_property", "idle-active"), "failed load idle")
            state("error")
            capture("failed-load-shows-error", "error")
            player.command("loadfile", str(folder / "red.mp4"))
            wait_for(lambda: player.command("get_property", "video-out-params") is not None,
                     "video before crash")
            time.sleep(0.25)
            capture("playing-before-player-crash", "red")
            state("ready")
            player.process.kill()
            player.process.wait(timeout=5)
            time.sleep(0.2)
            capture("crash-restores-original-background", "ready")
            secondary = pipeline("mirroring")
            time.sleep(0.2)
            capture("gstreamer-video-surface", "mirroring")
            secondary.set_state(Gst.State.NULL)
            secondary = None
            time.sleep(0.2)
            capture("gstreamer-stop-restores-background", "ready")
            secondary = pipeline("mirroring")
            tertiary = pipeline("red")
            time.sleep(0.2)
            capture("three-client-top-surface", "red")
            secondary.set_state(Gst.State.NULL)
            secondary = None
            time.sleep(0.2)
            capture("hidden-client-close-preserves-top-surface", "red")
            tertiary.set_state(Gst.State.NULL)
            tertiary = None
            time.sleep(0.2)
            capture("last-client-close-restores-background", "ready")
        else:
            background = pipeline("ready")
            time.sleep(0.35)
            capture("ready", "ready")
        geometry_sequence()
        report["checks"].append({"persistent_background_playing":
                                  background.get_state(0)[1] == Gst.State.PLAYING})
    except Exception as error:
        report["exception"] = str(error)
        raise
    finally:
        for _, release in gates.values():
            release.set()
        if tertiary:
            tertiary.set_state(Gst.State.NULL)
        if secondary:
            secondary.set_state(Gst.State.NULL)
        if player:
            player.close()
        if background:
            background.set_state(Gst.State.NULL)
        if server:
            server.shutdown()
            server.server_close()
        compositor.terminate()
        compositor.wait(timeout=5)
        compositor_log.close()
        report["pass"] = (bool(report["captures"]) and "exception" not in report
                          and all(item["pass"] for item in report["captures"])
                          and all(all(item.values()) for item in report["checks"]))
        (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    raise SystemExit(0 if report["pass"] else 1)


if __name__ == "__main__":
    if os.environ.get("UXPLAY_WAYLAND_QA_CHILD") == "1":
        main()
    else:
        # Fatal-critical negative controls abort Python before finally runs.
        # Keep every compositor/player in our own process group and clean it
        # even when the deliberately broken client aborts or times out.
        child = subprocess.Popen([sys.executable, __file__, *sys.argv[1:]],
            env=dict(os.environ, UXPLAY_WAYLAND_QA_CHILD="1"), start_new_session=True)
        result = 1
        try:
            result = child.wait(timeout=180)
        finally:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=5)
        raise SystemExit(result if result >= 0 else 128 - result)
