#!/usr/bin/env python3
"""Generate and fully decode-check synthetic HEVC/AAC qualification fixtures.

Run on a development machine, not the Pi under test. Requires ffmpeg/libx265.
Writes no credentials or third-party media. Existing output files are rejected.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess

MATRIX = [
    ('720p25-main', 1280, 720, 25, 8, 3),
    ('720p25-main10', 1280, 720, 25, 10, 3),
    ('1080p30-main', 1920, 1080, 30, 8, 5),
    ('2160p24-main', 3840, 2160, 24, 8, 10),
    ('2160p30-main', 3840, 2160, 30, 8, 12),
    ('2160p60-main', 3840, 2160, 60, 8, 20),
    ('2160p30-main10', 3840, 2160, 30, 10, 12),
    ('2160p60-main10', 3840, 2160, 60, 10, 20),
]


def run(args, timeout=900):
    p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if p.returncode:
        raise RuntimeError(f'{args[0]} failed ({p.returncode}): {p.stderr[-2000:]}')
    return p.stdout, p.stderr


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()


def verify(path, width, height, fps, bits, duration):
    # -count_frames invokes the software decoders over the entire fixture.
    out, err = run(['ffprobe', '-v', 'error', '-count_frames', '-count_packets',
                    '-show_streams', '-show_format', '-of', 'json', str(path)])
    if err.strip(): raise RuntimeError('Decode validation emitted errors: ' + path.name)
    info = json.loads(out)
    videos = [s for s in info['streams'] if s['codec_type'] == 'video']
    audios = [s for s in info['streams'] if s['codec_type'] == 'audio']
    assert len(videos) == len(audios) == 1
    v, a = videos[0], audios[0]
    assert v['codec_name'] == 'hevc' and v['width'] == width and v['height'] == height
    assert v['pix_fmt'] == ('yuv420p' if bits == 8 else 'yuv420p10le')
    assert v['profile'] == ('Main' if bits == 8 else 'Main 10')
    assert v['r_frame_rate'] == f'{fps}/1' and int(v['nb_read_frames']) == fps * duration
    assert int(v['nb_read_packets']) == fps * duration
    assert a['codec_name'] == 'aac' and int(a['sample_rate']) == 48000 and a['channels'] == 2
    assert int(a['nb_read_frames']) > 0 and int(a['nb_read_packets']) > 0
    assert v['color_transfer'] == 'bt709' and v['color_primaries'] == 'bt709'
    return {k: v[k] for k in ('codec_name', 'profile', 'pix_fmt', 'width', 'height',
                              'r_frame_rate', 'nb_read_frames', 'nb_read_packets',
                              'color_transfer', 'color_primaries')} | {
        'audio_codec': a['codec_name'], 'audio_rate': 48000, 'audio_channels': 2,
        'audio_packets': int(a['nb_read_packets']), 'duration': float(info['format']['duration'])}


def generate(root, entry, duration):
    name, width, height, fps, bits, mbps = entry
    folder = root / name
    folder.mkdir()
    path = folder / 'video.mp4'
    pixel = 'yuv420p' if bits == 8 else 'yuv420p10le'
    print('Encoding ' + name, flush=True)
    label = f'HEVC {width}x{height} {fps} fps {bits}-bit SDR'
    filters = ("drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf:"
               f"text='{label}':x=40:y=40:fontsize={max(28, height//30)}:"
               f"fontcolor=white:box=1:boxcolor=black@0.7,format={pixel}")
    run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-n',
         '-f', 'lavfi', '-i', f'testsrc2=size={width}x{height}:rate={fps}',
         '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
         '-t', str(duration), '-vf', filters, '-c:v', 'libx265', '-preset', 'ultrafast',
         '-x265-params', f'pools=4:frame-threads=2:keyint={fps*2}:min-keyint={fps*2}:scenecut=0:bframes=3:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited:log-level=error',
         '-pix_fmt', pixel, '-b:v', f'{mbps}M', '-maxrate', f'{mbps+2}M', '-bufsize', f'{2*(mbps+2)}M',
         '-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709', '-color_range', 'tv',
         '-tag:v', 'hvc1', '-c:a', 'aac', '-ac', '2', '-b:a', '128k', '-af', 'volume=0.25',
         '-movflags', '+faststart', str(path)])
    checked = verify(path, width, height, fps, bits, duration)
    # Container comparisons reuse identical compressed video and audio.
    common = ['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-n', '-i', str(path), '-c', 'copy']
    run(common + ['-f', 'mpegts', str(folder/'video.ts')])
    for kind in ('ts', 'fmp4'):
        dest = folder / ('hls-' + kind); dest.mkdir()
        options = ['-hls_time', '2', '-hls_playlist_type', 'vod', '-hls_list_size', '0']
        if kind == 'fmp4': options += ['-hls_segment_type', 'fmp4', '-hls_fmp4_init_filename', 'init.mp4']
        suffix = 'ts' if kind == 'ts' else 'm4s'
        run(common + options + ['-hls_segment_filename', str(dest/f'%03d.{suffix}'), str(dest/'index.m3u8')])
    return inspect_fixture(root, entry, duration, checked)


def inspect_fixture(root, entry, duration, checked=None):
    name, width, height, fps, bits, mbps = entry
    folder = root / name
    if checked is None: checked = verify(folder / 'video.mp4', width, height, fps, bits, duration)
    for variant in ('video.ts', 'hls-ts/index.m3u8', 'hls-fmp4/index.m3u8'):
        # Full decode once is established by MP4. Verify both tracks survive remux.
        out, err = run(['ffprobe', '-v', 'error', '-show_entries', 'stream=codec_name,codec_type', '-of', 'json', str(folder/variant)])
        if err.strip(): raise RuntimeError('Container validation error: ' + name + '/' + variant)
        assert {(s['codec_type'], s['codec_name']) for s in json.loads(out)['streams']} == {('video','hevc'),('audio','aac')}
    print('Validated ' + name, flush=True)
    return {'id': name, 'width': width, 'height': height, 'fps': fps, 'bits': bits,
            'target_mbps': mbps, 'duration': duration, 'frames': fps * duration,
            'validation': checked,
            'files': {str(p.relative_to(folder)): {'sha256': digest(p), 'bytes': p.stat().st_size}
                      for p in sorted(folder.rglob('*')) if p.is_file()}}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--duration', type=int, default=20)
    p.add_argument('--jobs', type=int, default=1, choices=(1, 2))
    p.add_argument('--verify-existing', action='store_true', help='Validate an existing complete matrix and write its manifest')
    args = p.parse_args()
    if not 15 <= args.duration <= 60: p.error('Duration must be 15–60 seconds')
    root = args.output.resolve()
    if not args.verify_existing and root.exists() and any(root.iterdir()): p.error('Use a new empty output directory')
    root.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        operation = inspect_fixture if args.verify_existing else generate
        fixtures = list(pool.map(lambda row: operation(root, row, args.duration), MATRIX))
    version, _ = run(['ffmpeg', '-version'])
    manifest = {'schema': 'uxplay-hevc-fixtures-v1', 'source': 'generated testsrc2 and quiet sine',
                'transfer': 'SDR BT.709; 10-bit does not imply HDR', 'ffmpeg': version.splitlines()[0],
                'fixtures': fixtures}
    (root/'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('All eight fixtures verified; manifest complete.', flush=True)


if __name__ == '__main__': main()
