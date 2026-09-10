#!/usr/bin/env python3
"""Synthetic live fMP4 HLS fixtures with declared but intermittently absent AAC.

All media is FFmpeg test video and a generated tone. Replacing an audio ``traf``
with a same-size ``free`` box removes its samples without changing any video
sample offset, its payload, the audio track declaration, or later timestamps.
The unused AAC bytes remain padding in ``mdat``; they cannot be demuxed because
there is no sample table referring to them.
"""

from dataclasses import dataclass
import json
from pathlib import Path
import struct
import subprocess


SEGMENT_SECONDS = 2
SEGMENT_COUNT = 8
SCENARIOS = {
    "normal": frozenset(),
    "absent": frozenset(range(SEGMENT_COUNT)),
    "late": frozenset((0, 1)),
    "late-delayed": frozenset((0, 1, 2)),
    "middle": frozenset((2, 3)),
}
STARTUP_SCENARIOS = ("startup-normal", "startup-absent", "startup-multimoof",
                     "startup-single-absent", "startup-single-multimoof")
STARTUP_SEGMENT_SECONDS = 10
STARTUP_SEGMENT_COUNT = 3
BUFFERING_SCENARIOS = ("buffering-default", "buffering-direct")
BUFFERING_SEGMENT_SECONDS = 6
BUFFERING_SEGMENT_COUNT = 3


@dataclass(frozen=True)
class Box:
    kind: bytes
    start: int
    end: int
    payload: int


def boxes(data, start=0, end=None):
    """Read bounded ISO BMFF boxes, rejecting truncation and invalid sizes."""
    end = len(data) if end is None else end
    while start < end:
        if end - start < 8:
            raise ValueError("Truncated MP4 box header")
        size, kind = struct.unpack_from(">I4s", data, start)
        header = 8
        if size == 1:
            if end - start < 16:
                raise ValueError("Truncated extended MP4 box header")
            size = struct.unpack_from(">Q", data, start + 8)[0]
            header = 16
        elif size == 0:
            size = end - start
        if size < header or size > end - start:
            raise ValueError("Invalid MP4 box size")
        yield Box(kind, start, start + size, start + header)
        start += size


def child(data, parent, kind):
    found = [box for box in boxes(data, parent.payload, parent.end)
             if box.kind == kind]
    if len(found) != 1:
        raise ValueError(f"Expected one {kind!r} box, found {len(found)}")
    return found[0]


def initialization_tracks(data):
    moov = next(box for box in boxes(data) if box.kind == b"moov")
    tracks = {}
    for trak in boxes(data, moov.payload, moov.end):
        if trak.kind != b"trak":
            continue
        tkhd = child(data, trak, b"tkhd")
        version = data[tkhd.payload]
        track_id = struct.unpack_from(">I", data, tkhd.payload + (20 if version else 12))[0]
        mdia = child(data, trak, b"mdia")
        hdlr = child(data, mdia, b"hdlr")
        handler = data[hdlr.payload + 8:hdlr.payload + 12].decode("ascii")
        stbl = child(data, child(data, mdia, b"minf"), b"stbl")
        stsd = child(data, stbl, b"stsd")
        entries = list(boxes(data, stsd.payload + 8, stsd.end))
        if len(entries) != 1:
            raise ValueError("Fixture requires exactly one codec per track")
        tracks[track_id] = {"handler": handler, "codec": entries[0].kind.decode("ascii")}
    if sorted(track["handler"] for track in tracks.values()) != ["soun", "vide"]:
        raise ValueError("Fixture must declare exactly one audio and one video track")
    if sorted(track["codec"] for track in tracks.values()) != ["avc1", "mp4a"]:
        raise ValueError("Fixture must declare H264 video and AAC audio")
    return tracks


