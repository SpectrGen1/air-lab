"""Waybar configuration reuse without modifying the user's desktop bar."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('widgets', Path(__file__).parent / 'scripts/start-native-widgets.py')
widgets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(widgets)


class TestNativeWidgets(unittest.TestCase):
    def test_jsonc_preserves_commands_urls_and_escaped_quotes(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'config'
            expected = {'custom/test': {'on-click': 'printf "https://example.test/a/*b*/,}"',
                                        'format': '// not a comment'}}
            text = json.dumps(expected)
            path.write_text('/* header */\n' + text[:-1] + ', // trailing comma\n}\n')
            self.assertEqual(widgets.read_jsonc(path), expected)

    def test_includes_and_role_split_preserve_actions_without_editing_source(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            child = root / 'modules.jsonc'
            child.write_text(json.dumps({'custom/test': {'on-click': 'echo action'}}))
            source = root / 'config'
            original = json.dumps({'include':'modules.jsonc', 'modules-left':['custom/test'],
                                   'modules-center':['clock'], 'modules-right':['pulseaudio'],
                                   'exclusive':True, 'output':'DP-2'})
            source.write_text(original)
            config = widgets.load_config(source)
            for role, module in enumerate(['custom/test','clock','pulseaudio']):
                row = widgets.widget_config(config, role, 'DP-4')
                self.assertEqual(row['modules-center'], [module])
                self.assertEqual(row['modules-left'], [])
                self.assertEqual(row['modules-right'], [])
                self.assertEqual(row['custom/test']['on-click'], 'echo action')
                self.assertEqual(row['output'], 'DP-4')
                self.assertFalse(row['exclusive'])
            self.assertEqual(source.read_text(), original)
            child.write_text('{"include":"config"}')
            with self.assertRaises(ValueError):
                widgets.load_config(source)

    def test_spatial_taskbar_sees_clients_on_sleeping_desktop_outputs(self):
        config = {'modules-left':['wlr/taskbar'],
                  'wlr/taskbar':{'all-outputs':False, 'on-click':'activate'}}
        row = widgets.widget_config(config,0,'DP-4')
        self.assertTrue(row['wlr/taskbar']['all-outputs'])
        self.assertEqual(row['wlr/taskbar']['on-click'],'activate')
        self.assertFalse(config['wlr/taskbar']['all-outputs'])
