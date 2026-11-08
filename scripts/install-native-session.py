#!/usr/bin/env python3
"""Stage tested native binaries for the next login; never restart the current desktop."""
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.local/build'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def configure_notifications(rollback_dir, date):
    """Keep warmup visible; use the normal five-second timeout once ready."""
    path = Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'mako/config'
    if not path.is_file():
        return
    marker = '# Air Spatial Lab tracker readiness'
    old_rule = (marker + '\n[app-name="Air Spatial Lab"]\n'
                'ignore-timeout=0\ndefault-timeout=0\n')
    rule = (old_rule + '\n[app-name="Air Spatial Lab" body~="^Glasses ready[.]"]\n'
            'ignore-timeout=1\ndefault-timeout=5000\n')
    before = path.read_text()
    if marker in before:
        if before.endswith(rule):
            return
        if not before.endswith(old_rule):
            raise SystemExit('Air notification rule was edited; inspect the mako config before installing.')
        updated = before[:-len(old_rule)] + rule
    else:
        updated = before.rstrip() + '\n\n' + rule
    backup = rollback_dir / ('mako.before-air-notifications-' + date)
    shutil.copy2(path, backup)
    path.write_text(updated)
    if shutil.which('makoctl'):
        result = subprocess.run(['makoctl', 'reload'], capture_output=True, text=True)
        if result.returncode:
            path.write_text(before)
            raise SystemExit('Could not reload mako; restored its config: ' + result.stderr.strip())
    print('Ready notices use the normal five-second timeout. Mako backup: ' + str(backup))


def main():
    if os.geteuid() == 0:
        raise SystemExit('Run as the desktop user, without sudo.')
    binaries = {'labwc': BUILD / 'labwc/labwc', 'presenter': BUILD / 'air-native/air-presenter',
                'tracker': BUILD / 'xr-driver/xrDriver'}
    for path in binaries.values():
        if not path.is_file():
            raise SystemExit('Missing build: ' + str(path))
    tests = sorted((ROOT / '.local/native-check').iterdir(), key=lambda path: path.name)
    if not tests or not (tests[-1] / 'result.json').exists():
        raise SystemExit('The most recent native integration test has not passed.')
    result = json.loads((tests[-1] / 'result.json').read_text())
    if not result.get('passed') or any(result[key + '_sha256'] != digest(binaries[key])
                                       for key in ('labwc', 'presenter')):
        raise SystemExit('Native binaries changed since the last passing integration test.')
    launchers = ['start-native-desktop.py', 'start-native-presenter.py', 'run-tracker.py']
    release_inputs = [*binaries.values(), *(ROOT / 'scripts' / name for name in launchers)]
    release_id = hashlib.sha256(''.join(digest(path) for path in release_inputs).encode()).hexdigest()[:16]
    release = ROOT / '.local/releases' / release_id
    release.mkdir(parents=True, exist_ok=True)
    for key, source in binaries.items():
        target = release / key
        if target.exists() and digest(target) != digest(source):
            raise SystemExit('Existing release differs: ' + str(target))
        if not target.exists():
            shutil.copy2(source, target)
            target.chmod(0o755)
    (release / 'scripts').mkdir(exist_ok=True)
    for name in launchers:
        target = release / 'scripts' / name
        source = ROOT / 'scripts' / name
        if target.exists() and target.read_bytes() != source.read_bytes():
            raise SystemExit('Release launcher differs; rebuild/test and create a new release.')
        if not target.exists():
            shutil.copy2(source, target)
            target.chmod(0o755)
    entrypoint = Path.home() / '.local/bin/start-labwc-logged'
    previous = entrypoint.read_text()
    original = 'dbus-run-session labwc -V >>"$log" 2>&1'
    date = datetime.now(ZoneInfo('America/Denver')).strftime('%Y%m%d-%H%M%S')
    rollback_dir = ROOT / '.local/rollback'
    rollback_dir.mkdir(parents=True, exist_ok=True)
    if previous.count(original) == 1:
        backup = rollback_dir / ('start-labwc-logged.before-native-' + date)
        shutil.copy2(entrypoint, backup)
    else:
        prior = json.loads((ROOT / '.local/native-install.json').read_text())
        if hashlib.sha256(previous.encode()).hexdigest() != prior['entrypoint_sha256']:
            raise SystemExit('The login launcher has newer edits; review before updating it.')
        shutil.copy2(entrypoint, rollback_dir / ('start-labwc-logged.previous-native-' + date))
        backup = Path(prior['backup'])
        previous = backup.read_text()
        if previous.count(original) != 1:
            raise SystemExit('The original stock backup does not match the inspected launcher.')
    command = 'dbus-run-session env ' + shlex.quote('AIR_LAB_ROOT=' + str(ROOT)) + ' /usr/bin/python3 ' + shlex.quote(str(release / 'scripts/start-native-desktop.py')) + ' -V >>"$log" 2>&1'
    updated = previous.replace(original, command).replace(
        "printf 'command=dbus-run-session labwc -V\\n'", "printf 'command=Air Spatial Lab native session; AIR_USE_STOCK=1 selects stock labwc\\n'")
    temp = entrypoint.with_name(entrypoint.name + '.air-new')
    shutil.copy2(entrypoint, temp)
    temp.write_text(updated)
    subprocess.run(['sh', '-n', str(temp)], check=True)
    config = {key: str(release / key) for key in binaries}
    config.update({'output': 'DP-4', 'udp_port': 4243, 'power': True,
                   'presenter_launcher': str(release / 'scripts/start-native-presenter.py'),
                   'tracker_launcher': str(release / 'scripts/run-tracker.py'),
                   'entrypoint': str(entrypoint), 'backup': str(backup),
                   'entrypoint_sha256': hashlib.sha256(updated.encode()).hexdigest(),
                   'validation': str(tests[-1])})
    install_config = ROOT / '.local/native-install.json'
    if install_config.exists():
        prior = json.loads(install_config.read_text())
        for key in ('live_tracker', 'live_helper_redirect'):
            if key in prior:
                config[key] = prior[key]
        config['panel_scale'] = prior.get('panel_scale', 2 / 3)
        config['head_roll'] = prior.get('head_roll', False)
        config['pointer_speed'] = prior.get('pointer_speed', 1)
    else:
        config.update(panel_scale=2 / 3, head_roll=False, pointer_speed=1)
    configure_notifications(rollback_dir, date)
    install_config.write_text(json.dumps(config, indent=2) + '\n')
    temp.replace(entrypoint)
    fallback = Path.home() / '.local/bin/air-spatial-stock'
    fallback.write_text('#!/bin/sh\nexport AIR_USE_STOCK=1\nexec ' + shlex.quote(str(entrypoint)) + '\n')
    fallback.chmod(0o755)
    print('Staged native session for the next login. Current desktop is unchanged.')
    print('Release: ' + str(release))
    print('Rollback backup: ' + str(backup))
    print('One-session stock fallback: ' + str(fallback))


if __name__ == '__main__':
    main()
