#!/usr/bin/env python3
"""Exercise native integration on two disposable nested outputs, never the host seat."""
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import shlex
import socket
import struct
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.local/build'
RUN = ROOT / ('.local/widget-preview' if os.environ.get('AIR_NATIVE_WIDGET_PREVIEW') == '1' else '.local/native-check') / str(time.time_ns())
RUN.mkdir(parents=True)
MARKER = str(RUN)
AUTO_START = os.environ.get('AIR_NATIVE_TEST_AUTO_START') == '1'
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
(config / 'rc.xml').write_text(
    '<labwc_config><keyboard><default />'
    '<keybind key="F9"><action name="VirtualOutputAdd" output_name="WL-2" /></keybind>'
    '<keybind key="F10"><action name="VirtualOutputRemove" output_name="WL-2" /></keybind>'
    '</keyboard><mouse><default /></mouse></labwc_config>\n')
# sh executes this script inside the nested compositor's own environment.
(config / 'autostart').write_text('#!/bin/sh\nprintf "%s\\n" "$WAYLAND_DISPLAY" > "' + str(RUN / 'display') + '"\n')
if AUTO_START:
    with (config / 'autostart').open('a') as script:
        script.write('AIR_TEST_CONFIRM_DISPOSABLE=1 ' + shlex.quote(str(LOCK)) + ' > ' + shlex.quote(str(RUN / 'login-lock.log')) + ' 2>&1 &\n')
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
    'AIR_SPATIAL_STEREO': '0',  # This fixture uses sacrificial mono virtual outputs.
    'AIR_SPATIAL_AUTO_START': '1' if AUTO_START else '0',
    'AIR_SPATIAL_WIDGET_HELPER': str(ROOT / 'scripts/start-native-widgets.py') if os.environ.get('AIR_NATIVE_TEST_WIDGETS') == '1' else '',
    'AIR_SPATIAL_POWER': '1', 'AIR_SPATIAL_UDP_PORT': str(port),
}
WIDGETS = os.environ.get('AIR_NATIVE_TEST_WIDGETS') == '1'
WIDGET_PREVIEW = os.environ.get('AIR_NATIVE_WIDGET_PREVIEW') == '1'
if WIDGETS:
    action = RUN / 'widget-action.py'
    action.write_text('import sys\nfrom pathlib import Path\nwith Path(' + repr(str(RUN / 'widget-actions')) + ').open("a") as f: f.write(sys.argv[1]+"\\n")\n')
    command = 'python3 ' + shlex.quote(str(action))
    menu = RUN / 'widget-menu.xml'
    menu.write_text('<interface><object class="GtkMenu" id="menu">' + ''.join('<child><object class="GtkMenuItem" id="action'+str(i)+'"><property name="label">Widget test action '+str(i)+'</property></object></child>' for i in range(8)) + '</object></interface>')
    cfg = {key:['custom/test'] for key in ('modules-left','modules-center','modules-right')}
    cfg['custom/test'] = {'format':'Spatial widget', 'tooltip':False,
                          'on-click':command+' click', 'on-scroll-up':command+' scroll',
                          'menu':'on-click-right', 'menu-file':str(menu),
                          'menu-actions':{'action'+str(i):command+' menu' for i in range(8)}}
    cfg['modules-left']=['custom/launcher']
    cfg['custom/launcher']={'format':'Launch', 'tooltip':False,
                            'on-click':command+' launcher; wofi --show run --conf /dev/null --width 500 --height 200'}
    fixture = RUN / 'waybar.json'
    fixture.write_text(json.dumps(cfg))
    style = RUN / 'waybar.css'
    style.write_text('* { font-family: sans-serif; font-size: 20px; } #custom-test { padding: 8px; }')
    nested_env.update(AIR_WIDGET_CONFIG=str(fixture), AIR_WIDGET_STYLE=str(style))
    if WIDGET_PREVIEW:
        nested_env.pop('AIR_WIDGET_CONFIG')
        nested_env.pop('AIR_WIDGET_STYLE')
