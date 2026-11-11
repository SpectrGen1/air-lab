"""Verify persistent supervisor records with disposable subprocesses."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent


class NativeEventsTests(unittest.TestCase):
    def test_supervisor_records_signal_and_preserves_session_after_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            runtime = directory / 'runtime'
            runtime.mkdir(mode=0o700)
            config_dir = directory / '.local'
            config_dir.mkdir()
            config = {
                'labwc': '/bin/sleep', 'presenter': '/bin/true',
                'tracker': '/bin/sleep', 'tracker_launcher': str(directory / 'tracker.py'),
                'output': 'disposable',
            }
            (config_dir / 'native-install.json').write_text(json.dumps(config))
            (directory / 'tracker.py').write_text('import time\ntime.sleep(30)\n')
            event_path = directory / 'state/air-lab/events/latest.jsonl'
            env = dict(os.environ, AIR_LAB_ROOT=str(directory), XDG_RUNTIME_DIR=str(runtime),
                       XDG_STATE_HOME=str(directory / 'state'),
                       AIR_SESSION_LOG=str(directory / 'session.log'))
            env.pop('AIR_USE_STOCK', None)
            env.pop('WAYLAND_DISPLAY', None)
            env.pop('WAYLAND_SOCKET', None)
            with subprocess.Popen([sys.executable, ROOT / 'scripts/start-native-desktop.py', '30'],
                                  env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as supervisor:
                try:
                    deadline = time.monotonic() + 5
                    compositor = None
                    while time.monotonic() < deadline and supervisor.poll() is None:
                        if event_path.is_file():
                            records = event_path.read_text().splitlines()
                            compositor = next((event['child_pid'] for event in map(json.loads, records)
                                               if event['component'] == 'compositor' and
                                               event['event'] == 'process_start'), None)
                            if compositor is not None:
                                break
                        time.sleep(.02)
                    self.assertIsNotNone(compositor)
                    os.kill(compositor, signal.SIGKILL)
                    _, errors = supervisor.communicate(timeout=5)
                    self.assertEqual(supervisor.returncode, 256 - signal.SIGKILL, errors)
                finally:
                    if supervisor.poll() is None:
                        supervisor.terminate()
                        supervisor.wait(timeout=5)
            records = list(map(json.loads, event_path.read_text().splitlines()))
            self.assertEqual(event_path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(records[0]['session_log'], env['AIR_SESSION_LOG'])
            exit_record = next(event for event in records if event['component'] == 'compositor' and
                               event['event'] == 'process_exit')
            self.assertEqual(exit_record['child_pid'], compositor)
            self.assertEqual(exit_record['signal'], signal.SIGKILL)
            self.assertFalse(exit_record['termination_requested'])
            tracker_exit = next(event for event in records if event['component'] == 'tracker' and
                                event['event'] == 'process_exit')
            self.assertTrue(tracker_exit['termination_requested'])
            self.assertEqual(records[-1]['event'], 'session_end')