def fragment_tracks(data):
    """Return sample counts, timestamps and byte spans from FFmpeg fragments."""
    result = {}
    for moof in boxes(data):
        if moof.kind != b"moof":
            continue
        for traf in boxes(data, moof.payload, moof.end):
            if traf.kind != b"traf":
                continue
            tfhd = child(data, traf, b"tfhd")
            flags = struct.unpack_from(">I", data, tfhd.payload)[0] & 0xFFFFFF
            track_id = struct.unpack_from(">I", data, tfhd.payload + 4)[0]
            cursor = tfhd.payload + 8
            base_offset = moof.start
            if flags & 1:
                base_offset = struct.unpack_from(">Q", data, cursor)[0]
                cursor += 8
            elif not flags & 0x020000:
                raise ValueError("Fixture requires explicit or moof-relative data offset")
            if flags & 2:
                cursor += 4
            default_duration = default_size = 0
            if flags & 8:
                default_duration = struct.unpack_from(">I", data, cursor)[0]
                cursor += 4
            if flags & 16:
                default_size = struct.unpack_from(">I", data, cursor)[0]
                cursor += 4
            tfdt = child(data, traf, b"tfdt")
            decode_time = struct.unpack_from(">Q" if data[tfdt.payload] else ">I",
                                             data, tfdt.payload + 4)[0]
            record = {"samples": 0, "decode_time": decode_time,
                      "duration": 0, "spans": [], "traf": traf}
            data_end = None
            for trun in boxes(data, traf.payload, traf.end):
                if trun.kind != b"trun":
                    continue
                flags = struct.unpack_from(">I", data, trun.payload)[0] & 0xFFFFFF
                count = struct.unpack_from(">I", data, trun.payload + 4)[0]
                cursor = trun.payload + 8
                if flags & 1:
                    data_end = base_offset + struct.unpack_from(">i", data, cursor)[0]
                    cursor += 4
                if flags & 4:
                    cursor += 4
                if data_end is None:
                    raise ValueError("Fixture's first sample run must carry a data offset")
                for _ in range(count):
                    duration, size = default_duration, default_size
                    if flags & 0x100:
                        duration = struct.unpack_from(">I", data, cursor)[0]
                        cursor += 4
                    if flags & 0x200:
                        size = struct.unpack_from(">I", data, cursor)[0]
                        cursor += 4
                    if flags & 0x400:
                        cursor += 4
                    if flags & 0x800:
                        cursor += 4
                    if size <= 0 or not 0 <= data_end <= len(data) - size:
                        raise ValueError("Invalid fixture sample byte span")
                    record["spans"].append((data_end, data_end + size))
                    data_end += size
                    record["samples"] += 1
                    record["duration"] += duration
                if cursor != trun.end:
                    raise ValueError("Unexpected fixture sample table layout")
            if track_id in result:
                raise ValueError("Fixture requires one fragment per track per segment")
            result[track_id] = record
    return result


def omit_audio(data, audio_id):
    record = fragment_tracks(data)[audio_id]
    traf = record["traf"]
    rewritten = bytearray(data)
    rewritten[traf.start + 4:traf.start + 8] = b"free"
    return bytes(rewritten)


def playlist(visible=SEGMENT_COUNT, segment_seconds=SEGMENT_SECONDS):
    """An open live playlist, intentionally without ENDLIST or an EOS shortcut."""
    lines = ["#EXTM3U", "#EXT-X-VERSION:7", f"#EXT-X-TARGETDURATION:{segment_seconds}",
             "#EXT-X-MEDIA-SEQUENCE:0", '#EXT-X-MAP:URI="init.mp4"']
    for index in range(visible):
        lines.extend((f"#EXTINF:{segment_seconds:.6f},", f"fragment-{index:03d}.m4s"))
    return "\n".join(lines) + "\n"


