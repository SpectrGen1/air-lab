#!/usr/bin/env python3
"""Session supervisor: the original desktop stays in one patched labwc process."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_stereo import StereoMode
from native_events import child_exit, open_session, record

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
    state_home = Path(os.environ.get('XDG_STATE_HOME') or Path.home() / '.local/state')
    workspace_dir = state_home / 'air-lab'
    try:
        workspace_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        workspace_state = str(workspace_dir / 'workspace-v1')
    except OSError as error:
        workspace_state = ''
        print(f'Air spatial layout persistence unavailable: {error}', file=sys.stderr)
    log_path = ROOT / '.local/tracker/state/xr_driver/driver.log'
    try:
        events = open_session(workspace_dir)
    except OSError as error:
        events = None
        print(f'Air diagnostics unavailable: {error}', file=sys.stderr, flush=True)
    record(events, 'supervisor', 'session_start', release=Path(config['labwc']).parent.name,
           session_log=os.environ.get('AIR_SESSION_LOG'), tracker_log=str(log_path))
    if events:
        print(f'Air diagnostics: {events}', file=sys.stderr, flush=True)
    offset = log_path.stat().st_size if log_path.exists() else 0
    env = os.environ | {
        'AIR_SPATIAL_EVENT_LOG': str(events) if events else '',
        'AIR_SPATIAL_WORKSPACE_STATE': workspace_state,
        'AIR_SPATIAL_HELPER': config.get('presenter_launcher', str(ROOT / 'scripts/start-native-presenter.py')),
        'AIR_NATIVE_RENDERER': config['presenter'],
        'AIR_SPATIAL_WIDGET_HELPER': config.get('widget_launcher', str(ROOT / 'scripts/start-native-widgets.py')) if config.get('widgets', True) else '',
        'AIR_LAB_ROOT': str(ROOT),
        'AIR_LAB_TRACKER_BINARY': config.get('tracker', str(ROOT / '.local/build/xr-driver/xrDriver')),
        'AIR_TRACKER_READY_FILE': str(ready_file),
        'AIR_SPATIAL_OUTPUT': config['output'],
        'AIR_SPATIAL_UDP_PORT': str(config.get('udp_port', 4243)),
        'AIR_SPATIAL_RESERVED_DISPLAY': '1' if config.get('reserved_display', False) else '0',
        'AIR_SPATIAL_STEREO': '1' if config.get('stereo', True) else '0',
        'AIR_SPATIAL_IPD_MM': str(config.get('ipd_mm', 64)),
        'AIR_SPATIAL_IPD_CENTER_MM': str(config.get('ipd_center_mm', 0)),
        'AIR_SPATIAL_IPD_BASELINE_MM': str(config.get('ipd_baseline_mm', config.get('ipd_mm',64))),
        'AIR_SPATIAL_AUTO_START': '1' if config.get('auto_start', True) else '0',
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

    children = []
    reported = set()
    shutdown_signal = None

    def interrupted(signum, frame):
        nonlocal shutdown_signal
        shutdown_signal = signum
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    stereo = StereoMode(ready_file, config, INSTALL)
    try:
        tracker = subprocess.Popen([sys.executable, config.get('tracker_launcher', str(ROOT / 'scripts/run-tracker.py')),
                                    '--udp-port', env['AIR_SPATIAL_UDP_PORT']], env=env, preexec_fn=child_lifetime)
        children.append(('tracker', tracker))
        record(events, 'tracker', 'process_start', child_pid=tracker.pid)
        compositor = subprocess.Popen([config['labwc'], *sys.argv[1:]], env=env,
                                       preexec_fn=child_lifetime)
        children.append(('compositor', compositor))
        record(events, 'compositor', 'process_start', child_pid=compositor.pid)
        connected = calibrated = False
        ready_at = 0
        last_state = None
        tail = b''
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
                        ready_at = 0
                    elif b'Device calibration complete' in line:
                        calibrated = True
                    elif b'Waiting for glasses' in line or b'Retrying driver connection' in line:
                        connected = calibrated = False
            except FileNotFoundError:
                pass
            try:
                stereo.update(compositor.pid, tracker.pid, connected and tracker.poll() is None)
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                print('Air stereo: mode transition is waiting: ' + str(error), file=sys.stderr)
            state = {'connected': connected and tracker.poll() is None,
                     'calibrated': calibrated, 'ready_at': ready_at, 'pid': tracker.pid,
                     'source_port': tracker_source_port(tracker.pid) if tracker.poll() is None else None}
            if tracker.returncode is not None and tracker.pid not in reported:
                child_exit(events, 'tracker', tracker, termination_requested=False)
                reported.add(tracker.pid)
            if state != last_state:
                record(events, 'tracker', 'readiness', child_pid=tracker.pid,
                       connected=state['connected'], calibrated=state['calibrated'],
                       source_ready=state['source_port'] is not None)
                tmp = ready_file.with_suffix('.tmp')
                tmp.write_text(json.dumps(state))
                tmp.replace(ready_file)
                last_state = state
            time.sleep(.2)
        child_exit(events, 'compositor', compositor, termination_requested=False)
        reported.add(compositor.pid)
        return compositor.returncode
    except KeyboardInterrupt:
        record(events, 'supervisor', 'shutdown_requested', signal=shutdown_signal)
        return 0
    except Exception as error:
        record(events, 'supervisor', 'failure', error=type(error).__name__,
               errno=getattr(error, 'errno', None))
        raise
    finally:
        try:
            stereo.restore()
        except OSError as error:
            print('Air stereo: could not request hardware restoration: ' + str(error), file=sys.stderr)
        if stereo.changed:
            time.sleep(1.2)  # Give the existing reader time to restore the hardware before termination.
        terminating = set()
        for _, child in reversed(children):
            if child.poll() is None:
                terminating.add(child.pid)
                child.terminate()
        for component, child in reversed(children):
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
            if child.pid not in reported:
                child_exit(events, component, child, termination_requested=child.pid in terminating)
        try:
            shutil.rmtree(runtime)
        except OSError as error:
            record(events, 'supervisor', 'runtime_cleanup_failed', errno=error.errno)
            print(f'Air runtime cleanup: {error}', file=sys.stderr, flush=True)
        record(events, 'supervisor', 'session_end', shutdown_signal=shutdown_signal)


if __name__ == '__main__':
    sys.exit(main())
