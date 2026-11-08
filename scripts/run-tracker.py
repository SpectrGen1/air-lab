#!/usr/bin/env python3
"""Run the locally built tracker in UDP-only mode with project-local settings."""
import argparse
import fcntl
import os
from pathlib import Path

ROOT = Path(os.environ.get('AIR_LAB_ROOT', Path(__file__).resolve().parent.parent)).resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--udp-port', type=int, default=4243)
    options = parser.parse_args()
    if not 1 <= options.udp_port <= 65535:
        parser.error('invalid UDP port')
    binary = Path(os.environ.get('AIR_LAB_TRACKER_BINARY', ROOT / '.local/build/xr-driver/xrDriver'))
    if not binary.is_file():
        parser.error('run sh scripts/build-tracker.sh first')
    owner_dir = ROOT / '.local/tracker'
    owner_dir.mkdir(parents=True, exist_ok=True)
    owner = (owner_dir / 'owner.lock').open('a+')
    try:
        fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error('this lab already owns a tracker; stop it before starting another')
    # Hold this project lock through exec, before touching the running config.
    os.set_inheritable(owner.fileno(), True)
    # Keep the normal runtime directory: upstream uses its lock to exclude
    # another xrDriver instance. Only config/state/data are project-local.
    env = os.environ.copy()
    for key, name in [('XDG_CONFIG_HOME', 'config'), ('XDG_STATE_HOME', 'state'),
                      ('XDG_DATA_HOME', 'data')]:
        directory = ROOT / '.local/tracker' / name
        directory.mkdir(parents=True, exist_ok=True)
        env[key] = str(directory)
    config_dir = Path(env['XDG_CONFIG_HOME']) / 'xr_driver'
    config_dir.mkdir(exist_ok=True)
    (config_dir / 'config.ini').write_text(
        'disabled=false\noutput_mode=external_only\nexternal_mode=opentrack\n'
        f'opentrack_app_ip=127.0.0.1\nopentrack_app_port={options.udp_port}\n'
        'metrics_disabled=true\ngamescope_reshade_wayland_disabled=true\n'
        'opentrack_listener_enabled=false\n')
    # Upstream attempts license HTTP requests even for this free UDP mode.
    # Direct those libcurl requests to a closed loopback port; no hardware ID
    # needs to leave the workstation for this experiment. UDP is unaffected.
    for key in ('http_proxy', 'https_proxy', 'all_proxy', 'HTTP_PROXY',
                'HTTPS_PROXY', 'ALL_PROXY'):
        env[key] = 'http://127.0.0.1:9'
    env['no_proxy'] = env['NO_PROXY'] = '127.0.0.1,localhost'
    print('Tracker: UDP only at 127.0.0.1:' + str(options.udp_port), flush=True)
    print('Tracking starts after the device finishes its initial calibration.', flush=True)
    print('Log: ' + str(Path(env['XDG_STATE_HOME']) / 'xr_driver/driver.log'), flush=True)
    # exec means Ctrl+C, timeout, and the supervising terminal target the driver.
    os.execve(binary, [str(binary)], env)


if __name__ == '__main__':
    main()
