#!/usr/bin/env python3
"""Bounded mpv protocol fixture. Receives only synthetic test media URLs."""
import fcntl
import json
import os
import signal
import socket
import sys
import threading
import time

mode = os.environ.get("UXPLAY_FAKE_MPV_MODE", "normal")
log_path = os.environ.get("UXPLAY_FAKE_MPV_LOG")


def log(value):
    if log_path:
        with open(log_path, "a", encoding="utf-8") as target:
            target.write(json.dumps(value) + "\n")


log({"argv": sys.argv, "pid": os.getpid()})
extra_fd = os.environ.get("UXPLAY_FAKE_MPV_EXTRA_FD")
if extra_fd:
    try:
        os.fstat(int(extra_fd))
    except OSError:
        pass
    else:
        log({"inherited_extra_descriptor": True})
        sys.exit(8)
lock = None
if os.environ.get("UXPLAY_FAKE_MPV_LOCK"):
    lock = open(os.environ["UXPLAY_FAKE_MPV_LOCK"], "a", encoding="utf-8")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        log({"overlapping_children": True})
        sys.exit(4)

fd = next(int(a.split("fd://", 1)[1]) for a in sys.argv if a.startswith("--input-ipc-client=fd://"))
sock = socket.socket(fileno=fd)
sock.settimeout(5)
observations = {}
deferred = []
loaded = False
send_lock = threading.Lock()


def send(value):
    data = json.dumps(value, separators=(",", ":")).encode() + b"\n"
    with send_lock:
        if mode == "partial":
            for i in range(0, len(data), 7):
                sock.sendall(data[i:i + 7])
                time.sleep(0.0001)
        else:
            sock.sendall(data)


def event(event_type, **kwargs):
    send({"event": event_type, **kwargs})


def prop(name, value):
    if name in observations:
        event("property-change", id=observations[name], name=name, data=value)


def reply(request, data=None, error="success"):
    send({"request_id": request["request_id"], "data": data, "error": error})


