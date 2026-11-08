#!/usr/bin/env python3
"""Session supervisor: the original desktop stays in one patched labwc process."""
import ctypes
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(os.environ.get('AIR_LAB_ROOT', Path(__file__).resolve().parent.parent)).resolve()
INSTALL = ROOT / '.local/native-install.json'


def tracker_source_port(pid):
    """Identify the sole UDP socket owned by our child, not an arbitrary sender."""
    try:
        inodes = set()
        for path in Path(f'/proc/{pid}/fd').iterdir():
            try:
                target = str(path.readlink())
            except OSError:
                continue
            if target.startswith('socket:['):
                inodes.add(target[8:-1])
        ports = []
        for line in Path(f'/proc/{pid}/net/udp').read_text().splitlines()[1:]:
            fields = line.split()
            if fields[9] in inodes:
                port = int(fields[1].split(':')[1], 16)
                if port:
                    ports.append(port)
        return ports[0] if len(ports) == 1 else None
    except (OSError, ValueError, IndexError):
        return None


class TrackerNotifications:
    """Announce each connection's warmup and ready transition, even without a preview."""
    def __init__(self, send=None):
        self.send = send or self.notify
        self.connection = None
        self.announced_ready = False

    @staticmethod
    def notify(body):
        if os.environ.get('AIR_NATIVE_TEST_ID'):
            return
        try:
            subprocess.run(['notify-send', '-a', 'Air Spatial Lab', '-t', '8000',
                            'Air spatial mode', body], check=False, timeout=2)
        except (OSError, subprocess.TimeoutExpired):
            pass

    def observe(self, state, now):
        if not state.get('connected'):
            self.connection = None
            self.announced_ready = False
            return
        connection = (state.get('pid'), state.get('ready_at'))
        ready = (state.get('calibrated') and state.get('source_port')
                 and now >= state.get('ready_at', float('inf')))
        if connection != self.connection:
            self.connection = connection
            self.announced_ready = False
            if not ready:
                self.send('Leave the glasses motionless on a table for one minute. '
                          'You will be notified when tracking is ready.')
        if ready and not self.announced_ready:
            self.send('Glasses ready. Calibration and the one-minute warmup are complete. '
                      'Put them on; once the preview is visible, face forward and '
                      'press Super+Alt+R to recenter.')
            self.announced_ready = True


def main():
    if os.environ.get('AIR_USE_STOCK') == '1':
        os.execv('/usr/bin/labwc', ['/usr/bin/labwc', *sys.argv[1:]])
    try:
        config = json.loads(INSTALL.read_text())
        if not all(os.access(config[key], os.X_OK) for key in ('labwc', 'presenter', 'tracker')):
            raise ValueError('native release is incomplete')
    except (OSError, ValueError, KeyError) as error:
        print('Native setup unavailable; starting stock labwc: ' + str(error), file=sys.stderr)
        os.execv('/usr/bin/labwc', ['/usr/bin/labwc', *sys.argv[1:]])
    runtime = Path(tempfile.mkdtemp(prefix='air-spatial-', dir=os.environ['XDG_RUNTIME_DIR']))
    ready_file = runtime / 'tracker.json'
    log_path = ROOT / '.local/tracker/state/xr_driver/driver.log'
    offset = log_path.stat().st_size if log_path.exists() else 0
    env = os.environ | {
        'AIR_SPATIAL_HELPER': config.get('presenter_launcher', str(ROOT / 'scripts/start-native-presenter.py')),
        'AIR_NATIVE_RENDERER': config['presenter'],
        'AIR_LAB_ROOT': str(ROOT),
        'AIR_LAB_TRACKER_BINARY': config.get('tracker', str(ROOT / '.local/build/xr-driver/xrDriver')),
        'AIR_TRACKER_READY_FILE': str(ready_file),
        'AIR_SPATIAL_OUTPUT': config['output'],
        'AIR_SPATIAL_UDP_PORT': str(config.get('udp_port', 4243)),
        'AIR_SPATIAL_POWER': '1' if config.get('power', False) else '0',
        'AIR_SPATIAL_PANEL_SCALE': str(config.get('panel_scale', 2 / 3)),
        'AIR_SPATIAL_HEAD_ROLL': '1' if config.get('head_roll', False) else '0',
        'AIR_SPATIAL_POINTER_SPEED': str(config.get('pointer_speed', 1)),
    }
    parent_pid = os.getpid()

    def child_lifetime():
        # Set before exec; no thread exists in this supervisor.
        ctypes.CDLL(None).prctl(1, signal.SIGTERM, 0, 0, 0)
        if os.getppid() != parent_pid:
            os._exit(1)

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    children = []
    try:
        tracker = subprocess.Popen([sys.executable, config.get('tracker_launcher', str(ROOT / 'scripts/run-tracker.py')),
                                    '--udp-port', env['AIR_SPATIAL_UDP_PORT']], env=env, preexec_fn=child_lifetime)
        children.append(tracker)
        compositor = subprocess.Popen([config['labwc'], *sys.argv[1:]], env=env,
                                       preexec_fn=child_lifetime)
        children.append(compositor)
        connected = calibrated = False
        ready_at = 0
        last_state = None
        tail = b''
        notifications = TrackerNotifications()
        while compositor.poll() is None:
            try:
                with log_path.open('rb') as stream:
                    if log_path.stat().st_size < offset:
                        offset = 0
                        tail = b''
                        connected = calibrated = False
                    stream.seek(offset)
                    data = stream.read()
                    offset = stream.tell()
                lines = (tail + data).split(b'\n')
                tail = lines.pop()
                for line in lines:
                    if b'Device connected, redirecting input' in line:
                        connected, calibrated = True, False
                        ready_at = time.monotonic() + 60
                    elif b'Device calibration complete' in line:
                        calibrated = True
                    elif b'Waiting for glasses' in line or b'Retrying driver connection' in line:
                        connected = calibrated = False
            except FileNotFoundError:
                pass
            state = {'connected': connected and tracker.poll() is None,
                     'calibrated': calibrated, 'ready_at': ready_at, 'pid': tracker.pid,
                     'source_port': tracker_source_port(tracker.pid) if tracker.poll() is None else None}
            notifications.observe(state, time.monotonic())
            if state != last_state:
                tmp = ready_file.with_suffix('.tmp')
                tmp.write_text(json.dumps(state))
                tmp.replace(ready_file)
                last_state = state
            time.sleep(.2)
        return compositor.returncode
    except KeyboardInterrupt:
        return 0
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
        for child in reversed(children):
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        ready_file.unlink(missing_ok=True)
        runtime.rmdir()


if __name__ == '__main__':
    sys.exit(main())
