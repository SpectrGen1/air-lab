"""Quiet regressions for transition deadlines and unchanged physical outputs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent


class DisplayChangeTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory(prefix='air-display-check-') as d:
            src=Path(d)/'check.c';src.write_text(code)
            out=Path(d)/'check'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',str(ROOT/'native/common'),str(src),'-o',str(out)],check=True)
            subprocess.run([str(out)],check=True)

    def test_selected_mode_matches_confirmed_hardware_and_transport(self):
        self.compile_run(r'''
#include "air-auto.h"
#include <assert.h>
int main(void) {
 assert(aselected_display_mode(true,0,1920,1080,1,0)==0);
 assert(aselected_display_mode(true,1,3840,1080,1,0)==1);
 assert(aselected_display_mode(false,0,1920,1080,1,0)==0);
 assert(aselected_display_mode(false,1,3840,1080,1,0)==-1);
 assert(aselected_display_mode(true,0,3840,1080,1,0)==-1);
 assert(aselected_display_mode(true,1,1920,1080,1,0)==-1);
 assert(aselected_display_mode(true,-1,1920,1080,1,0)==-1);
 assert(aselected_display_mode(true,1,3840,1080,2,0)==-1);
 assert(aselected_display_mode(true,1,3840,1080,1,1)==-1);
 assert(aselected_display_mode(true,1,3840,1200,1,0)==-1);
}
''')

    def test_unchanged_heads_skip_commit_but_real_changes_and_pending_state_do_not(self):
        # Extract the tracked patch's real helper rather than relying on a build
        # checkout or reproducing the comparison in the regression.
        patch=(ROOT/'patches/labwc-native-spatial.patch').read_text()
        added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
        a=added.index('static bool\noutput_config_unchanged(')
        helper=added[a:added.index('\n}',a)+2]
        self.compile_run(r'''
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#define WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED 1
struct wlr_output_mode {int dummy;};
struct wlr_output {bool enabled;struct wlr_output_mode *current_mode;int width,height,refresh;float scale;int transform,adaptive_sync_status;};
struct output {struct {unsigned committed;} pending;};
struct wlr_output_layout_output {int x,y;};
struct wlr_output_configuration_head_v1 {struct {struct wlr_output *output;bool enabled;struct wlr_output_mode *mode;struct {int width,height,refresh;} custom_mode;float scale;int transform,x,y;bool adaptive_sync_enabled;} state;};
static struct output physical;
static struct wlr_output_layout_output position={3440,0};
static bool in_layout=true;
static struct {void *output_layout;} server;
static struct output *output_from_wlr_output(struct wlr_output *o){(void)o;return &physical;}
static struct wlr_output_layout_output *wlr_output_layout_get(void *l,struct wlr_output *o){(void)l;(void)o;return in_layout?&position:NULL;}
'''+helper+r'''
int main(void) {
 struct wlr_output_mode mode={0},other={1};
 struct wlr_output o={.enabled=true,.current_mode=&mode,.width=3440,.height=1440,.refresh=60000,.scale=1};
 struct wlr_output_configuration_head_v1 h={.state={.output=&o,.enabled=true,.mode=&mode,.scale=1,.x=3440}};
 assert(output_config_unchanged(&h));
 physical.pending.committed=1;assert(!output_config_unchanged(&h));physical.pending.committed=0;
 h.state.enabled=false;assert(!output_config_unchanged(&h));h.state.enabled=true;
 h.state.mode=&other;assert(!output_config_unchanged(&h));h.state.mode=&mode;
 h.state.x=0;assert(!output_config_unchanged(&h));h.state.x=3440;
 h.state.scale=2;assert(!output_config_unchanged(&h));h.state.scale=1;
 h.state.transform=1;assert(!output_config_unchanged(&h));h.state.transform=0;
 h.state.adaptive_sync_enabled=true;assert(!output_config_unchanged(&h));h.state.adaptive_sync_enabled=false;
 in_layout=false;assert(!output_config_unchanged(&h));
 o.enabled=h.state.enabled=false;assert(output_config_unchanged(&h));
 in_layout=true;assert(!output_config_unchanged(&h));
 o.enabled=h.state.enabled=true;o.current_mode=NULL;h.state.mode=NULL;
 h.state.custom_mode.width=3440;h.state.custom_mode.height=1440;h.state.custom_mode.refresh=60000;
 assert(output_config_unchanged(&h));h.state.custom_mode.refresh=59999;assert(!output_config_unchanged(&h));
}
''')
