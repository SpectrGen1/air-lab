#!/usr/bin/env python3
"""Exercise the spatial workspace on two isolated headless Wayland outputs."""
import json
import math
import os
from pathlib import Path
import shlex
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.local/build'
RUN = ROOT / '.local/native-headless' / str(time.time_ns())
TARGET = 'HEADLESS-2'
LABWC = BUILD / 'labwc/labwc'
CONTROL = BUILD / 'native-tests/air-test-control'


def wait_for(check, label, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.04)
    raise AssertionError(f'{label} timed out. Logs in {RUN}')


def launch(env, processes, name, *command):
    with (RUN / f'{name}.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
    processes.append(process)


def build_lock_client():
    protocols = Path(subprocess.check_output(
        ['pkg-config', '--variable=pkgdatadir', 'wayland-protocols'], text=True).strip())
    generated = []
    for name in ('pointer-constraints', 'relative-pointer'):
        stem = f'{name}-unstable-v1'
        xml = protocols / 'unstable' / name / f'{stem}.xml'
        for mode, suffix in (('client-header', '-protocol.h'), ('private-code', '-protocol.c')):
            path = RUN / (stem + suffix)
            subprocess.run(['wayland-scanner', mode, xml, path], check=True)
            if mode == 'private-code':
                generated.append(path)
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'gtk+-3.0', 'wayland-client'], text=True))
    binary = RUN / 'workspace-lock'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', RUN,
                    ROOT / 'native/tests/workspace-lock.c', *generated, '-o', binary, *flags], check=True)
    return binary