def validate_fixture(directory):
    directory = Path(directory)
    tracks = initialization_tracks((directory / "normal" / "init.mp4").read_bytes())
    audio_id = next(key for key, value in tracks.items() if value["handler"] == "soun")
    video_id = next(key for key, value in tracks.items() if value["handler"] == "vide")
    summaries = {}
    for scenario, missing in SCENARIOS.items():
        location = directory / scenario
        if initialization_tracks((location / "init.mp4").read_bytes()) != tracks:
            raise AssertionError("Audio track declaration changed between scenarios")
        counts = []
        for index in range(SEGMENT_COUNT):
            name = f"fragment-{index:03d}.m4s"
            original = (directory / "normal" / name).read_bytes()
            candidate = (location / name).read_bytes()
            source = fragment_tracks(original)
            actual = fragment_tracks(candidate)
            if source[video_id] != actual[video_id]:
                raise AssertionError("Video sample table changed")
            for start, end in source[video_id]["spans"]:
                if original[start:end] != candidate[start:end]:
                    raise AssertionError("Video payload changed")
            # Ordinary AV deliberately has ALL video data before audio data in
            # the same fragment. A repair must wait for the fragment boundary.
            if max(end for _, end in source[video_id]["spans"]) > min(
                    start for start, _ in source[audio_id]["spans"]):
                raise AssertionError("Fixture must put video before audio in each fragment")
            if index in missing:
                if audio_id in actual:
                    raise AssertionError("Missing-audio fragment still contains audio samples")
            elif source[audio_id] != actual[audio_id] or original != candidate:
                raise AssertionError("Present audio data or timing changed")
            counts.append({"video": actual[video_id]["samples"],
                           "audio": actual.get(audio_id, {}).get("samples", 0)})
        summaries[scenario] = counts
    return {"segment_seconds": SEGMENT_SECONDS, "segment_count": SEGMENT_COUNT,
            "tracks": tracks, "scenarios": summaries,
            "startup": validate_startup_fixture(directory),
            "buffering": validate_buffering_fixture(directory)}


def generate_media(location, ffmpeg, segment_seconds, segment_count):
    location.mkdir(parents=True, exist_ok=True)
    keyframe_interval = str(segment_seconds * 10)
    subprocess.run(
        [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
         "-i", "testsrc2=size=160x90:rate=10", "-f", "lavfi", "-i",
         "sine=frequency=440:sample_rate=48000", "-t", str(segment_seconds * segment_count),
         "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-preset", "ultrafast",
         "-pix_fmt", "yuv420p", "-g", keyframe_interval, "-keyint_min", keyframe_interval, "-sc_threshold", "0",
         "-bf", "0", "-c:a", "aac", "-b:a", "48k", "-ac", "2", "-fflags", "+bitexact",
         "-flags:v", "+bitexact", "-flags:a", "+bitexact", "-map_metadata", "-1",
         "-f", "hls", "-hls_time", str(segment_seconds), "-hls_list_size", "0",
         "-hls_segment_type", "fmp4", "-hls_fmp4_init_filename", "init.mp4",
         "-hls_segment_filename", str(location / "fragment-%03d.m4s"),
         str(location / "stream.m3u8")], check=True, timeout=60,
    )


def startup_moofs(data):
    """Split an HLS segment into moof/mdat chunks with unchanged relative offsets."""
    pending = None
    for box in boxes(data):
        if box.kind == b"moof":
            if pending is not None:
                raise ValueError("Fixture has a moof without its mdat")
            pending = box.start
        elif box.kind == b"mdat":
            if pending is None:
                raise ValueError("Fixture has an mdat without its moof")
            yield data[pending:box.end]
            pending = None
    if pending is not None:
        raise ValueError("Fixture's final moof has no mdat")


