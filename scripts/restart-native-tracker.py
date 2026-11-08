#!/usr/bin/env python3
"""Replace a native session's tracker, keeping its compositor and clients alive.

The handoff monitor owns the replacement until the current compositor or original
session supervisor exits. The next login uses the normal session supervisor.
"""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

ROOT = Path(os.environ.get('AIR_LAB_ROOT', Path(__file__).resolve().parent.parent)).resolve()


def environment(proc):
    return dict(item.split(b'=', 1) for item in (proc / 'environ').read_bytes().split(b'\0') if b'=' in item)


def find_session():
    sessions = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            exe = (proc / 'exe').readlink()
            if exe.name == 'labwc' and exe.is_relative_to(ROOT / '.local/releases'):
                env = environment(proc)
                if env.get(b'AIR_LAB_ROOT') == os.fsencode(ROOT):
                    sessions.append((proc, env))
        except OSError:
            pass
    if len(sessions) != 1:
        raise SystemExit('Expected one installed native desktop session.')
    return sessions[0]


def parent_pid(proc):
    return int(next(line.split()[1] for line in (proc / 'status').read_text().splitlines() if line.startswith('PPid:')))


def supervise():
    proc, env = find_session()
    ready = Path(os.fsdecode(env[b'AIR_TRACKER_READY_FILE']))
    state = json.loads(ready.read_text())
    old = Path('/proc') / str(state['pid'])
    old_exe = (old / 'exe').readlink()
    if old_exe.name != 'tracker' or not old_exe.is_relative_to(ROOT / '.local/releases'):
        raise SystemExit('Unexpected existing reader; refusing handoff.')
    session_parent = parent_pid(proc)
    if parent_pid(old) != session_parent:
        raise SystemExit('Reader is not owned by the original session supervisor.')
    # pidfds prevent PID reuse from changing the lifetime we follow or terminate.
    compositor_fd = os.pidfd_open(int(proc.name))
    parent_fd = os.pidfd_open(session_parent)
    old_fd = os.pidfd_open(int(old.name))
    lifetime = select.poll()
    lifetime.register(compositor_fd, select.POLLIN)
    lifetime.register(parent_fd, select.POLLIN)
    config_path = ROOT / '.local/native-install.json'
    config = json.loads(config_path.read_text())
    replacement = Path(config['tracker'])
    if not replacement.is_file() or not Path(config['tracker_launcher']).is_file():
        raise SystemExit('Missing selected tracker release.')
    # Exit any private presentation child first, letting labwc restore outputs.
    for child in Path('/proc').iterdir():
        if not child.name.isdigit():
            continue
        try:
            child_env = environment(child)
            if parent_pid(child) == int(proc.name) and b'AIR_SPATIAL_FD' in child_env:
                fd = os.pidfd_open(int(child.name))
                signal.pidfd_send_signal(fd, signal.SIGTERM)
                os.close(fd)
        except OSError:
            pass
    signal.pidfd_send_signal(old_fd, signal.SIGTERM)
    stopped = select.poll()
    stopped.register(old_fd, select.POLLIN)
    if not stopped.poll(10000):
        raise SystemExit('Old tracker did not exit; replacement was not started.')
    os.close(old_fd)
    log = ROOT / '.local/tracker/state/xr_driver/driver.log'
    offset = log.stat().st_size if log.exists() else 0
    module_spec = importlib.util.spec_from_file_location('native_session', ROOT / 'scripts/start-native-desktop.py')
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    own_pid = os.getpid()

    def child_lifetime():
        ctypes.CDLL(None).prctl(1, signal.SIGTERM, 0, 0, 0)
        if os.getppid() != own_pid:
            os._exit(1)

    child_env = os.environ | {'AIR_LAB_ROOT': str(ROOT), 'AIR_LAB_TRACKER_BINARY': str(replacement)}
    child = subprocess.Popen([sys.executable, config['tracker_launcher'], '--udp-port', str(config['udp_port'])],
                             env=child_env, preexec_fn=child_lifetime)
    config['live_tracker'] = {'pid': child.pid, 'monitor_pid': own_pid,
                              'compositor_pid': int(proc.name), 'binary': str(replacement)}
    config_path.write_text(json.dumps(config, indent=2) + '\n')
    connected = calibrated = False
    ready_at = 0
    tail = b''
    temp = ready.with_name('tracker.handoff.tmp')
    try:
        while child.poll() is None and not lifetime.poll(200):
            if log.exists():
                with log.open('rb') as stream:
                    if log.stat().st_size < offset:
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
            state = {'connected': connected, 'calibrated': calibrated, 'ready_at': ready_at,
                     'pid': child.pid, 'source_port': module.tracker_source_port(child.pid)}
            # The old supervisor may publish its dead child's state once when
            # these shared log events arrive. Republish the live owner's state;
            # a competing false state can only delay readiness, never enable it.
            temp.write_text(json.dumps(state))
            temp.replace(ready)
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        temp.unlink(missing_ok=True)
        try:
            state = json.loads(ready.read_text())
            if state.get('pid') == child.pid:
                state.update(connected=False, calibrated=False, source_port=None)
                temp.write_text(json.dumps(state))
                temp.replace(ready)
        except FileNotFoundError:
            pass  # Normal session cleanup may already have removed the runtime directory.
        for fd in (parent_fd, compositor_fd):
            os.close(fd)


def main():
    if sys.argv[1:] == ['--supervise']:
        def stop(signum, frame):
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, stop)
        try:
            supervise()
        except KeyboardInterrupt:
            pass
        return
    if sys.argv[1:]:
        raise SystemExit('Usage: python3 scripts/restart-native-tracker.py')
    config = json.loads((ROOT / '.local/native-install.json').read_text())
    live = config.get('live_tracker', {})
    if live.get('monitor_pid') and Path('/proc', str(live['monitor_pid'])).exists():
        raise SystemExit('A tracker handoff is already running; inspect the install manifest.')
    log_path = ROOT / '.local/tracker/handoff.log'
    with log_path.open('ab') as log:
        monitor = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--supervise'],
                                   start_new_session=True, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
    print(f'Tracker handoff monitor {monitor.pid}; log: {log_path}')
    print('Leave the Air resting for its fresh stationary warmup.')


if __name__ == '__main__':
    main()