def check(runtime, processes, sender, lock_client):
    config = RUN / 'config'
    config.mkdir()
    (config / 'rc.xml').write_text(
        '<labwc_config><keyboard><default /></keyboard><mouse><default /></mouse></labwc_config>\n')
    (config / 'autostart').write_text('#!/bin/sh\nprintf "%s" "$WAYLAND_DISPLAY" > ' +
                                     shlex.quote(str(RUN / 'display')) + '\n')
    (config / 'autostart').chmod(0o700)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    ready = RUN / 'tracker.json'
    ready.write_text(json.dumps({'connected': True, 'calibrated': True, 'ready_at': 0,
                                 'source_port': sender.getsockname()[1]}))
    env = {key: value for key, value in os.environ.items()
           if key not in ('WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'DISPLAY', 'DBUS_SESSION_BUS_ADDRESS')}
    diagnostics = RUN / 'diagnostics.jsonl'
    diagnostics.touch(mode=0o600)
    env.update(AIR_SPATIAL_EVENT_LOG=str(diagnostics), XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=str(config), XDG_STATE_HOME=str(RUN),
               WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='2', WLR_RENDERER='gles2',
               WLR_RENDER_DRM_DEVICE=os.getenv('AIR_TEST_RENDER_NODE', '/dev/dri/renderD128'),
               AIR_NATIVE_TEST_ID=str(RUN), AIR_SPATIAL_OUTPUT=TARGET, AIR_SPATIAL_POWER='0',
               AIR_SPATIAL_HELPER=str(ROOT / 'scripts/start-native-presenter.py'),
               AIR_NATIVE_RENDERER=str(BUILD / 'air-native/air-presenter'),
               AIR_TRACKER_READY_FILE=str(ready), AIR_SPATIAL_UDP_PORT=str(port),
               AIR_SPATIAL_AUTO_START='0', AIR_SPATIAL_STEREO='0', AIR_SPATIAL_WIDGET_HELPER='',
               AIR_SPATIAL_WORKSPACE_STATE=str(RUN / 'layout'))
    stopped = threading.Event()

    def feed():
        sequence = 0
        while not stopped.is_set():
            sender.sendto(struct.pack('=6dI', 0, 0, 0, 0, 0, 0, sequence), ('127.0.0.1', port))
            sequence += 1
            stopped.wait(1 / 120)

    thread = threading.Thread(target=feed, daemon=True)
    thread.start()
    try:
        launch(env, processes, 'compositor', LABWC, '-C', config, '-V')
        wait_for(lambda: (RUN / 'display').exists() and (RUN / 'display').stat().st_size > 0,
                 'private compositor')
        display = (RUN / 'display').read_text()
        assert '/' not in display and (Path(runtime) / display).is_socket()
        events = RUN / 'events'
        env.update(WAYLAND_DISPLAY=display, GDK_BACKEND='wayland', AIR_TEST_EVENTS=str(events),
                   AIR_TEST_CONFIRM_DISPOSABLE='1')

        def log():
            return (RUN / 'compositor.log').read_text(errors='replace')

        def trace():
            return events.read_text().splitlines() if events.exists() else []

        def journal(event):
            return [item for item in map(json.loads, diagnostics.read_text().splitlines())
                    if item['event'] == event]

        def control(*actions):
            subprocess.run([str(CONTROL), *map(str, actions)], env=env, timeout=5, check=True)

        def image():
            return subprocess.check_output(['grim', '-o', TARGET, '-t', 'ppm', '-'], env=env, timeout=4)

        # Keep input capabilities advertised while short control invocations come and go.
        launch(env, processes, 'input', CONTROL, 'wait', '30000')
        time.sleep(.15)
        launch(env, processes, 'client', 'python3', ROOT / 'native/tests/workspace.py')
        wait_for(lambda: 'ready' in trace(), 'GTK windows')
        control('chord', 45)
        wait_for(lambda: 'SPATIAL |' in log(), 'spatial presentation', 12)
        control('chord', 38, 'wait', 200)
        overview = image()
        subprocess.run(['grim', '-o', TARGET, str(RUN / 'overview.png')], env=env,
                       timeout=4, check=True)
        control('key', 31, 'key', 24, 'key', 22, 'key', 28, 'wait', 220)  # sou + Enter
        assert image() != overview
        control('key', 30)
        wait_for(lambda: 'Source key a' in trace(), 'overview switched keyboard focus')
        assert 'Destination key s' not in trace() and 'Source key u' not in trace()

        output = next(item for item in json.loads(subprocess.check_output(
            ['wlr-randr', '--json'], env=env, timeout=5)) if item['name'] == TARGET)
        width = output.get('logical_width', 1280)
        gain = 2 * math.tan(math.radians(20)) / width
        control('chord', 33, 'chord', 38, 'key', 32, 'key', 18, 'key', 31, 'wait', 160,
                'motion', 0, -math.atan(-.01925) / gain, 'wait', 120,
                'button', 272, 1, 'button', 272, 0, 'wait', 100, 'key', 48)
        wait_for(lambda: 'Destination key b' in trace(), 'overview mouse selection')
        control('chord', 38, 'key', 31, 'key', 24, 'key', 22, 'key', 28, 'wait', 150)

        control('chord', 3, 'wait', 250, 'chord', 31)  # Pair, save
        wait_for(lambda: (RUN / 'layout').exists(), 'atomic layout save')
        control('chord', 34, 'wait', 250)  # Game focus
        game = image()
        control('chord', 34, 'wait', 250, 'chord', 5, 'wait', 250)  # Leave and recall
        pair = image()
        assert pair != game
        control('chord', 2, 'wait', 180)
        focus = image()
        control('chord', 4, 'wait', 180)
        assert image() != focus
        control('chord', 5, 'wait', 180)
        control('chord', 48, 'wait', 250)  # Reading
        assert image() != pair
        control('chord', 48, 'wait', 250)

        # Pair puts focused Source at left, Destination at right. Drag in steps
        # so GTK has a chance to establish its implicit drag grab at the source.
        control('chord', 33, 'motion', -.175 / gain, 0, 'wait', 80,
                'button', 272, 1, 'motion', 18, 0, 'wait', 220,
                'motion', .35 / gain - 18, 0, 'wait', 220, 'button', 272, 0)
        wait_for(lambda: 'Destination drop air-workspace-test' in trace(), 'cross-window drag and drop')
        control('chord', 33, 'motion', -.175 / gain, 0, 'wait', 80,
                'button', 272, 1, 'motion', 18, 0, 'wait', 150,
                'motion', .35 / gain - 18, 0, 'wait', 150,
                'key', 1, 'wait', 80, 'button', 272, 0)
        assert trace().count('Source drag started') == 2
        assert trace().count('Destination drop air-workspace-test') == 1
        launch(env, processes, 'lock-client', lock_client)
        wait_for(lambda: 'Game ready' in trace(), 'lock client')
        control('chord', 38, 'key', 25, 'key', 24, 'key', 23, 'key', 28, 'wait', 250)
        control('chord', 34, 'chord', 33, 'wait', 120, 'key', 37)
        wait_for(lambda: 'Game locked' in trace(), 'spatial pointer lock')
        control('motion', 70, -20)
        wait_for(lambda: 'Game relative motion' in trace(), 'relative motion during lock')
        control('key', 22, 'wait', 150, 'chord', 34)
        wait_for(lambda: 'Game client unlocked' in trace(), 'clean pointer unlock')
        assert 'desktop fallback' not in log() and 'unsupported pointer constraint' not in log()
        control('chord', 38, 'key', 31, 'key', 24, 'key', 22, 'key', 28, 'wait', 170,
                'chord', 2, 'key', 64)
        wait_for(lambda: 'Animation started' in trace(), 'simulated scrolling/video load')
        samples = log().count('pose age at scanout p95/p99')
        wait_for(lambda: log().count('pose age at scanout p95/p99') > samples,
                 'presentation timing metrics under load')
        assert 'restoring desktop' not in log()
        control('chord', 45)
        wait_for(lambda: journal('outputs_restored'), 'desktop recovery event')
        assert journal('spatial_exit')[-1]['reason'] == 'automatic spatial mode disabled'
        wait_for(lambda: any(item['termination_requested'] for item in journal('process_exit')),
                 'expected presenter termination')
        control('chord', 45)
        wait_for(lambda: len(journal('spatial_enter')) == 2, 'second spatial entry')
        crash_pid = journal('spatial_arming')[-1]['presenter_pid']
        os.kill(crash_pid, signal.SIGKILL)
        wait_for(lambda: len(journal('spatial_exit')) == 2 and
                 any(item['child_pid'] == crash_pid for item in journal('process_exit')),
                 'unexpected presenter death and recovery')
        crash = next(item for item in journal('process_exit') if item['child_pid'] == crash_pid)
        assert crash['signal'] == signal.SIGKILL and not crash['termination_requested'], crash
        assert journal('spatial_exit')[-1]['phase'] == 'spatial'
        control('chord', 45)
    finally:
        stopped.set()
        thread.join()


def main():
    RUN.mkdir(parents=True)
    processes = []
    lock_client = build_lock_client()
    try:
        with tempfile.TemporaryDirectory(prefix='air-headless-') as runtime, socket.socket(
                socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.bind(('127.0.0.1', 0))
            try:
                check(runtime, processes, sender, lock_client)
            finally:
                for process in reversed(processes):
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                for process in reversed(processes):
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=3)
    except Exception:
        print(f'Headless workspace logs: {RUN}', flush=True)
        raise
    print(f'Headless workspace passed: {RUN}')


if __name__ == '__main__':
    main()
