#!/usr/bin/env python3
"""Exec one compact Waybar row; only compositor-owned PIDs become spatial widgets."""
import json
import os
from pathlib import Path
import sys


def read_jsonc(path):
    text = Path(path).read_text()
    result = []
    i = 0
    quoted = False
    while i < len(text):
        c = text[i]
        if quoted:
            result.append(c)
            if c == '\\' and i + 1 < len(text):
                i += 1
                result.append(text[i])
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
            result.append(c)
        elif text[i:i+2] == '//':
            i = text.find('\n', i)
            if i < 0:
                break
            result.append('\n')
        elif text[i:i+2] == '/*':
            end = text.find('*/', i + 2)
            if end < 0:
                raise ValueError('Unterminated Waybar config comment')
            result.append(' ')
            i = end + 1
        else:
            result.append(c)
        i += 1
    text = ''.join(result)
    result = []
    quoted = False
    i = 0
    while i < len(text):
        c = text[i]
        if quoted:
            result.append(c)
            if c == '\\' and i + 1 < len(text):
                i += 1
                result.append(text[i])
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
            result.append(c)
        elif c != ',' or not text[i+1:].lstrip().startswith((']', '}')):
            result.append(c)
        i += 1
    return json.loads(''.join(result))


def load_config(path, seen=None):
    path = Path(path).expanduser().resolve()
    seen = set() if seen is None else seen
    if path in seen:
        raise ValueError('Waybar config include cycle')
    seen.add(path)
    config = read_jsonc(path)
    if isinstance(config, list):
        config = config[0]
    if not isinstance(config, dict):
        raise ValueError('Waybar config must be an object or a nonempty array')
    includes = config.pop('include', [])
    if isinstance(includes, str):
        includes = [includes]
    for include in includes:
        include_path = Path(os.path.expandvars(include)).expanduser()
        if not include_path.is_absolute():
            include_path = path.parent / include_path
        for key, value in load_config(include_path, seen.copy()).items():
            config.setdefault(key, value)
    return config


def widget_config(config, role, output):
    config = dict(config)
    keys = ('modules-left', 'modules-center', 'modules-right')
    modules = config.get(keys[role], [])
    if not modules:
        modules = ['clock'] if role == 1 else ['pulseaudio'] if role == 2 else ['wlr/taskbar']
    for module in modules:
        if module.split('#', 1)[0] == 'wlr/taskbar':
            taskbar = dict(config.get(module, {}))
            # Canonical windows remain on the sleeping desktop outputs.
            taskbar['all-outputs'] = True
            config[module] = taskbar
    for key in keys:
        config[key] = modules if key == 'modules-center' else []
    config.update(output=output, name='air-widget-' + str(role), layer='overlay',
                  position='bottom', exclusive=False, passthrough=False,
                  width=1100 if role == 2 else 700, height=48, spacing=8, margin=0,
                  **{'margin-top':0, 'margin-right':0, 'margin-left':0,
                     'margin-bottom':role * 60, 'ipc':False, 'start_hidden':False})
    config.pop('mode', None)
    return config


def main():
    role, output = int(sys.argv[1]), sys.argv[2]
    os.environ['GDK_BACKEND'] = 'wayland'
    os.environ['NO_AT_BRIDGE'] = '1'
    if role not in range(3):
        raise SystemExit('Expected widget role 0, 1 or 2')
    root = Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'waybar'
    path = os.environ.get('AIR_WIDGET_CONFIG') or next(
        (str(root / name) for name in ('config', 'config.jsonc') if (root / name).is_file()), None)
    config = load_config(path) if path else {}
    style = Path(os.environ.get('AIR_WIDGET_STYLE', root / 'style.css'))
    css = '@import url(' + json.dumps(style.resolve().as_uri()) + ');\n' if style.is_file() else ''
    css += '\nwindow#waybar { background: #142129; color: #e1f2f0; border: 1px solid #50857d; border-radius: 10px; }\n'
    css += 'window#waybar .modules-left, window#waybar .modules-right { background: transparent; border: none; box-shadow: none; padding: 0; margin: 0; min-width: 0; }\n'
    fds = []
    for name, data in [('air-widget-config', json.dumps(widget_config(config, role, output))),
                       ('air-widget-style', css)]:
        fd = os.memfd_create(name, 0)
        os.write(fd, data.encode())
        os.lseek(fd, 0, os.SEEK_SET)
        os.set_inheritable(fd, True)
        fds.append(fd)
    os.execvp('waybar', ['waybar', '-c', '/proc/self/fd/' + str(fds[0]),
                         '-s', '/proc/self/fd/' + str(fds[1])])


if __name__ == '__main__':
    main()
