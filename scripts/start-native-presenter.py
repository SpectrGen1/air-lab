#!/usr/bin/env python3
"""Wait for the session's stationary tracker warmup, then exec the private renderer."""
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import time


def main():
    renderer = os.environ['AIR_NATIVE_RENDERER']
    state_file = Path(os.environ['AIR_TRACKER_READY_FILE'])
    poll = select.poll()
    poll.register(int(os.environ['AIR_SPATIAL_FD']), select.POLLHUP | select.POLLERR)
    deadline = time.monotonic() + 100
    notified = False
    while time.monotonic() < deadline:
        try:
            state = json.loads(state_file.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            state = {}
        if (state.get('connected') and state.get('calibrated')
                and state.get('source_port') and time.monotonic() >= state.get('ready_at', float('inf'))):
            os.environ['AIR_SPATIAL_SOURCE_PORT'] = str(state['source_port'])
            os.execv(renderer, [renderer, *sys.argv[1:]])
        if not notified and shutil.which('notify-send'):
            subprocess.run(['notify-send', 'Air spatial mode',
                            'Leave the glasses motionless on a table for one minute. '
                            'The native view will open after calibration.'], check=False)
            notified = True
        if poll.poll(250):
            return 1
    print('Air tracker is not ready. Check USB and the session tracker log.', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
