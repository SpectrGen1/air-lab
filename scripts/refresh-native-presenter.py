#!/usr/bin/env python3
"""Redirect an existing session's helper launcher after a renderer-only update."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    config_path = ROOT / '.local/native-install.json'
    config = json.loads(config_path.read_text())
    matches = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            exe = (proc / 'exe').readlink()
            if exe.name != 'labwc' or not exe.is_relative_to(ROOT / '.local/releases'):
                continue
            env = dict(item.split(b'=', 1) for item in (proc / 'environ').read_bytes().split(b'\0') if b'=' in item)
            if env.get(b'AIR_LAB_ROOT') != os.fsencode(ROOT):
                continue
            matches.append((proc, exe, env))
        except (OSError, ValueError):
            continue
    if len(matches) != 1:
        raise SystemExit('Expected exactly one running installed native session.')
    proc, exe, env = matches[0]
    if digest(exe) != digest(Path(config['labwc'])):
        raise SystemExit('The compositor changed too; save work and log in again instead.')
    launcher = Path(os.fsdecode(env[b'AIR_SPATIAL_HELPER']))
    if (not launcher.is_relative_to(ROOT / '.local/releases') or launcher.is_symlink()
            or launcher.name != 'start-native-presenter.py'):
        raise SystemExit('Unexpected live helper launcher; refusing to replace it.')
    if launcher == Path(config['presenter_launcher']):
        print('This session already uses the selected helper launcher.')
        return
    original = launcher.read_bytes()
    backup = ROOT / '.local/rollback' / ('presenter-launcher-' + hashlib.sha256(original).hexdigest() + '.py')
    if not backup.exists():
        shutil.copy2(launcher, backup)
    # Keep the running compositor and its inherited private connections. Only its
    # next helper spawn changes; binaries in the older release remain untouched.
    script = ('#!/usr/bin/python3\nimport os,sys\n'
              + 'os.environ["AIR_NATIVE_RENDERER"] = ' + repr(config['presenter']) + '\n'
              + 'os.execv("/usr/bin/python3", ["/usr/bin/python3", '
              + repr(config['presenter_launcher']) + ', *sys.argv[1:]])\n')
    temp = launcher.with_suffix('.air-new')
    temp.write_text(script)
    temp.chmod(0o755)
    subprocess.run([sys.executable, '-c', 'import ast,sys; ast.parse(open(sys.argv[1]).read())', str(temp)], check=True)
    config['live_helper_redirect'] = {'pid': int(proc.name), 'launcher': str(launcher),
                                    'backup': str(backup), 'sha256': hashlib.sha256(script.encode()).hexdigest()}
    config_path.write_text(json.dumps(config, indent=2) + '\n')
    temp.replace(launcher)
    print('The next spatial toggle will use the updated renderer; desktop clients are unchanged.')
    print('Previous helper launcher: ' + str(backup))


if __name__ == '__main__':
    main()
