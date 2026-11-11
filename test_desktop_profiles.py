"""Pure config transforms, never start kanshi or touch physical outputs."""
import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('desktop_profiles',ROOT/'scripts/set-desktop-monitor-mode.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)


class DesktopProfilesTests(unittest.TestCase):
    config='''# preserve aliases and comments
output "Nreal nreal air 0x88888800" alias $xreal
profile dual-xreal {
 output $dell enable
 output $lg enable
 output $xreal enable mode 1920x1080@60Hz
}
profile dual-static {
 output $dell enable position 0,0
 output $lg enable position 3440,0
}
profile lg-only {
 output $lg enable position 0,0
 output $dell disable
}
profile travel {
 output eDP-1 enable
}
'''
    def test_air_no_longer_chooses_desktop_count_and_selected_profile_is_first(self):
        single=module.desktop_profiles(self.config,'lg-only')
        self.assertNotIn('profile dual-xreal',single)
        self.assertEqual(single.count('output $xreal enable position 6880,0'),1)
        self.assertEqual(single.count('output $xreal enable position 3440,0'),1)
        self.assertNotIn('...output',single)
        self.assertNotIn('mode 1920x1080',single)
        self.assertLess(single.index('profile lg-only'),single.index('profile dual-static'))
        self.assertIn('profile travel',single)
        self.assertEqual(single,module.desktop_profiles(single,'lg-only'))
        dual=module.desktop_profiles(single,'dual-static')
        self.assertLess(dual.index('profile dual-static'),dual.index('profile lg-only'))
        self.assertIn('output $dell disable',dual)

    def test_both_profiles_match_with_or_without_air_and_preserve_stereo_mode(self):
        result=module.desktop_profiles(self.config,'dual-static')
        import re
        for name in ('dual-static','lg-only'):
            for air in (False,True):
                variant=name if air else name+'-unplugged'
                block=re.search(r'profile '+variant+r' \{([^{}]*)\}',result).group(1)
                required=set(re.findall(r'^\s*output (\$\w+)',block,re.M))
                connected={'$dell','$lg'}|({'$xreal'} if air else set())
                self.assertEqual(required,connected)
                # Exact matching makes hotplug select a new profile and enable
                # the new head, while retaining the desktop-count preference.
                self.assertNotEqual(required,connected^{'$xreal'})
                self.assertNotRegex(block,r'\$xreal[^\n]*\bmode\b')

    def test_unknown_or_missing_profile_cannot_overwrite_configuration(self):
        with self.assertRaises(ValueError):module.desktop_profiles(self.config,'other')
        with self.assertRaises(ValueError):module.desktop_profiles('profile other {}','dual-static')
