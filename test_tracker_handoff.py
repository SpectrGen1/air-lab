"""Exercise tracker handoff/lifetime using disposable processes, with no sensors."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent


def wait_for(check, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(.03)
    raise AssertionError('Timed out waiting for disposable tracker state')


class TestTrackerHandoff(unittest.TestCase):
    def test_replacement_stops_with_compositor_and_preserves_other_client(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release = root / '.local/releases/test'
            release.mkdir(parents=True)
            (root / 'scripts').mkdir()
            shutil.copy2(ROOT / 'scripts/start-native-desktop.py', root / 'scripts/start-native-desktop.py')
            for name in ('labwc', 'tracker'):
                shutil.copy2('/bin/sleep', release / name)
            ready = root / 'ready.json'
            log = root / '.local/tracker/state/xr_driver/driver.log'
            log.parent.mkdir(parents=True)
            reader = release / 'reader.py'
            reader.write_text('import socket,time\nfrom pathlib import Path\n'
                              's=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(("127.0.0.1",0))\n'
                              + 'with Path(' + repr(str(log)) + ').open("a") as f:\n'
                              ' f.write("Device connected, redirecting input\\nDevice calibration complete\\n");f.flush()\n'
                              'time.sleep(30)\n')
            config_path = root / '.local/native-install.json'
            config_path.write_text(json.dumps({'tracker': str(reader), 'tracker_launcher': str(reader), 'udp_port': 12345}))
            parent_script = root / 'parent.py'
            parent_script.write_text('import subprocess,os,time,json,sys\nfrom pathlib import Path\n'
                                     + 'c=subprocess.Popen([' + repr(str(release / 'labwc')) + ',"30"])\n'
                                     + 't=subprocess.Popen([' + repr(str(release / 'tracker')) + ',"30"])\n'
                                     + 'Path(' + repr(str(ready)) + ').write_text(json.dumps({"pid":t.pid}))\n'
                                     'print(c.pid,t.pid,flush=True)\n'
                                     'try:c.wait()\nfinally:\n t.terminate();t.wait()\n')
            env = os.environ | {'AIR_LAB_ROOT': str(root), 'AIR_TRACKER_READY_FILE': str(ready), 'AIR_NATIVE_TEST_ID': str(root)}
            # An unrelated process must survive both the handoff and compositor exit.
            unrelated = subprocess.Popen(['/bin/sleep', '30'])
            parent = subprocess.Popen([sys.executable, str(parent_script)], env=env, stdout=subprocess.PIPE, text=True)
            monitor = None
            new_pid = None
            try:
                compositor_pid, old_pid = map(int, parent.stdout.readline().split())
                monitor = subprocess.Popen([sys.executable, str(ROOT / 'scripts/restart-native-tracker.py'), '--supervise'],
                                           env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                config = wait_for(lambda: json.loads(config_path.read_text()) if 'live_tracker' in config_path.read_text() else None)
                new_pid = config['live_tracker']['pid']
                def live_ready():
                    state = json.loads(ready.read_text())
                    return state if state.get('pid') == new_pid and state.get('source_port') and state.get('calibrated') else None
                self.assertTrue(wait_for(live_ready)['connected'])
                self.assertIsNone(unrelated.poll())
                self.assertTrue(Path('/proc', str(compositor_pid)).exists())
                os.kill(compositor_pid, signal.SIGTERM)
                out, err = monitor.communicate(timeout=5)
                self.assertEqual(monitor.returncode, 0, err)
                self.assertFalse(Path('/proc', str(new_pid)).exists())
                self.assertFalse(json.loads(ready.read_text())['connected'])
                self.assertIsNone(unrelated.poll())
                parent.wait(timeout=5)
            finally:
                if monitor and monitor.poll() is None:
                    monitor.terminate(); monitor.communicate(timeout=5)
                if parent.poll() is None:
                    parent.terminate(); parent.wait(timeout=5)
                parent.stdout.close()
                unrelated.terminate(); unrelated.wait(timeout=5)
                if new_pid and Path('/proc', str(new_pid)).exists():
                    os.kill(new_pid, signal.SIGTERM)
