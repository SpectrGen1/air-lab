#!/usr/bin/env python3
"""Quiet validation, with an optional isolated headless workspace check."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--workspace-headless', action='store_true',
                    help='Include the workspace test on private headless outputs')
options = parser.parse_args()

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / '.local/native-focused' / str(time.time_ns())
RUN.mkdir(parents=True)
checks = [
    [sys.executable, '-m', 'unittest', '-v'],
    ['sh', 'scripts/check-tracker.sh'],
    ['meson', 'test', '-C', '.local/build/air-native', '--no-rebuild', '--print-errorlogs'],
]
if options.workspace_headless:
    checks.append([sys.executable, 'scripts/check-native-workspace.py'])
for i, command in enumerate(checks):
    with (RUN / f'check-{i}.log').open('wb') as log:
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
result = {'passed': True, 'kind': 'focused', 'graphical_tests_run': False,
          'workspace_headless': options.workspace_headless, 'checks': checks,
          'limits': ['No on-screen nested run or physical glasses acceptance.']}
for key, path in {'labwc': '.local/build/labwc/labwc',
                  'presenter': '.local/build/air-native/air-presenter',
                  'tracker': '.local/build/xr-driver/xrDriver'}.items():
    result[key + '_sha256'] = hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
(RUN / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(RUN)
