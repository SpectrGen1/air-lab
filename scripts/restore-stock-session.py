#!/usr/bin/env python3
"""Restore the previous login launcher without stopping the running compositor."""
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parent.parent
config = json.loads((ROOT / '.local/native-install.json').read_text())
entrypoint = Path(config['entrypoint'])
if hashlib.sha256(entrypoint.read_bytes()).hexdigest() != config['entrypoint_sha256']:
    raise SystemExit('The login launcher has newer edits. Review it against ' + config['backup'])
backup = Path(config['backup'])
temp = entrypoint.with_name(entrypoint.name + '.air-stock')
shutil.copy2(backup, temp)
temp.replace(entrypoint)
print('Original login launcher restored. The current desktop has not been stopped.')
