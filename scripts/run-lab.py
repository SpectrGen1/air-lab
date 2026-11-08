#!/usr/bin/env python3
"""Launch the preview, optionally with the local Air tracker; Ctrl+C stops both."""
import argparse
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pose', action='store_true', help='start the locally built hardware tracker')
    parser.add_argument('--udp-port', type=int, default=4243)
    parser.add_argument('--window', action='append', default=[], metavar='ID')
    parser.add_argument('--no-browser', action='store_true', help='print URL without opening a browser')
    opt = parser.parse_args()
    if len(opt.window) > 3 or not 1 <= opt.udp_port <= 65535:
        parser.error('use at most three windows and a valid UDP port')
    if opt.pose and not (ROOT / '.local/build/xr-driver/xrDriver').is_file():
        parser.error('run sh scripts/build-tracker.sh first')
    children = []

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    try:
        command = [sys.executable, str(ROOT / 'bridge.py')]
        if opt.pose:
            command += ['--pose', '--udp-port', str(opt.udp_port)]
        for window in opt.window:
            command += ['--window', window]
        bridge = subprocess.Popen(command, stdout=subprocess.PIPE, text=True)
        children.append(bridge)
        url = bridge.stdout.readline().strip()
        bridge.stdout.close()
        if not url.startswith('http://127.0.0.1:'):
            raise RuntimeError('Bridge failed to start; see error above.')
        print(url, flush=True)
        if opt.pose:
            children.append(subprocess.Popen([
                sys.executable, str(ROOT / 'scripts/run-tracker.py'),
                '--udp-port', str(opt.udp_port)]))
            print('Keep the glasses on and hold your head still for 60 seconds. '
                  'The browser will open after sensor bias calibration.', flush=True)
            warmup_until = time.monotonic() + 60
            while time.monotonic() < warmup_until:
                if any(child.poll() is not None for child in children):
                    raise RuntimeError('A lab process exited during calibration.')
                time.sleep(.25)
            print('Calibration warmup finished. Face forward, '
                  'then press R in the preview.', flush=True)
        if not opt.no_browser:
            subprocess.run(['setsid', '-f', 'xdg-open', url], check=True)
        print('Ctrl+C stops the lab. Esc releases the captured mouse.', flush=True)
        while all(child.poll() is None for child in children):
            time.sleep(.25)
        raise RuntimeError('A lab process exited; stopping the remaining process.')
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        return 1
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
        for child in children:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


if __name__ == '__main__':
    sys.exit(main())