buffer = b""
try:
    while True:
        chunk = sock.recv(8192)
        if not chunk:
            break
        buffer += chunk
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            request = json.loads(line)
            args = request["command"]
            name = args[0]
            log({"command": args, "request_id": request["request_id"]})
            if mode == "hang-start" and name != "quit":
                continue
            if name == "get_property":
                if args[1] == "mpv-version":
                    reply(request, "mpv fixture")
                elif args[1] == "command-list":
                    commands = ["loadfile", "seek", "quit", "show-text"]
                    if mode == "missing-command":
                        commands.remove("loadfile")
                    reply(request, [{"name": c} for c in commands])
                elif args[1] == "property-list":
                    reply(request, ["time-pos", "duration", "pause", "paused-for-cache", "seekable"])
            elif name == "observe_property":
                if mode == "metadata-unavailable" and args[1] > 5:
                    reply(request, error="property not found")
                    continue
                observations[args[2]] = args[1]
                if mode == "out-of-order" and args[1] <= 5:
                    deferred.append(request)
                    if args[1] == 5:
                        for old in reversed(deferred):
                            reply(old)
                else:
                    reply(request)
            elif name == "loadfile":
                if mode == "load-error":
                    reply(request, error="Cannot load http://private.invalid/do-not-log")
                    continue
                reply(request)
                if mode == "load-hang":
                    continue
                if mode == "malformed":
                    sock.sendall(b"{not-json}\n")
                    continue
                if mode == "oversized":
                    sock.sendall(b"{" + b"x" * 70000)
                    continue
                if mode == "eof-ipc":
                    sock.close()
                    sys.exit(0)
                if mode == "crash":
                    os._exit(17)
                # Unknown/stale request IDs must never fail the new session.
                send({"request_id": -100, "error": "stale failure"})
                event("property-change", id=-(2**63), name="duration", data=9999)
                prop("duration", None)
                prop("seekable", mode != "nonseekable")
                prop("time-pos", 3.5)
                prop("video-params", {"w": 1920, "h": 1080, "pixelformat": "yuv420p"})
                prop("video-dec-params", {"w": 1920, "h": 1080, "pixelformat": "yuv420p"})
                prop("core-idle", False)
                prop("track-list", [{"id": 1, "type": "video", "selected": True},
                                    {"id": 2, "type": "audio", "selected": True,
                                     "external-filename": "https://private.invalid/never-retain"}])
                prop("video-format", "h264")
                # Real compatibility property describes the codec, not decoder.
                prop("video-codec", "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10")
                prop("current-tracks/video/codec", "h264")
                prop("current-tracks/video/decoder", "h264")
                prop("current-tracks/video/codec-profile", "High")
                prop("current-tracks/audio/codec", "aac")
                prop("current-tracks/audio/decoder", "aac")
                prop("audio-codec-name", "aac")
                prop("audio-params", {"samplerate": 48000, "channel-count": 2})
                prop("hwdec-current", "no")
                prop("current-vo", "null")
                prop("current-ao", "null")
                prop("container-fps", 25.0)
                prop("decoder-frame-drop-count", 4)
                prop("frame-drop-count", 2)
                prop("cache-buffering-state", 65.5)
                prop("avsync", -0.04)
                prop("audio-pts", -0.1)
                prop("demuxer-cache-state", {"seekable-ranges": [{"start": 0, "end": 7}, {"start": 10, "end": 20}],
                                            "cache-duration": 3.5, "fw-bytes": 123456, "raw-input-rate": 456789,
                                            "eof": False, "idle": False, "underrun": False,
                                            "ts-per-stream": [{"type": "video", "reader-pts": 3.5, "cache-end": 7, "cache-duration": 3.5},
                                                              {"type": "audio", "reader-pts": 3.4, "cache-end": 6.9, "cache-duration": 3.5}]})
                if mode == "invalid-metadata":
                    prop("current-tracks/video/decoder", "https://private.invalid/path?token=secret")
                    prop("current-tracks/audio/decoder", "private.invalid/path")
                    prop("current-tracks/video/codec-profile", "High\u0000hidden")
                    prop("frame-drop-count", -1)
                    prop("demuxer-cache-state", {"cache-duration": 1e300, "fw-bytes": "123", "raw-input-rate": -2,
                                                "ts-per-stream": [{"type": "video", "reader-pts": 1e300,
                                                                   "cache-end": -1e300, "cache-duration": 1e300}]})
                    prop("cache-buffering-state", 101)
                    prop("avsync", 1e300)
                    prop("audio-pts", -1e300)
                    prop("container-fps", 1e12)
                    prop("video-dec-params", None)
                    prop("core-idle", None)
                    prop("track-list", "unavailable")
                if mode == "stall-paused":
                    prop("core-idle", True)
                    prop("video-dec-params", None)
                    prop("audio-params", None)
                    prop("demuxer-cache-state", {"eof": False, "idle": False, "underrun": True,
                                                "fw-bytes": 0, "raw-input-rate": 0,
                                                "ts-per-stream": [{"type": "video", "reader-pts": 0.0}]})
                event("file-loaded")
                loaded = True
                if mode == "ignore-stop":
                    signal.signal(signal.SIGTERM, signal.SIG_IGN)
                    while True:
                        time.sleep(1)
                if mode == "eof":
                    event("end-file", reason="eof")
                elif mode == "end-error":
                    event("end-file", reason="error", file_error="http://private.invalid/do-not-log")
                elif mode in ("end-audio-error", "end-format-exit", "terminal-warnings"):
                    event("end-file", reason="error", file_error=("audio output initialization failed"
                          if mode == "end-audio-error" else "unrecognized file format"))
                    if mode == "end-format-exit":
                        sys.exit(19)
                    if mode == "terminal-warnings":
                        time.sleep(0.04)
                        event("log-message", prefix="ffmpeg", level="warn", text="http: HTTP error 403 private.invalid?token=secret")
                        event("log-message", prefix="vd", level="warn", text="Error while decoding frame!")
                        event("file-loaded")
                        event("playback-restart")
                        prop("video-dec-params", {"w": 999, "h": 999, "pixelformat": "fake"})
                        prop("time-pos", 9999)
                        event("event-queue-overflow")
                        event("log-message", prefix="overflow", level="fatal", text="log message buffer overflow: 99 messages skipped")
                elif mode == "warning-reasons":
                    for module, text in (
                        ("vd", "Error while decoding frame!"),
                        ("ffmpeg/video", "hevc: Could not find ref with POC 14"),
                        ("ffmpeg/video", "hevc: Error parsing NAL unit #2."),
                        ("cplayer", "Invalid video timestamp: 0 -> 500"),
                        ("cplayer", "Audio device underrun detected."),
                        ("ffmpeg", "http: HTTP error 503 private.invalid?token=secret"),
                        ("ffmpeg/demuxer", "hls: Failed to open an initialization section in playlist 1"),
                        ("ffmpeg/demuxer", "hls: Failed to open segment 4 of playlist 1"),
                        ("ffmpeg/demuxer", "hls: Failed to reload playlist 1"),
                        ("private.invalid/path", "secret token=never-retain"),
                        ("ffmpeg", "http: HTTP error 4030 invalid"),
                        ("ffmpeg", "HTTP error 403\u0000hidden"),
                        ("ffmpeg", "x" * 5000)):
                        event("log-message", prefix=module, level="warn", text=text)
                elif mode == "diagnostic-errors":
                    for module in ("ffmpeg/video", "ad", "vo/gpu", "ao/alsa", "demux", "stream",
                                   "http://private.invalid/do-not-log"):
                        event("log-message", prefix=module, level="error", text="private.invalid/path?token=secret")
                    event("log-message", prefix="vd", level="warn", text="not requested")
                elif mode.startswith("packet-"):
                    packet_lines = [
                        "append packet to video: size=1000 pts=0.000000 dts=-0.080000 pos=0 [num=1 size=1080]\n",
                        "append packet to video: size=1200 pts=0.040000 dts=-0.040000 pos=1 [num=>1 size=2400]\n",
                        "append packet to audio: size=100 pts=0.000000 dts=0.000000 pos=2 [num=1 size=180]\n",
                        "append packet to audio: size=110 pts=0.020000 dts=0.020000 pos=-1 [num=>1 size=390]\n",
                        "append packet to audio: size=120 pts=-9223372036854775808.000000 dts=-9223372036854775808.000000 pos=-1 [num=>1 size=510]\n",
                    ]
                    if mode in ("packet-video-only", "packet-late-audio", "packet-cutoff-budget", "packet-cutoff-overflow", "packet-after-marker"):
                        packet_lines = packet_lines[:2]
                    elif mode == "packet-audio-unknown":
                        packet_lines = packet_lines[:2] + packet_lines[-1:]
                    elif mode == "packet-none":
                        packet_lines = []
                    for text in packet_lines:
                        event("log-message", prefix="lavf", level="trace", text=text)
                    # A foreign module or source cannot inject queue evidence.
                    event("log-message", prefix="demux/private", level="trace",
                          text="append packet to video: size=1000 pts=0.000000 dts=0.000000 pos=0 [num=1 size=1080]\n")
                    if mode == "packet-malicious":
                        for text in (packet_lines[0].rstrip() + " secret=https://private.invalid/", 
                                     packet_lines[0].replace("0.000000", "nan"),
                                     packet_lines[0].replace("size=1000", "size=99999999999999999999"),
                                     packet_lines[0] + "\u0000hidden"):
                            event("log-message", prefix="lavf", level="trace", text=text)
                    if mode == "packet-warning-reasons":
                        for module, level, text in (
                            ("ffmpeg/demuxer", "warn", "mpegts: PES packet size mismatch"),
                            ("ffmpeg/demuxer", "warn", "mpegts: Packet corrupt (stream = 1, dts = 123)."),
                            ("demux/lavf", "warn", "error reading packet: Invalid data found when processing input."),
                            ("vo/gpu/drm", "error", "Can't open TTY for VT control: No such device"),
                            ("ffmpeg/demuxer", "warn", "https://private.invalid/unknown-warning")):
                            event("log-message", prefix=module, level=level, text=text)
            elif name == "request_log_messages":
                assert args[1] in ("warn", "terminal-default")
                reply(request, error="rejected" if mode == "packet-log-reject" and args[1] == "terminal-default" else "success")
            elif name == "set_property":
                if args[1] == "msg-level" and mode == "packet-restore-timeout":
                    continue
                reply(request, error="rejected" if args[1] == "msg-level" and mode == "packet-restore-error" else "success")
                if args[1] == "msg-level" and args[2] == "all=warn,cplayer=info":
                    if mode == "packet-cutoff-budget":
                        filler = json.dumps({"event": "log-message", "prefix": "lavf", "level": "trace",
                                             "text": "bounded fixture backlog " + "x" * 3000}).encode() + b"\n"
                        with send_lock:
                            sock.sendall(filler * 80)
                    if mode in ("packet-late-audio", "packet-cutoff-budget", "packet-cutoff-overflow"):
                        event("log-message", prefix="lavf", level="trace",
                              text="append packet to audio: size=256 pts=0.000000 dts=0.000000 pos=9 [num=1 size=336]\n")
                    if mode == "packet-cutoff-overflow":
                        event("event-queue-overflow")
                if args[1] == "pause":
                    prop("pause", True if mode == "stall-paused" else args[2])
                    if loaded:
                        event("playback-restart")
            elif name == "print-text":
                assert args[1] == "uxplay-packet-capture-boundary-v1"
                reply(request)
                if mode != "packet-marker-missing":
                    event("log-message", prefix="cplayer", level="info", text=args[1] + "\n")
                if mode == "packet-after-marker":
                    event("log-message", prefix="lavf", level="trace",
                          text="append packet to audio: size=256 pts=0.000000 dts=0.000000 pos=9 [num=1 size=336]\n")
            elif name == "seek":
                if mode == "seek-error":
                    reply(request, error="seek rejected http://private.invalid/do-not-log")
                else:
                    event("seek")
                    def complete_seek(saved=request, position=args[1]):
                        reply(saved)
                        prop("time-pos", position)
                        if mode != "seek-never-completes":
                            event("playback-restart")
                    if mode == "seek-delayed":
                        threading.Timer(0.08, complete_seek).start()
                    else:
                        complete_seek()
            elif name == "quit":
                if mode == "stop-warnings":
                    time.sleep(0.04)
                    event("end-file", reason="error", file_error="unrecognized file format")
                    event("log-message", prefix="ffmpeg", level="warn", text="http: HTTP error 403 private.invalid?token=secret")
                    event("log-message", prefix="vd", level="warn", text="Error while decoding frame!")
                    event("event-queue-overflow")
                # Late old-process updates must not affect a replacement.
                event("file-loaded")
                prop("duration", 9999)
                reply(request)
                time.sleep(0.02)
                sys.exit(0)
            elif name == "raw" and args[1] == "show-text":
                reply(request)
            else:
                reply(request, error="unsupported command")
except (BrokenPipeError, ConnectionResetError, socket.timeout):
    pass
finally:
    sock.close()