def make_startup_fixture(directory, ffmpeg):
    normal = directory / "startup-normal"
    generate_media(normal, ffmpeg, STARTUP_SEGMENT_SECONDS, STARTUP_SEGMENT_COUNT)
    init = (normal / "init.mp4").read_bytes()
    tracks = initialization_tracks(init)
    audio_id = next(key for key, value in tracks.items() if value["handler"] == "soun")
    absent = directory / "startup-absent"
    absent.mkdir(exist_ok=True)
    (absent / "init.mp4").write_bytes(init)
    for index in range(STARTUP_SEGMENT_COUNT):
        name = f"fragment-{index:03d}.m4s"
        (absent / name).write_bytes(omit_audio((normal / name).read_bytes(), audio_id))
    # One HLS segment may contain several moof/mdat pairs. Preserve every AAC
    # sample in this case; only the HTTP fragment boundary proves completion.
    chunks = directory / "startup-chunks"
    generate_media(chunks, ffmpeg, 2, STARTUP_SEGMENT_COUNT * 5)
    multiple = directory / "startup-multimoof"
    multiple.mkdir(exist_ok=True)
    (multiple / "init.mp4").write_bytes((chunks / "init.mp4").read_bytes())
    for index in range(STARTUP_SEGMENT_COUNT):
        parts = [(chunks / f"fragment-{part:03d}.m4s").read_bytes()
                 for part in range(index * 5, (index + 1) * 5)]
        styp = next(box for box in boxes(parts[0]) if box.kind == b"styp")
        body = parts[0][styp.start:styp.end]
        body += b"".join(chunk for part in parts for chunk in startup_moofs(part))
        (multiple / f"fragment-{index:03d}.m4s").write_bytes(body)
    single_absent = directory / "startup-single-absent"
    single_multiple = directory / "startup-single-multimoof"
    single_absent.mkdir(exist_ok=True)
    single_multiple.mkdir(exist_ok=True)
    (single_absent / "init.mp4").write_bytes(init)
    (single_multiple / "init.mp4").write_bytes((multiple / "init.mp4").read_bytes())
    for index in range(STARTUP_SEGMENT_COUNT):
        name = f"fragment-{index:03d}.m4s"
        (single_absent / name).write_bytes((absent / name).read_bytes())
        data = (multiple / name).read_bytes()
        styp = next(box for box in boxes(data) if box.kind == b"styp")
        parts = list(startup_moofs(data))
        # The first four moofs have no AAC samples, but the last moof in this
        # same HTTP body has valid AAC. Its final audio bytes will be delayed by
        # the server, exposing premature completion decisions at any early mdat.
        body = data[styp.start:styp.end]
        body += b"".join(omit_audio(part, audio_id) for part in parts[:-1]) + parts[-1]
        (single_multiple / name).write_bytes(body)
    for scenario in STARTUP_SCENARIOS:
        (directory / scenario / "stream.m3u8").write_text(
            playlist(STARTUP_SEGMENT_COUNT, STARTUP_SEGMENT_SECONDS), encoding="utf-8")


def validate_startup_fixture(directory):
    directory = Path(directory)
    summaries = {}
    for scenario in STARTUP_SCENARIOS:
        location = directory / scenario
        tracks = initialization_tracks((location / "init.mp4").read_bytes())
        audio_id = next(key for key, value in tracks.items() if value["handler"] == "soun")
        video_id = next(key for key, value in tracks.items() if value["handler"] == "vide")
        segments = []
        for index in range(STARTUP_SEGMENT_COUNT):
            name = f"fragment-{index:03d}.m4s"
            data = (location / name).read_bytes()
            chunks = list(startup_moofs(data))
            expected_chunks = 5 if scenario.endswith("multimoof") else 1
            if len(chunks) != expected_chunks:
                raise AssertionError("Unexpected number of moof/mdat pairs per HLS segment")
            samples = [fragment_tracks(chunk) for chunk in chunks]
            video = sum(item[video_id]["samples"] for item in samples)
            audio = sum(item.get(audio_id, {}).get("samples", 0) for item in samples)
            if video != 100 or (audio == 0) != scenario.endswith("absent"):
                raise AssertionError("Startup fixture must contain 100 video frames and the expected audio")
            if scenario.endswith("absent"):
                original = (directory / "startup-normal" / name).read_bytes()
                source = fragment_tracks(original)[video_id]
                if source != fragment_tracks(data)[video_id]:
                    raise AssertionError("Startup video sample table changed when omitting audio")
                for start, end in source["spans"]:
                    if original[start:end] != data[start:end]:
                        raise AssertionError("Startup video payload changed")
            if scenario == "startup-single-multimoof":
                if any(audio_id in sample for sample in samples[:-1]) or audio_id not in samples[-1]:
                    raise AssertionError("Only the final moof may carry actual AAC samples")
                original_chunks = list(startup_moofs((directory / "startup-multimoof" / name).read_bytes()))
                for original, candidate in zip(original_chunks, chunks):
                    original_video = fragment_tracks(original)[video_id]
                    if original_video != fragment_tracks(candidate)[video_id]:
                        raise AssertionError("Video changed while constructing delayed AAC control")
                    for start, end in original_video["spans"]:
                        if original[start:end] != candidate[start:end]:
                            raise AssertionError("Video bytes changed in delayed AAC control")
            segments.append({"moofs": len(chunks), "video": video, "audio": audio})
        summaries[scenario] = segments
    return {"segment_seconds": STARTUP_SEGMENT_SECONDS,
            "segment_count": STARTUP_SEGMENT_COUNT, "scenarios": summaries}