angles = [0., 0., 0.]
paused = AUTO_START
startup_jitter = WIDGETS and AUTO_START
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
                supplied = list(angles)
                if startup_jitter:
                    supplied[0] += .2 * math.sin(sequence * .37)
                sender.sendto(struct.pack('=6dI', 0, 0, 0, *supplied, sequence), ('127.0.0.1', port))
                sequence = (sequence + 1) & 0xffffffff
            stop.wait(1 / 120)


(RUN / 'tracker.json').write_text(json.dumps({'connected':not AUTO_START,'calibrated':not AUTO_START,'ready_at':0,'source_port':source_port}))
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
    editor_pid = next(pid for pid, args in processes()
                      if str(ROOT / 'native/tests/editor.py').encode() in args)

    def logs():
        return (RUN / 'compositor.log').read_text(errors='replace')

    def events():
        return [json.loads(line) for line in (RUN / 'events.jsonl').read_text().splitlines()]

    def control(*args):
        subprocess.run([str(CONTROL), *map(str, args)], env=env, check=True, timeout=10)
        time.sleep(.15)

    def power():
        return subprocess.check_output(['wlopm'], env=env, text=True, timeout=5)

    auto_enabled = AUTO_START

    def enter(first=False):
        global auto_enabled
        if first and AUTO_START:
            wait_for(lambda: 'SPATIAL |' in logs(), 'automatic first spatial entry without confirmation')
        else:
            count = logs().count('SPATIAL |')
            if not auto_enabled:
                control('chord', 45)
                auto_enabled = True
            wait_for(lambda: logs().count('SPATIAL |') > count, 'spatial entry without confirmation')
        assert 'WL-1 off' in power() and 'WL-2 on' in power()

    def restore():
        global auto_enabled
        if auto_enabled:
            control('chord', 45)
            auto_enabled = False
        wait_for(lambda: 'WL-1 on' in power(), 'physical output restoration')
        if AUTO_START:
            attempts = logs().count('automatic preview for connected glasses')
            time.sleep(.8)
            assert logs().count('automatic preview for connected glasses') == attempts
            assert 'WL-1 on' in power(), 'disabled automatic mode restarted'

    if AUTO_START:
        wait_for(lambda: (RUN / 'login-lock.log').exists() and 'LOCKED' in (RUN / 'login-lock.log').read_text(), 'initial login lock')
        assert 'automatic preview for connected glasses' not in logs()
        assert 'WL-1 on' in power() and 'WL-2 on' in power()
        wait_for(lambda: 'UNLOCKED' in (RUN / 'login-lock.log').read_text(), 'initial authentication')
        print('PASS: initial login lock prevents entry until authentication completes', flush=True)
        wait_for(lambda: 'automatic preview for connected glasses' in logs(), 'automatic preview at initialization')
        time.sleep(.4)
        assert 'air presenter: native spatial' not in logs(), 'presenter bypassed tracker readiness'
        assert 'WL-1 on' in power() and 'WL-2 on' in power()
        (RUN / 'tracker.json').write_text(json.dumps({'connected':True,'calibrated':False,
                                                     'source_port':source_port}))
        time.sleep(.4)
        assert 'air presenter: native spatial' not in logs(), 'presenter bypassed calibration'
        (RUN / 'tracker.json').write_text(json.dumps({'connected':True,'calibrated':True,
                                                     'source_port':None}))
        time.sleep(.4)
        assert 'air presenter: native spatial' not in logs(), 'presenter bypassed source ownership'
        (RUN / 'tracker.json').write_text(json.dumps({'connected':True,'calibrated':True,
                                                     'source_port':source_port}))
        wait_for(lambda: 'air presenter: native spatial' in logs(), 'renderer after calibration')
        time.sleep(.2)
        assert 'WL-1 on' in power() and 'SPATIAL |' not in logs(), 'entry bypassed live tracking'
        paused = False

    enter(True)
    if startup_jitter:
        startup_jitter = False
        time.sleep(.2)
        control('chord',19)  # Recenter only after proving automatic entry under changing pose.
        time.sleep(.2)
        print('PASS: automatic entry and monitor sleep complete despite continuous head-pose changes',flush=True)
    if WIDGETS:
        for role in range(3):
            wait_for(lambda: 'spatial widget geometry role=' + str(role) in logs(), 'live Waybar layer capture')
    if WIDGETS and WIDGET_PREVIEW:
        time.sleep(1)
        angles = [0.,26.565051,0.]
        time.sleep(1)
        (RUN / 'widgets-profile.png').write_bytes(subprocess.check_output(
            ['grim','-o','WL-2','-t','png','-'],env=env,timeout=5))
        print('Preview: ' + str(RUN / 'widgets-profile.png'),flush=True)
        raise SystemExit(0)  # Preview evidence is separate from integration acceptance.
    if AUTO_START:
        print('PASS: first connection enters without confirmation, retaining calibration, source and live-scene gates', flush=True)
    soak = float(os.environ.get('AIR_NATIVE_TEST_SOAK', '0'))
    if soak:
        started = time.monotonic()
        while time.monotonic() - started < soak:
            assert not 'restoring desktop' in logs(), 'healthy spatial mode exited during soak'
            time.sleep(.25)
        assert 'WL-1 off' in power() and 'WL-2 on' in power()
        print(f'PASS: active spatial mode stays open for {soak:g}s with monitors sleeping', flush=True)
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
    viewport = re.match(rb'P6\s+(?:#[^\n]*\n\s*)?(\d+)\s+(\d+)', frames[0])
    assert viewport, 'Missing renderer viewport dimensions'
    mouse_gain = 2*math.tan(math.radians(20))/int(viewport.group(1))
    control('chord', 33, 'motion', 20, 0)
    motion = [e for e in events() if e['kind']=='motion'][-1]
    expected = 450 + 1.4*math.tan(20*mouse_gain)/.0006
    assert abs(motion['x']-expected)<1, (motion,expected)
    control('motion', -20, 0, 'chord', 12, 'motion', 20, 0)  # slower
    motion = [e for e in events() if e['kind']=='motion'][-1]
    expected = 450 + 1.4*math.tan(20*mouse_gain/1.2)/.0006
    assert abs(motion['x']-expected)<1, (motion,expected)
    control('chord', 11, 'chord', 33)  # restore configured speed and pointer centre
    print('PASS: viewport-calibrated mouse gain and live sensitivity controls target the expected editor pixel', flush=True)

    print('PASS: captured editor animation continues while the flat primary output sleeps', flush=True)
    control('chord', 33, 'button', 273, 1, 'button', 273, 0)
    wait_for(lambda:any(e['kind']=='context-ready' for e in events()), 'right-click context menu')
    (RUN/'context.png').write_bytes(subprocess.check_output(['grim','-o','WL-2','-t','png','-'],env=env,timeout=5))
    control('motion', 20, 12, 'wait', 650, 'button', 272, 1, 'button', 272, 0)
    wait_for(lambda:any(e['kind']=='context-action' for e in events()), 'click context menu action')
    control('chord', 33, 'button', 273, 1, 'button', 273, 0)
    wait_for(lambda:sum(e['kind']=='context-ready' for e in events())>=2, 'second context menu')
    control('key', 1)
    wait_for(lambda:sum(e['kind']=='context-closed' for e in events())>=2, 'Escape dismisses menu')
    control('wait', 550)  # Separate right clicks from GTK's double-click interval.
    control('chord', 33, 'button', 273, 1, 'button', 273, 0)
    wait_for(lambda:sum(e['kind']=='context-ready' for e in events())>=3, 'third context menu')
    control('motion', -60, -60, 'button', 272, 1, 'button', 272, 0)
    wait_for(lambda:sum(e['kind']=='context-closed' for e in events())>=3, 'outside click dismisses menu')
    print('PASS: real right-click popup opens, accepts a pointer-selected action and dismisses with Escape/outside click', flush=True)


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

    entries_before_lock = logs().count('SPATIAL |')
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
    print('PASS: lock blanks native output, kills retained helper buffers and restores monitors', flush=True)
    if auto_enabled:
        wait_for(lambda: logs().count('SPATIAL |') > entries_before_lock, 'automatic re-entry after unlock')
        assert 'WL-1 off' in power() and 'WL-2 on' in power()
        print('PASS: connected glasses automatically resume spatial mode after unlock', flush=True)
    else:
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
    print('PASS: auto-mode toggle restores desktop with a frozen helper and releases held buttons exactly once', flush=True)

    enter()
    before = logs().count('watchdog expired')
    paused = True
    wait_for(lambda: logs().count('watchdog expired') > before, 'stale tracking recovery')
    wait_for(lambda: 'WL-1 on' in power(), 'stale tracking restored outputs')
    paused = False
    if AUTO_START:
        previous_entries = logs().count('SPATIAL |')
        wait_for(lambda: logs().count('SPATIAL |') > previous_entries, 'automatic recovery after tracker returns', seconds=12)
        restore()
        print('PASS: connected glasses retry after failure and enter when tracking recovers', flush=True)
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
    # Spatial controls consume pointer actions. The frame requests native sizes
    # and wait for the editor to allocate its newly reflowed window.
    def editor_size():
        sizes = [(e['width'], e['height']) for e in events() if e['kind'] == 'window-size']
        return sizes[-1] if sizes else None

    pose = dict(angle=0., height=0., distance=1.4, mpp_x=.0006, mpp_y=.0006, w=900, h=600)
    def aim(x, y):
        angle, h, d = pose['angle'], pose['height'], pose['distance']
        pitch = math.atan2(h, d)
        right = (math.cos(angle), 0., -math.sin(angle))
        up = (-math.sin(angle)*math.sin(pitch), math.cos(pitch), -math.cos(angle)*math.sin(pitch))
        center = (d*math.sin(angle), h, d*math.cos(angle))
        q = [center[i] + right[i]*(x-pose['w']/2)*pose['mpp_x'] +
             up[i]*(pose['h']/2-y)*pose['mpp_y'] for i in range(3)]
        theta, height = math.atan2(q[0], q[2]), 1.4*q[1]/math.hypot(q[0],q[2])
        elevation = math.atan2(height,1.4)
        control('chord', 33, 'motion', 0, -elevation/mouse_gain,
                'motion', theta*math.cos(elevation)/mouse_gain, 0, 'wait', 100)

    def finish_control(previous):
        wait_for(lambda: logs().count('spatial control end') > previous, 'spatial frame release')
        lines = [line for line in logs().splitlines() if 'spatial control end' in line]
        m = re.search(r'angle=([-.0-9]+) height=([-.0-9]+) distance=([-.0-9]+) '
                      r'mpp=([-.0-9]+),([-.0-9]+) desktop=([-0-9,]+) requested=([0-9,]+)', lines[-1])
        assert m, lines[-1]
        pose.update(zip(('angle','height','distance','mpp_x','mpp_y'), map(float, m.groups()[:5])))
        return tuple(map(int, m.group(6).split(','))), tuple(map(int, m.group(7).split(',')))

    before_events = len(events())
    initial_size = editor_size()
    assert initial_size == (900, 600), initial_size
    for horizontal in (True, False):
        before = logs().count('spatial control end')
        old_width, old_height = pose['w'], pose['h']
        if horizontal:
            aim(old_width + 6, old_height / 2)
        else:
            aim(old_width / 2, old_height + 6)
        control('button', 272, 1, 'motion', 30 if horizontal else 0,
                0 if horizontal else 20, 'wait', 120, 'button', 272, 0)
        _, requested = finish_control(before)
        width, height = requested
        if horizontal:
            assert width > old_width and height == old_height, requested
        else:
            assert width == old_width and height > old_height, requested
        wait_for(lambda: editor_size() == requested, 'editor committed the requested edge size')
        pose['w'], pose['h'] = requested
    print('PASS: straight edges resize the real client along one axis', flush=True)
    for left, top, dx, dy in ((True,True,-25,-10), (False,True,25,0),
                               (True,False,0,25), (False,False,20,25)):
        before = logs().count('spatial control end')
        old_width, old_height = pose['w'], pose['h']
        old_mpp = (pose['mpp_x'], pose['mpp_y'])
        aim(-6 if left else old_width + 6, -46 if top else old_height + 6)
        control('button', 272, 1, 'motion', dx, dy, 'wait', 120, 'button', 272, 0)
        _, requested = finish_control(before)
        width, height = requested
        assert width > old_width and height > old_height, requested
        assert abs(height - width * old_height / old_width) <= 1, requested
        assert (pose['mpp_x'], pose['mpp_y']) == old_mpp
        wait_for(lambda: editor_size() == requested, 'editor committed the requested size')
        pose['w'], pose['h'] = requested
    print('PASS: all four corners resize the real client and preserve its aspect ratio', flush=True)

    before = logs().count('spatial control end')
    aim(450, -20)
    control('button', 272, 1)
    stationary = pose.copy()
    angles = [12., 4., 6.]
    time.sleep(.2)
    angles = [0.,0.,0.]
    time.sleep(.2)
    control('button', 272, 0)
    desktop, _ = finish_control(before)
    assert desktop[2:] == editor_size()
    assert pose == stationary, (pose, stationary)
    before = logs().count('spatial control end')
    aim(450, -20)
    control('button', 272, 1, 'motion', 80, 30, 'axis', 10, 'wait', 120, 'button', 272, 0)
    assert finish_control(before)[0] == desktop
    assert pose['angle'] > .02 and pose['height'] < 0 and pose['distance'] > 1.4, pose
    assert not [e for e in events()[before_events:] if e['kind'] in ('press','release','scroll')]
    image_data = subprocess.check_output(['grim','-o','WL-2','-t','png','-'],env=env,timeout=5)
    (RUN / 'spatial-controls.png').write_bytes(image_data)
    print('PASS: title drag moves on the cylinder, wheel changes depth, and head-only motion never moves the panel', flush=True)
    before = logs().count('spatial control end')
    aim(450, -20)
    control('button', 272, 1, 'axis', -10, 'wait', 120, 'button', 272, 0)
    assert finish_control(before)[0] == desktop
    assert abs(pose['distance']-1.4)<1e-5, pose
    assert pose['mpp_x'] == stationary['mpp_x'] and pose['mpp_y'] == stationary['mpp_y']

    aim(450, -20)
    control('button', 272, 1)
    before_events = len(events())
    restore()
    control('button', 272, 0)
    wait_for(lambda: editor_size() == initial_size, 'editor restored its desktop size')
    assert not [e for e in events()[before_events:] if e['kind'] in ('press','release')]
    assert Path(f"/proc/{editor_pid}").exists()
    enter()
    control('key', 30)
    assert any(e['kind']=='text' for e in events()[before_events:])
    print('PASS: auto-mode disable during a spatial drag consumes its release and preserves the original editor', flush=True)
    control('key', 61)  # F3: a disposable second toplevel, not another application process.
    wait_for(lambda: any(e['kind']=='lifecycle-ready' for e in events()), 'lifecycle window')
    pose = dict(angle=0., height=0., distance=1.4, mpp_x=.0006, mpp_y=.0006, w=900, h=600)
    before = logs().count('spatial control begin')
    aim(450, -20)
    control('button', 272, 1)
    wait_for(lambda: logs().count('spatial control begin') > before, 'lifecycle frame held')
    before_events = len(events())
    control('key', 1)
    wait_for(lambda: any(e['kind']=='lifecycle-closed' for e in events()), 'held view destroyed')
    control('motion', 10, 10, 'button', 272, 0)
    assert not [e for e in events()[before_events:] if e['kind'] in ('press','release')]
    control('key', 30)
    assert any(e['kind']=='text' for e in events()[before_events:])
    assert Path(f"/proc/{editor_pid}").exists()
    print('PASS: destroying a held spatial panel cancels its grab without a stray release or losing the editor', flush=True)
    if WIDGETS and WIDGET_PREVIEW:
        angles=[0.,26.565051,0.]
        time.sleep(1)
        (RUN / 'widgets-profile.png').write_bytes(subprocess.check_output(
            ['grim','-o','WL-2','-t','png','-'],env=env,timeout=5))
        angles=[0.,0.,0.]
        time.sleep(.5)
    if WIDGETS and not WIDGET_PREVIEW:
        # The actual Waybar process receives these actions through the real seat.
        action_file = RUN / 'widget-actions'
        subprocess.run([str(BUILD / 'native-tests/air-test-widget-auth')],env=env,check=True,timeout=5)
        def widget_actions():
            return action_file.read_text().splitlines() if action_file.exists() else []
        def widget_aim(x=350., y=24., yaw=0., role=1):
            # Fixture is a 700x48 bar; shared pose uses .0006 m/logical pixel.
            elevation = -.3048 + (1-role)*.045
            pitch = math.atan2(elevation, .6096)
            right = (math.cos(yaw),0.,-math.sin(yaw))
            up = (-math.sin(yaw)*math.sin(pitch),math.cos(pitch),-math.cos(yaw)*math.sin(pitch))
            center = (.6096*math.sin(yaw),elevation,.6096*math.cos(yaw))
            q = [center[i]+right[i]*(x-350)*.0006+up[i]*(24-y)*.0006 for i in range(3)]
            theta = math.atan2(q[0],q[2])
            h = 1.4*q[1]/math.hypot(q[0],q[2])
            pe = math.atan2(h,1.4)
            # Summon starts at the camera yaw/elevation, then move to the ray.
            camera_yaw = -angles[0]*math.pi/180
            camera_elevation = -angles[1]*math.pi/180
            control('chord',33,'motion',0,-(pe-camera_elevation)/mouse_gain,
                    'motion',(theta-camera_yaw)*math.cos(pe)/mouse_gain,0,'wait',150)
        angles = [0.,26.565051,0.]
        time.sleep(.6)
        widget_aim()
        old = len(widget_actions())
        control('button',272,1,'button',272,0)
        wait_for(lambda: len(widget_actions())>old, 'Waybar widget click')
        assert widget_actions()[-1]=='click'
        control('axis',-10)
        wait_for(lambda: 'scroll' in widget_actions(), 'Waybar widget scrolling')
        count = len(events())
        control('key',30)
        wait_for(lambda: any(e['kind']=='text' for e in events()[count:]), 'typing focus after widget click')
        # Rotate horizontally, keep the dock's height fixed, and click its new position.
        angles = [-20.,26.565051,8.]
        time.sleep(.7)
        widget_aim(yaw=20*math.pi/180)
        old = widget_actions().count('click')
        control('button',272,1,'button',272,0)
        wait_for(lambda: widget_actions().count('click')>old, 'yaw-following Waybar click')
        prior = logs().count('spatial widget popup role=1')
        control('button',273,1,'button',273,0)
        wait_for(lambda: logs().count('spatial widget popup role=1')>prior, 'Waybar popup capture')
        geometry = re.findall(r'spatial widget popup role=1 geometry=([-0-9,]+)',logs())[-1]
        px,py,pw,ph = map(int,geometry.split(','))
        widget_aim(px+pw/2,py+ph/2,yaw=20*math.pi/180)
        control('button',272,1,'button',272,0)
        wait_for(lambda: 'menu' in widget_actions(),'Waybar popup action')
        # An action-only widget must leave application keyboard focus intact.
        count = len(events())
        control('key',30)
        wait_for(lambda: any(e['kind']=='text' for e in events()[count:]), 'typing focus after widget menu')
        (RUN / 'widgets.png').write_bytes(subprocess.check_output(
            ['grim','-o','WL-2','-t','png','-'],env=env,timeout=5))
        print('PASS: live Waybar widgets receive click/scroll/menu actions, retain typing focus and follow head yaw',flush=True)
        # Existing Waybar launcher actions open a real, keyboard-interactive layer.
        mapped_launchers = logs().count('spatial widget mapped role=3')
        time.sleep(.6)
        widget_aim(yaw=20*math.pi/180,role=0)
        control('button',272,1,'button',272,0)
        wait_for(lambda: 'launcher' in widget_actions(), 'Waybar launcher action')
        wait_for(lambda: logs().count('spatial widget mapped role=3')>mapped_launchers,
                 'Waybar child launcher capture')
        control('key',30,'key',1)  # Search text, then cancel without launching a command.
        wait_for(lambda: not any(args and args[0]==b'wofi' for _,args in processes()),
                 'spatial launcher dismissal')
        print('PASS: Waybar opens a captured interactive launcher and Escape dismisses it',flush=True)
        # Restart a managed Waybar child; stale captures must disappear/recover.
        mapped = logs().count('spatial widget mapped')
        candidates = [pid for pid,args in processes() if args and args[0] == b'waybar']
        assert candidates
        managed = re.findall(r'spatial widget mapped role=1 id=[0-9]+ pid=([0-9]+)',logs())
        target = int(managed[-1])
        assert target in candidates, 'restart target is not a marked Waybar child'
        widget_aim(yaw=20*math.pi/180)
        control('button',272,1)
        app_events=len(events())
        os.kill(target,signal.SIGKILL)
        time.sleep(.3)
        control('button',272,0)
        assert not [e for e in events()[app_events:] if e['kind'] in ('press','release')], 'dead widget leaked a release'
        wait_for(lambda: logs().count('spatial widget mapped')>mapped,'Waybar child restart')
        time.sleep(.5)
        widget_aim(yaw=20*math.pi/180)
        before_clicks=widget_actions().count('click')
        control('button',272,1,'button',272,0)
        wait_for(lambda: widget_actions().count('click')>before_clicks,'click after Waybar restart')
        print('PASS: Waybar restart cancels a held click without leaking release and restores interaction',flush=True)
        mapped_launchers = logs().count('spatial widget mapped role=3')
        widget_aim(yaw=20*math.pi/180,role=0)
        control('button',272,1,'button',272,0)
        wait_for(lambda: logs().count('spatial widget mapped role=3')>mapped_launchers,'second launcher')
        restore()
        wait_for(lambda: not any(args and args[0] in (b'waybar',b'wofi') for _,args in processes()),
                 'widget and launcher cleanup on desktop exit')
        angles = [0.,0.,0.]
        time.sleep(.5)
        enter()
        print('PASS: desktop exit dismisses the launcher and removes all managed Waybar processes',flush=True)

    before = logs().count('output topology changed')
    parent_env = base_env | {'AIR_TEST_CONFIRM_DISPOSABLE': '1'}
    subprocess.run([str(DISCONNECT)], env=parent_env, check=True, timeout=5)
    wait_for(lambda: logs().count('output topology changed') > before, 'output disconnect recovery')
    wait_for(lambda: 'WL-1 on' in power(), 'remaining output restoration after disconnect')
    before_edit = len(events())
    control('key', 46)
    wait_for(lambda: any(e['kind'] == 'text' for e in events()[before_edit:]), 'client survived output disconnect')
    print('PASS: glasses-output disconnect restores a usable output without killing the existing editor', flush=True)

    if AUTO_START:
        attempts = logs().count('automatic preview for connected glasses')
        time.sleep(.8)
        assert logs().count('automatic preview for connected glasses') == attempts
        assert not any(str(HELPER).encode() == args[0] for _, args in processes())
        control('key', 67)  # F9: add the replacement on the sacrificial nested socket.
        wait_for(lambda: logs().count('automatic preview for connected glasses') > attempts,
                 'automatic preview on display reappearance')
        wait_for(lambda: 'WL-1 off' in power(), 'automatic re-entry without confirmation')
        assert 'WL-2 on' in power()
        control('key', 68)  # F10: remove that replacement; restore without a hotkey.
        wait_for(lambda: 'WL-1 on' in power(), 'automatic exit on removal')
        assert Path(f"/proc/{editor_pid}").exists()
        print('PASS: display reappearance re-enters, removal restores desktop, and existing clients survive', flush=True)

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
