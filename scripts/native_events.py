"""Private per-login event records for Air Lab lifecycle diagnostics."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import sys
import tempfile

KEEP_SESSIONS = 20


def open_session(state_dir):
    directory = state_dir / 'events'
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    fd, path = tempfile.mkstemp(prefix=f'session-{stamp}-', suffix='.jsonl', dir=directory)
    os.close(fd)
    path = Path(path)
    temporary = path.with_suffix('.link')
    temporary.symlink_to(path.name)
    temporary.replace(directory / 'latest.jsonl')
    for old in sorted(directory.glob('session-*.jsonl'), reverse=True)[KEEP_SESSIONS:]:
        old.unlink(missing_ok=True)
    return path


def record(path, component, event, **fields):
    if not path:
        return
    data = {'ts': datetime.now(timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z'),
            'component': component, 'event': event, 'pid': os.getpid(), **fields}
    try:
        fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            os.write(fd, (json.dumps(data, separators=(',', ':')) + '\n').encode())
        finally:
            os.close(fd)
    except OSError as error:
        print(f'Air diagnostics: {error}', file=sys.stderr, flush=True)


def child_exit(path, component, child, *, termination_requested):
    code = child.returncode
    number = -code if code < 0 else None
    record(path, component, 'process_exit', child_pid=child.pid, returncode=code,
           signal=number, signal_name=signal.strsignal(number) if number else None,
           termination_requested=termination_requested)
