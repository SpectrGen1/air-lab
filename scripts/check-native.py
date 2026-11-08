#!/usr/bin/env python3
"""Exercise native integration on two disposable nested outputs, never the host seat."""
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.local/build'
RUN = ROOT / '.local/native-check' / str(time.time_ns())
RUN.mkdir(parents=True)
MARKER = str(RUN)
LABWC = BUILD / 'labwc/labwc'
HELPER = BUILD / 'air-native/air-presenter'
CONTROL = BUILD / 'native-tests/air-test-control'
LOCK = BUILD / 'native-tests/air-test-lock'
DISCONNECT = BUILD / 'native-tests/air-test-disconnect'
for binary in (LABWC, HELPER, CONTROL, LOCK, DISCONNECT):
    if not binary.exists():
        raise SystemExit('Build with scripts/build-native.sh and scripts/build-native-test-tools.sh first.')


def wait_for(check, description, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.05)
    raise AssertionError('Timed out: ' + description)


def processes():
    found = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            env = (proc / 'environ').read_bytes().split(b'\0')
            if ('AIR_NATIVE_TEST_ID=' + MARKER).encode() in env:
                found.append((int(proc.name), (proc / 'cmdline').read_bytes().split(b'\0')))
        except (OSError, PermissionError):
            pass
    return found


config = RUN / 'config'
config.mkdir()
(config / 'rc.xml').write_text('<labwc_config><keyboard><default /></keyboard><mouse><default /></mouse></labwc_config>\n')
# sh executes this script inside the nested compositor's own environment.
(config / 'autostart').write_text('#!/bin/sh\nprintf "%s\\n" "$WAYLAND_DISPLAY" > "' + str(RUN / 'display') + '"\n')
(config / 'autostart').chmod(0o700)
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
base_env = os.environ | {'AIR_NATIVE_TEST_ID': MARKER}
nested_env = base_env | {
    'WLR_BACKENDS': 'wayland', 'WLR_WL_OUTPUTS': '2',
    'WLR_RENDERER': os.environ.get('AIR_NATIVE_TEST_RENDERER', 'vulkan'), 'WLR_NO_HARDWARE_CURSORS': '1',
    'AIR_SPATIAL_HELPER': str(ROOT / 'scripts/start-native-presenter.py'), 'AIR_SPATIAL_OUTPUT': 'WL-2',
    'AIR_NATIVE_RENDERER': str(HELPER), 'AIR_TRACKER_READY_FILE': str(RUN / 'tracker.json'),
    'AIR_SPATIAL_POWER': '1', 'AIR_SPATIAL_UDP_PORT': str(port),
}
angles = [0., 0., 0.]
paused = False
stop = threading.Event()
reset_stream = threading.Event()
sender_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sender_socket.bind(('127.0.0.1', 0))
source_port = sender_socket.getsockname()[1]

def send_pose():
    with sender_socket as sender:
        sequence = 0
        while not stop.is_set():
            if not paused:
                if reset_stream.is_set():
                    sequence = 0
                    reset_stream.clear()
                sender.sendto(struct.pack('=6dI', 0, 0, 0, *angles, sequence), ('127.0.0.1', port))
                sequence = (sequence + 1) & 0xffffffff
            stop.wait(1 / 120)


(RUN / 'tracker.json').write_text(json.dumps({'connected':True,'calibrated':True,'ready_at':0,'source_port':source_port}))
thread = threading.Thread(target=send_pose, daemon=True)
try:
    with (RUN / 'compositor.log').open('wb') as log:
        subprocess.run(['setsid', '-f', str(LABWC), '-C', str(config), '-V'], env=nested_env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    wait_for(lambda: (RUN / 'display').exists(), 'nested compositor startup')
    display = (RUN / 'display').read_text().strip()
    if display == os.environ.get('WAYLAND_DISPLAY'):
        raise AssertionError('Refusing to inject input on the host display')
    env = base_env | {'WAYLAND_DISPLAY': display, 'AIR_TEST_CONFIRM_DISPOSABLE': '1',
                      'GDK_BACKEND': 'wayland', 'AIR_TEST_EVENTS': str(RUN / 'events.jsonl')}
    thread.start()
    with (RUN / 'editor.log').open('wb') as log:
        subprocess.run(['setsid', '-f', 'python3', str(ROOT / 'native/tests/editor.py')], env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    wait_for(lambda: (RUN / 'events.jsonl').exists(), 'GTK editor')

    def logs():
        return (RUN / 'compositor.log').read_text(errors='replace')

    def events():
        return [json.loads(line) for line in (RUN / 'events.jsonl').read_text().splitlines()]

    def control(*args):
        subprocess.run([str(CONTROL), *map(str, args)], env=env, check=True, timeout=10)
        time.sleep(.15)

    def power():
        return subprocess.check_output(['wlopm'], env=env, text=True, timeout=5)

    def enter(first=False):
        count = logs().count('SPATIAL |')
        control('chord', 45)
        if first:
            time.sleep(1)
            control('chord', 28)
        wait_for(lambda: logs().count('SPATIAL |') > count, 'spatial entry')
        assert 'WL-1 off' in power() and 'WL-2 on' in power()

    def restore():
        control('chord', 1)
        wait_for(lambda: 'WL-1 on' in power(), 'physical output restoration')

    enter(True)
    control('motion', 0, 0, 'button', 272, 1, 'button', 272, 0, 'key', 30)
    wait_for(lambda: any(e['kind'] == 'text' and e['value'].endswith('a') for e in events()), 'typing into original editor')
    print('PASS: original editor receives real seat clicks/keys with its flat output sleeping', flush=True)

    before = len(events())
    angles = [15., 5., 3.]
    time.sleep(.4)
    angles = [-15., -5., -3.]
    time.sleep(.4)
    angles = [0., 0., 0.]
    time.sleep(.3)
    assert not [e for e in events()[before:] if e['kind'] in ('enter', 'leave', 'motion', 'press', 'release', 'key')]
    control('key', 48)
    assert events()[-1]['value'].endswith('b')
    print('PASS: head-only rotation produces no pointer or keyboard events and keeps focus', flush=True)

    # A source port owned by another local UDP process must not change the view.
    paused = True
    before = len(events())
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as rogue:
        for _ in range(10):
            rogue.sendto(struct.pack('=6dI', 0, 0, 0, 60, 0, 0, 1), ('127.0.0.1', port))
        time.sleep(.035)
        subprocess.run([str(CONTROL), 'button', '272', '1', 'button', '272', '0', 'wait', '25'],
                       env=env, check=True, timeout=5)
        time.sleep(.025)
    paused = False
    assert any(e['kind'] == 'press' for e in events()[before:]), 'unauthorized UDP source displaced the trusted view'
    print('PASS: unrelated local UDP packets cannot replace the session-owned tracking source', flush=True)


    frames = []
    for _ in range(2):
        frames.append(subprocess.check_output(['grim', '-o', 'WL-2', '-t', 'ppm', '-'], env=env, timeout=5))
        time.sleep(.4)
    assert hashlib.sha256(frames[0]).digest() != hashlib.sha256(frames[1]).digest()
    print('PASS: captured editor animation continues while the flat primary output sleeps', flush=True)

    angles = [60., 0., 0.]
    time.sleep(.3)
    before = len(events())
    control('button', 272, 1, 'button', 272, 0)
    assert not [e for e in events()[before:] if e['kind'] in ('press', 'release')]
    angles = [0., 0., 0.]
    time.sleep(.3)
    control('button', 272, 1, 'motion', 600, 0, 'wait', 100, 'button', 272, 0)
    assert [e for e in events() if e['kind'] == 'motion'][-1]['x'] > 900
    assert [e for e in events() if e['kind'] == 'release'][-1]['x'] > 900
    print('PASS: offscreen new clicks are rejected; implicit grabs and releases extend past panel edges', flush=True)

    restore()
    subprocess.run(['wlr-randr', '--output', 'WL-1', '--scale', '1.25'], env=env, check=True, timeout=5)
    enter()
    control('chord', 33, 'key', 60)  # summon pointer, open GTK transient dialog with F2
    wait_for(lambda: any(e['kind'] == 'dialog-ready' for e in events()), 'modal dialog')
    time.sleep(.5)
    control('key', 1)
    wait_for(lambda: any(e['kind'] == 'dialog-closed' for e in events()), 'modal dialog closure')
    print('PASS: fractional scale 1.25 and native transient dialog capture/focus', flush=True)

    # The test lock is intentionally unauthenticated and exists only on this nested socket.
    with (RUN / 'lock.log').open('wb') as log:
        locker = subprocess.Popen([str(LOCK)], env=env, stdout=log, stderr=subprocess.STDOUT)
        wait_for(lambda: 'LOCKED' in (RUN / 'lock.log').read_text(), 'lock fixture')
        time.sleep(.2)
        ppm = subprocess.check_output(['grim', '-o', 'WL-2', '-t', 'ppm', '-'], env=env, timeout=5)
        (RUN / 'locked.ppm').write_bytes(ppm)
        header = re.match(rb'P6\s+(?:#[^\n]*\n\s*)?\d+\s+\d+\s+(?:255|65535)\s', ppm)
        assert header and not any(ppm[header.end():]), 'Lock output exposed nonblack content'
        assert not any(str(HELPER).encode() == args[0] for _, args in processes())
        assert locker.wait(timeout=8) == 0
    wait_for(lambda: 'WL-1 on' in power(), 'lock recovery output restoration')
    entered = logs().count('SPATIAL |')
    time.sleep(.3)
    assert logs().count('SPATIAL |') == entered
    print('PASS: lock blanks native output, kills retained helper buffers, restores monitors, and requires re-entry', flush=True)

    enter()
    control('chord', 33)
    before_events = len(events())
    held = subprocess.Popen([str(CONTROL), 'button', '272', '1', 'wait', '1800',
                             'button', '272', '0'], env=env)
    wait_for(lambda: any(e['kind'] == 'press' for e in events()[before_events:]), 'held press')
    helper_pid = next(pid for pid, args in processes() if args[0] == str(HELPER).encode())
    os.kill(helper_pid, signal.SIGSTOP)
    restore()
    assert held.wait(timeout=5) == 0
    time.sleep(.2)
    releases = [e for e in events()[before_events:] if e['kind'] == 'release']
    assert len(releases) == 1, releases
    print('PASS: emergency escape works with a frozen helper and releases held buttons exactly once', flush=True)

    enter()
    before = logs().count('watchdog expired')
    paused = True
    wait_for(lambda: logs().count('watchdog expired') > before, 'stale tracking recovery')
    wait_for(lambda: 'WL-1 on' in power(), 'stale tracking restored outputs')
    paused = False
    print('PASS: stale tracking restores desktop without input from the helper', flush=True)

    enter()
    before = logs().count('tracker reference reset')
    reset_helper = next(pid for pid, args in processes() if args[0] == str(HELPER).encode())
    reset_stream.set()
    wait_for(lambda: logs().count('tracker reference reset') > before, 'tracker reference reset recovery')
    wait_for(lambda: not Path(f'/proc/{reset_helper}').exists(), 'reference reset helper exit')
    wait_for(lambda: 'WL-1 on' in power(), 'reference reset restored outputs')
    print('PASS: tracker reference reset exits spatial mode instead of flipping the workspace', flush=True)

    enter()
    helper_pid = next(pid for pid, args in processes() if args[0] == str(HELPER).encode())
    before = logs().count('watchdog expired')
    os.kill(helper_pid, signal.SIGSTOP)
    wait_for(lambda: logs().count('watchdog expired') > before, 'hung helper recovery')
    wait_for(lambda: 'WL-1 on' in power(), 'hung helper restored outputs')
    wait_for(lambda: not Path(f'/proc/{helper_pid}').exists(), 'hung helper termination')
    print('PASS: hung helper is terminated and outputs recover through the compositor watchdog', flush=True)
    enter()
    before = logs().count('output topology changed')
    parent_env = base_env | {'AIR_TEST_CONFIRM_DISPOSABLE': '1'}
    subprocess.run([str(DISCONNECT)], env=parent_env, check=True, timeout=5)
    wait_for(lambda: logs().count('output topology changed') > before, 'output disconnect recovery')
    wait_for(lambda: 'WL-1 on' in power(), 'remaining output restoration after disconnect')
    before_edit = len(events())
    control('key', 46)
    wait_for(lambda: any(e['kind'] == 'text' for e in events()[before_edit:]), 'client survived output disconnect')
    print('PASS: glasses-output disconnect restores a usable output without killing the existing editor', flush=True)

    (RUN / 'result.json').write_text(json.dumps({'passed':True,'labwc_sha256':hashlib.sha256(LABWC.read_bytes()).hexdigest(),'presenter_sha256':hashlib.sha256(HELPER.read_bytes()).hexdigest()}))
    print('Evidence: ' + str(RUN), flush=True)
finally:
    stop.set()
    if thread.is_alive():
        thread.join(timeout=2)
    # Only processes carrying this test's unique marker can be stopped.
    for pid, args in processes():
        if args and args[0] == str(LABWC).encode():
            os.kill(pid, signal.SIGTERM)
    time.sleep(.2)
    for pid, args in processes():
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
