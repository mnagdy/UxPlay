"""Host-only checks of qualification gates, evidence and recovery decisions."""
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock

FILE = Path(__file__).resolve().parents[1] / 'scripts' / 'pi-hevc-tests'
loader = importlib.machinery.SourceFileLoader('hevc_tests', str(FILE))
spec = importlib.util.spec_from_loader(loader.name, loader)
m = importlib.util.module_from_spec(spec)
loader.exec_module(m)
ROW = {'id': '2160p30-main', 'width': 3840, 'height': 2160, 'fps': 30,
       'duration': 20, 'frames': 600, 'bits': 8}


def samples():
    return [{'t': i / 2, 'p': {'time-pos': i / 2, 'audio-pts': i / 2,
             'avsync': .002, 'frame-drop-count': 0, 'decoder-frame-drop-count': 0,
             'video-dec-params': {'w': 3840, 'h': 2160}}} for i in range(40)]


def good():
    return {'samples': samples(), 'warnings': [], 'hardware_seen': True,
            'exit_code': 0, 'shutdown_method': 'quit'}


class HevcTests(unittest.TestCase):
    def test_primary_plane_experiment_keeps_osd_separate(self):
        args = m.mpv_args('overlay-primary', 'display', 77)
        self.assertIn('--drm-drmprime-video-plane=primary', args)
        self.assertIn('--drm-draw-plane=overlay', args)
        self.assertIn('--drm-draw-surface-size=1280x720', args)

    def test_ffmpeg_reference_uses_hardware_frames_and_pacing(self):
        for stage in ('smoke', 'display', 'benchmark'):
            args = m.ffmpeg_args('fixture.mp4', stage)
            self.assertIn('-no_cvt_hw', args)
            self.assertEqual(args[args.index('-hwaccel_output_format') + 1], 'drm_prime')
            self.assertEqual(args[args.index('-f') + 1], 'vout_drm')
            self.assertIn('-an', args)
            self.assertEqual('-readrate' in args, stage != 'benchmark')

    def test_ffmpeg_submission_requires_complete_hardware_output(self):
        result = {'stderr': 'Video: wrapped_avframe, 1 reference frame, drm_prime\n600 frames decoded; 0 decode errors;', 'frames': 600,
                  'progress_end': True, 'exit_code': 0, 'shutdown_method': 'quit', 'elapsed': 10}
        self.assertTrue(m.evaluate_ffmpeg(result.copy(), ROW, 'benchmark')['passed'])
        for change in ({'frames': 599}, {'stderr': 'Video: wrapped_avframe, yuv420p'},
                       {'progress_end': False}, {'exit_code': 1},
                       {'stderr': result['stderr'] + '\nFailed to commit atomic request'},
                       {'stderr': result['stderr'] + '\n10 decode errors;'},
                       {'after': {'throttled': 'throttled=0x80008'}}):
            self.assertFalse(m.evaluate_ffmpeg(result | change, ROW, 'benchmark')['passed'])
        self.assertFalse(m.evaluate_ffmpeg(result.copy(), ROW, 'benchmark')['audio_tested'])

    def test_short_soak_is_not_a_pass(self):
        self.assertFalse(m.evaluate(good() | {'elapsed': 20}, ROW, 'soak')['passed'])

    def test_ffmpeg_blocks_receiver_restore(self):
        with patch.object(m, 'command') as command:
            command.side_effect = [Mock(returncode=1), Mock(returncode=1), Mock(returncode=0)]
            self.assertFalse(m.no_players())

    def test_rejected_display_commits_fail_even_with_zero_drops(self):
        result = good() | {'warnings': [{'level': 'warn', 'prefix': 'vo/gpu',
                                       'text': 'Failed to commit atomic request: Error number 22 occurred\n'}]}
        self.assertFalse(m.evaluate(result, ROW, 'display')['passed'])
        self.assertIn('display driver rejected frame updates', result['failure_reasons'])
        result = good() | {'display_commit_failures': 1200}
        self.assertFalse(m.evaluate(result, ROW, 'display')['passed'])

    def test_ssh_terminal_notice_requires_exact_context_and_gpu_progress(self):
        notice = {'level': 'error', 'prefix': 'vo/gpu/drm',
                  'text': 'VT_GETMODE failed: Inappropriate ioctl for device\n'}
        companion = {'level': 'warn', 'prefix': 'vo/gpu/drm',
                     'text': 'Failed to set up VT switcher. Terminal switching will be unavailable.\n'}
        result = good() | {'warnings': [notice, companion]}
        for s in result['samples']: s['p']['current-vo'] = 'gpu'
        self.assertTrue(m.evaluate(result, ROW, 'smoke')['passed'])
        self.assertEqual(result['nonfatal_environment_messages'], [notice])
        self.assertEqual(len(result['warnings']), 2)
        for change in ({'prefix': 'vd'}, {'level': 'fatal'}, {'text': 'Failed to acquire DRM master'}):
            result['warnings'] = [notice | change, companion]
            self.assertFalse(m.evaluate(result, ROW, 'smoke')['passed'])
        result['warnings'] = [notice]
        self.assertFalse(m.evaluate(result, ROW, 'smoke')['passed'])
        result['warnings'] = [notice, companion, {'level': 'error', 'prefix': 'vd', 'text': 'decode failed'}]
        self.assertFalse(m.evaluate(result, ROW, 'smoke')['passed'])
        result['warnings'] = [notice, companion]
        for s in result['samples']: s['p']['time-pos'] = 0
        self.assertFalse(m.evaluate(result, ROW, 'smoke')['passed'])

    def test_hardware_paths_never_silently_fall_back(self):
        for path, (hwdec, interop) in m.PATHS.items():
            args = m.mpv_args(path, 'display', 77)
            self.assertIn('--hwdec=' + hwdec, args)
            self.assertIn('--gpu-hwdec-interop=' + interop, args)
            self.assertIn('--hwdec-software-fallback=no', args)
            self.assertIn('--hwdec-codecs=hevc', args)
            self.assertNotIn('--hwdec=auto', args)

    def test_benchmark_cannot_measure_hdmi(self):
        args = m.mpv_args('gpu', 'benchmark', 77)
        self.assertIn('--vo=null', args)
        self.assertIn('--framedrop=no', args)
        self.assertNotIn('--vo=gpu', args)

    def test_smoke_before_4k_and_seeks_after_display(self):
        self.assertEqual(m.prerequisite('display', ROW), ('smoke', 'all'))
        self.assertEqual(m.prerequisite('seek-keyframes', ROW), ('display', ROW['id']))
        self.assertEqual(m.prerequisite('seek-exact', ROW), ('seek-keyframes', ROW['id']))
        self.assertEqual(m.prerequisite('soak', ROW), ('display', ROW['id']))

    def test_old_environment_or_failed_run_cannot_unlock(self):
        gates = {'x': {'passed': True, 'fingerprint': 'old'}}
        self.assertFalse(m.gate_ok(gates, 'x', 'new'))
        self.assertTrue(m.gate_ok(gates, 'x', 'old'))
        gates['x']['passed'] = False
        self.assertFalse(m.gate_ok(gates, 'x', 'old'))

    def test_metrics_do_not_claim_visual_confirmation(self):
        result = m.evaluate(good(), ROW, 'display')
        self.assertTrue(result['passed'])
        self.assertIn('pending', result['visual_confirmation'])

    def test_missing_hardware_audio_or_resolution_fails(self):
        for field in ('hardware', 'audio', 'resolution'):
            result = good()
            if field == 'hardware': result['hardware_seen'] = False
            else:
                for s in result['samples']:
                    s['p'].pop('audio-pts' if field == 'audio' else 'video-dec-params')
            self.assertFalse(m.evaluate(result, ROW, 'display')['passed'])

    def test_missing_drop_telemetry_fails(self):
        result = good()
        for s in result['samples']: s['p'].pop('frame-drop-count')
        self.assertFalse(m.evaluate(result, ROW, 'display')['passed'])

    def test_high_drops_and_av_error_fail(self):
        result = good()
        for i, s in enumerate(result['samples']):
            s['p']['frame-drop-count'] = i * 5
            s['p']['avsync'] = .5
        self.assertFalse(m.evaluate(result, ROW, 'display')['passed'])
        self.assertGreater(result['metrics']['drop_fraction'], .1)

    def test_seek_and_loop_jumps_not_counted_as_performance(self):
        data = samples()
        for s in data[20:]: s['p']['time-pos'] -= 8
        metrics = m.measure(data, 30)
        self.assertAlmostEqual(metrics['progress_ratio'], 1)
        self.assertEqual(metrics['measured_seconds'], 16)

    def test_benchmark_requires_full_eof_and_realtime(self):
        result = good()
        self.assertFalse(m.evaluate(result, ROW, 'benchmark')['passed'])
        result = good() | {'end_reason': 'eof', 'eof_after_unpause': 10}
        self.assertTrue(m.evaluate(result, ROW, 'benchmark')['passed'])
        self.assertEqual(result['decode_fps_including_startup'], 60)

    def test_seek_requires_observed_recovery_and_pause(self):
        result = good()
        self.assertFalse(m.evaluate(result, ROW, 'seek-exact')['passed'])

    def test_kernel_wait_is_unreaped_not_a_success(self):
        proc = Mock()
        proc.poll.return_value = None
        proc.wait.side_effect = subprocess.TimeoutExpired('mpv', 1)
        self.assertEqual(m.terminate(proc, Mock()), ('unreaped', None))
        result = good() | {'shutdown_method': 'unreaped', 'exit_code': None}
        self.assertFalse(m.evaluate(result, ROW, 'smoke')['passed'])

    def test_clean_quit_does_not_send_signals(self):
        proc = Mock()
        proc.poll.side_effect = [None, 0]
        self.assertEqual(m.terminate(proc, Mock()), ('quit', 0))
        proc.terminate.assert_not_called()
        proc.kill.assert_not_called()

    def test_never_restore_over_live_or_wedged_player(self):
        with patch.object(m, 'no_players', return_value=False), patch.object(m, 'command') as command:
            self.assertFalse(m.restore_receiver(True))
            command.assert_not_called()

    def test_restoration_verified_after_start(self):
        with patch.object(m, 'no_players', return_value=True), patch.object(m, 'command') as command:
            command.return_value.stdout = 'active\n'
            self.assertTrue(m.restore_receiver(True))
            self.assertEqual(command.call_args_list[0].args[0], ['sudo', '-n', 'systemctl', 'start', 'uxplay.service'])
            command.return_value.stdout = 'failed\n'
            self.assertFalse(m.restore_receiver(True))

    def test_fixture_path_cannot_escape_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            (root / 'file').write_text('generated')
            (root / 'link').symlink_to(root / 'file')
            self.assertEqual(m.checked_file(root, 'file'), root / 'file')
            for bad in ('../file', str(root / 'file'), 'link'):
                with self.assertRaises(ValueError): m.checked_file(root, bad)

    def test_current_throttling_fails_but_historical_flag_does_not(self):
        result = good() | {'before': {'throttled': 'throttled=0x80000'}}
        self.assertTrue(m.evaluate(result, ROW, 'display')['passed'])
        result = good() | {'health': [{'throttled': 'throttled=0x80008'}]}
        self.assertFalse(m.evaluate(result, ROW, 'display')['passed'])

    def test_stage_stops_after_failure_and_restores_receiver(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            rows = [ROW | {'id': '720p25-main', 'height': 720}]
            failed = good() | {'passed': False, 'failure_reasons': ['test failure']}
            with patch.object(sys, 'argv', ['pi-hevc-tests', '--fixtures', str(root), '--run']), \
                 patch.object(sys, 'platform', 'linux'), patch.object(m.os, 'getuid', return_value=1000), \
                 patch.object(m.Path, 'home', return_value=root), patch.object(m, 'fixtures', return_value=(rows, 'hash')), \
                 patch.object(m, 'environment', return_value={'hdmi': 'Crtc 1 1280x720@60', 'boot': 'test'}), \
                 patch.object(m, 'command') as cmd, patch.object(m.subprocess, 'run'), \
                 patch.object(m.signal, 'signal'), patch.object(m, 'no_players', return_value=True), \
                 patch.object(m, 'restore_receiver', return_value=True) as restore, \
                 patch.object(m, 'trial', return_value=failed) as trial:
                # Select the generated test ID; fail the first cycle of three.
                sys.argv += ['--fixture', '720p25-main', '--results', str(root / 'results')]
                cmd.return_value.stdout = 'active\n'
                self.assertEqual(m.main(), 1)
                self.assertEqual(trial.call_count, 1)
                restore.assert_called_once_with(True)
                gates = json.loads((root / 'results/gates.json').read_text())
                self.assertFalse(gates['smoke-all-gpu']['passed'])

    @unittest.skipUnless(sys.platform == 'linux', 'Linux parent-death launcher')
    def test_real_child_ipc_and_clean_teardown(self):
        fake = '''import socket, sys, json, select, time
s=socket.socket(fileno=int(sys.argv[1])); buf=b''; started=None; tick=-1
def emit(x): s.sendall((json.dumps(x)+'\\n').encode())
while True:
 ready,_,_=select.select([s],[],[],.02)
 if ready:
  chunk=s.recv(65536)
  if not chunk: break
  buf+=chunk
  while b'\\n' in buf:
   line,buf=buf.split(b'\\n',1); cmd=json.loads(line)['command']
   if cmd[0]=='quit': sys.exit(0)
   if cmd[0]=='loadfile': emit({'event':'file-loaded'})
   if cmd[:2]==['set_property','pause'] and cmd[2] is False: started=time.monotonic()
 if started:
  age=time.monotonic()-started
  if int(age*2)!=tick:
   tick=int(age*2)
   for name,value in {'time-pos':age,'audio-pts':age,'pause':False,'avsync':0,'hwdec-current':'drm','frame-drop-count':0,'decoder-frame-drop-count':0,'video-dec-params':{'w':3840,'h':2160}}.items():
    emit({'event':'property-change','name':name,'data':value})
  if age>3.6: emit({'event':'end-file','reason':'eof'}); started=None
'''
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / 'fake.py'; script.write_text(fake)
            with patch.object(m, 'mpv_args', side_effect=lambda p,s,fd: [sys.executable, str(script), str(fd)]), \
                 patch.object(m, 'telemetry', return_value={}):
                result = m.trial('/generated.mp4', ROW, 'gpu', 'smoke', 6)
            self.assertTrue(result['passed'], result['failure_reasons'])
            self.assertEqual(result['shutdown_method'], 'quit')
            self.assertEqual(result['exit_code'], 0)


if __name__ == '__main__': unittest.main()
