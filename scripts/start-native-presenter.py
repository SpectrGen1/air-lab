#!/usr/bin/env python3
"""Wait for the session-owned tracker connection, then exec the private renderer."""
import json
import os
from pathlib import Path
import select
import sys
import time

from native_events import record


def main():
    events = os.environ.get('AIR_SPATIAL_EVENT_LOG')
    generation = int(os.environ.get('AIR_SPATIAL_GENERATION', '0'))
    renderer = os.environ['AIR_NATIVE_RENDERER']
    state_file = Path(os.environ['AIR_TRACKER_READY_FILE'])
    poll = select.poll()
    poll.register(int(os.environ['AIR_SPATIAL_FD']), select.POLLHUP | select.POLLERR)
    deadline = time.monotonic() + 100
    while time.monotonic() < deadline:
        try:
            state = json.loads(state_file.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            state = {}
        if (state.get('connected') and state.get('calibrated')
                and state.get('source_port')):
            os.environ['AIR_SPATIAL_SOURCE_PORT'] = str(state['source_port'])
            try:
                os.execv(renderer, [renderer, *sys.argv[1:]])
            except OSError as error:
                record(events, 'presenter', 'exec_failure', generation=generation, errno=error.errno)
                raise
        if poll.poll(250):
            return 1
    record(events, 'presenter', 'tracker_readiness_timeout', generation=generation)
    print('Air tracker is not ready. Check USB and the session tracker log.', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
