"""Hardware-free checks for launcher ownership and inherited private sockets."""
import json
import importlib.util
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent


class TestNativeLaunchers(unittest.TestCase):
    def test_ready_notifications_follow_warmup_and_reconnection(self):
        spec = importlib.util.spec_from_file_location('native_session', ROOT / 'scripts/start-native-desktop.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        messages = []
        notifications = module.TrackerNotifications(messages.append)
        state = dict(connected=True, calibrated=False, source_port=None, ready_at=60, pid=1)
        notifications.observe(state, 0)
        self.assertEqual(len(messages), 1)
        state.update(calibrated=True, source_port=12345)
        notifications.observe(state, 59)
        self.assertEqual(len(messages), 1)
        state.update(source_port=None)
        notifications.observe(state, 60)
        self.assertEqual(len(messages), 1)
        state.update(source_port=12345)
        notifications.observe(state, 60)
        notifications.observe(state, 90)
        self.assertEqual(len(messages), 2)
        self.assertIn('Glasses ready', messages[-1])
        notifications.observe(dict(connected=False), 91)
        state.update(ready_at=151, calibrated=False)
        notifications.observe(state, 91)
        notifications.observe(state, 160)
        self.assertEqual(len(messages), 4)
        state.update(calibrated=True)
        notifications.observe(state, 161)
        self.assertEqual(len(messages), 5)
        self.assertIn('Keep the glasses on', messages[0])
        self.assertIn('disconnected or asleep', messages[2])
        notifications.observe(dict(connected=False), 162)
        notifications.observe(dict(connected=False), 163)
        self.assertEqual(len(messages), 6)
        self.assertIn('disconnected or asleep', messages[-1])

    def test_notification_delivery_retries_and_replaces_persistently(self):
        from unittest.mock import patch
        spec = importlib.util.spec_from_file_location('native_session', ROOT / 'scripts/start-native-desktop.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        notifications = module.TrackerNotifications()
        state = dict(connected=True, calibrated=True, source_port=12345, ready_at=60, pid=1)
        results = [subprocess.CompletedProcess([], 1, '', 'bus unavailable'),
                   subprocess.CompletedProcess([], 0, '42\n', ''),
                   subprocess.CompletedProcess([], 0, '42\n', ''),
                   subprocess.CompletedProcess([], 0, '42\n', '')]
        with patch.dict(os.environ, {}, clear=True), patch.object(module.subprocess, 'run', side_effect=results) as run:
            notifications.observe(state, 0)
            notifications.observe(state, 1)
            self.assertEqual(run.call_count, 1)
            notifications.observe(state, 5)
            notifications.observe(state, 60)
            notifications.observe(dict(connected=False), 61)
            calls = [call.args[0] for call in run.call_args_list]
            self.assertEqual(len(calls), 4)
            for command in calls:
                self.assertEqual(command[command.index('-t') + 1], '0')
            self.assertEqual(calls[2][calls[2].index('-r') + 1], '42')
            self.assertIn('Glasses ready', calls[2][-1])
            self.assertIn('disconnected or asleep', calls[3][-1])

    def test_tracker_lock_prevents_reconfiguring_running_reader(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'scripts').mkdir()
            shutil.copyfile(ROOT / 'scripts/run-tracker.py', root / 'scripts/run-tracker.py')
            binary = root / '.local/build/xr-driver/xrDriver'
            binary.parent.mkdir(parents=True)
            binary.write_text('#!/bin/sh\nexec /bin/sleep 30\n')
            binary.chmod(0o700)
            env = dict(os.environ)
            env.pop('AIR_LAB_ROOT', None)
            env.pop('AIR_LAB_TRACKER_BINARY', None)
            first = subprocess.Popen([sys.executable, str(root / 'scripts/run-tracker.py')],
                                     env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                self.assertTrue(first.stdout.readline().startswith('Tracker:'))
                config = root / '.local/tracker/config/xr_driver/config.ini'
                before = config.read_text()
                second = subprocess.run([sys.executable, str(root / 'scripts/run-tracker.py'),
                                         '--udp-port', '4343'], env=env, capture_output=True, text=True, timeout=5)
                self.assertEqual(second.returncode, 2)
                self.assertIn('already owns a tracker', second.stderr)
                self.assertEqual(config.read_text(), before)
            finally:
                first.terminate()
                first.communicate(timeout=5)

    def test_identify_owned_udp_socket(self):
        spec = importlib.util.spec_from_file_location('native_session', ROOT / 'scripts/start-native-desktop.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.bind(('127.0.0.1', 0))
            self.assertEqual(module.tracker_source_port(os.getpid()), sender.getsockname()[1])

    def test_private_socket_survives_presenter_wrapper_exec(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ready = directory / 'ready.json'
            ready.write_text(json.dumps({'connected': True, 'calibrated': True, 'ready_at': 0, 'source_port': 12345}))
            renderer = directory / 'renderer'
            renderer.write_text('#!' + sys.executable + '\nimport os,socket\n'
                                's=socket.socket(fileno=int(os.environ["AIR_SPATIAL_FD"]))\n'
                                's.send(b"inherited private connection")\n')
            renderer.chmod(0o700)
            parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            parent.settimeout(5)
            env = os.environ | {'AIR_NATIVE_RENDERER': str(renderer),
                                'AIR_TRACKER_READY_FILE': str(ready), 'AIR_SPATIAL_FD': str(child.fileno())}
            with parent, child:
                process = subprocess.Popen([sys.executable, str(ROOT / 'scripts/start-native-presenter.py'),
                                            '--output', 'WL-2'], env=env, pass_fds=(child.fileno(),))
                try:
                    self.assertEqual(parent.recv(1024), b'inherited private connection')
                    self.assertEqual(process.wait(timeout=5), 0)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()


if __name__ == '__main__':
    unittest.main()