def make_buffering_fixture(directory, ffmpeg):
    baseline = directory / "buffering-default"
    direct = directory / "buffering-direct"
    generate_media(baseline, ffmpeg, BUFFERING_SEGMENT_SECONDS, BUFFERING_SEGMENT_COUNT)
    direct.mkdir(exist_ok=True)
    (direct / "init.mp4").write_bytes((baseline / "init.mp4").read_bytes())
    for index in range(BUFFERING_SEGMENT_COUNT):
        name = f"fragment-{index:03d}.m4s"
        (direct / name).write_bytes((baseline / name).read_bytes())
    for scenario in BUFFERING_SCENARIOS:
        (directory / scenario / "stream.m3u8").write_text(
            playlist(BUFFERING_SEGMENT_COUNT, BUFFERING_SEGMENT_SECONDS), encoding="utf-8")


def validate_buffering_fixture(directory):
    directory = Path(directory)
    baseline = directory / "buffering-default"
    direct = directory / "buffering-direct"
    init = (baseline / "init.mp4").read_bytes()
    if init != (direct / "init.mp4").read_bytes():
        raise AssertionError("Buffering profile comparisons must use identical initialization")
    tracks = initialization_tracks(init)
    audio_id = next(key for key, value in tracks.items() if value["handler"] == "soun")
    video_id = next(key for key, value in tracks.items() if value["handler"] == "vide")
    segments = []
    for index in range(BUFFERING_SEGMENT_COUNT):
        name = f"fragment-{index:03d}.m4s"
        data = (baseline / name).read_bytes()
        if data != (direct / name).read_bytes():
            raise AssertionError("Buffering profile comparisons must use identical media")
        samples = fragment_tracks(data)
        if samples[video_id]["samples"] != 60 or samples[audio_id]["samples"] < 250:
            raise AssertionError("Buffering fixture must contain six seconds of real video and AAC")
        segments.append({"video": samples[video_id]["samples"], "audio": samples[audio_id]["samples"]})
    return {"segment_seconds": BUFFERING_SEGMENT_SECONDS,
            "segment_count": BUFFERING_SEGMENT_COUNT, "segments": segments}


def make_fixture(directory, ffmpeg):
    directory = Path(directory)
    normal = directory / "normal"
    generate_media(normal, ffmpeg, SEGMENT_SECONDS, SEGMENT_COUNT)
    init = (normal / "init.mp4").read_bytes()
    tracks = initialization_tracks(init)
    audio_id = next(key for key, value in tracks.items() if value["handler"] == "soun")
    for scenario, missing in SCENARIOS.items():
        location = directory / scenario
        location.mkdir(exist_ok=True)
        (location / "init.mp4").write_bytes(init)
        (location / "stream.m3u8").write_text(playlist(), encoding="utf-8")
        for index in range(SEGMENT_COUNT):
            name = f"fragment-{index:03d}.m4s"
            data = (normal / name).read_bytes()
            (location / name).write_bytes(omit_audio(data, audio_id) if index in missing else data)
    make_startup_fixture(directory, ffmpeg)
    make_buffering_fixture(directory, ffmpeg)
    summary = validate_fixture(directory)
    (directory / "fixture.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary
