"""Handoff lifetime checks; no actual processes or signals."""
import importlib.util
from pathlib import Path
import signal
import unittest
from unittest.mock import patch,call

ROOT=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('tracker_handoff',ROOT/'scripts/restart-native-tracker.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)


class TrackerHandoffTests(unittest.TestCase):
    def test_original_stereo_controller_is_resumed_even_if_replacement_fails(self):
        for failure in (None,RuntimeError('replacement failed')):
            with patch.object(module,'find_session',return_value=(Path('/proc/123'),{})), \
                 patch.object(module,'parent_pid',return_value=25), \
                 patch.object(module.os,'pidfd_open',side_effect=[10,20]), \
                 patch.object(module.os,'close') as close, \
                 patch.object(module.signal,'pidfd_send_signal') as send, \
                 patch.object(module,'supervise_owned',side_effect=failure) as own:
                if failure:
                    with self.assertRaises(RuntimeError):module.supervise()
                else:module.supervise()
                own.assert_called_once_with(Path('/proc/123'),{},10,20)
                self.assertEqual(send.call_args_list,[call(10,signal.SIGSTOP),call(10,signal.SIGCONT)])
                self.assertEqual(close.call_args_list,[call(10),call(20)])
