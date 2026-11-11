"""Compile the compositor's policy toggle without opening a graphical session."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class AutomaticToggleTests(unittest.TestCase):
    def test_toggle_disables_active_and_arming_modes_and_rearms_cleanly(self):
        source = (ROOT / 'native/labwc/air-spatial.c').read_text()
        a = source.index('static void toggle_automatic(void) {')
        toggle = source[a:source.index('\n}\n', a) + 3]
        program = r'''
#include "air-auto.h"
#include <assert.h>
#include <string.h>
enum {DESKTOP, ARMING, SPATIAL};
static struct { bool auto_start; struct air_auto automatic; uint64_t auto_poll; int mode;} xr;
static int stops, notifications;
static bool notified_state;
#define aevent_write(...) ((void)0)
static void notify_auto_state(bool enabled) { notifications++;notified_state=enabled; }
#define WLR_INFO 0
#define wlr_log(...) ((void)0)
static void air_spatial_stop(const char *reason) {
 assert(!strcmp(reason,"automatic spatial mode disabled"));
 stops++; xr.mode=DESKTOP;
}
''' + toggle + r'''
int main(void) {
 for(int mode=DESKTOP;mode<=SPATIAL;mode++) {
  xr.auto_start=true; xr.mode=mode;xr.auto_poll=123;
  xr.automatic=(struct air_auto){.present=true,.pending=true,.since=456};
  int before=stops;
  toggle_automatic();
  assert(!xr.auto_start && xr.mode==DESKTOP && stops==before+1);
  assert(!notified_state);
  assert(!xr.automatic.present && !xr.automatic.pending && !xr.auto_poll);
  toggle_automatic();
  assert(xr.auto_start && xr.mode==DESKTOP && stops==before+1);
  assert(notified_state);
  assert(!aauto_update(&xr.automatic,false,false,true,0)); /* Off face. */
  assert(!aauto_update(&xr.automatic,true,true,true,1000000000)); /* Locked. */
  assert(!aauto_update(&xr.automatic,true,true,true,2000000000));
  assert(aauto_update(&xr.automatic,true,false,true,2600000000)); /* Unlock. */
 }
 assert(notifications==6);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-auto-toggle-') as d:
            file = Path(d) / 'toggle.c'
            file.write_text(program)
            binary = Path(d) / 'toggle'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror',
                            '-I',str(ROOT/'native/common'),str(file),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
