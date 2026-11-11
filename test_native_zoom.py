"""Quiet checks of the actual temporary zoom controls and narrowed frusta."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_cursor import function
ROOT = Path(__file__).resolve().parent

class ZoomTests(unittest.TestCase):
    def test_controls_release_and_visibility(self):
        source = (ROOT/'native/labwc/air-spatial.c').read_text()
        program = r'''
#include "air-math.h"
#include <assert.h>
enum {DESKTOP, ARMING, SPATIAL, RESTORING};
#define WLR_MODIFIER_LOGO 1
#define WLR_MODIFIER_ALT 2
#define WL_POINTER_AXIS_VERTICAL_SCROLL 0
static struct seat {void *current_constraint;struct {struct {unsigned button_count;} pointer_state;} *wlr_seat;} seat;
static struct {struct seat seat;} server;
static struct {double zoom;int mode,count;uint64_t pose_stamp;bool calibration;
 struct {bool paused;} watchdog;struct {unsigned region;} manipulation;} xr;
static unsigned mods;
static int packets,scenes;
static uint64_t now_ns(void){return 1000000000;}
static unsigned keyboard_get_all_modifiers(struct seat *s){(void)s;return mods;}
static bool send_packet(struct air_packet *m,int fd){
 assert(fd==-1&&m->type==AIR_ZOOM&&m->data.zoom.factor==xr.zoom);packets++;return true;
}
static bool scene_update(void){assert(xr.count==-1);scenes++;return true;}
''' + ''.join(function(source,n) for n in ('zoom_set','air_spatial_modifiers','zoom_axis')) + r'''
int main(void){
 typeof(*seat.wlr_seat) state={0};server.seat.wlr_seat=&state;
 xr.mode=SPATIAL;xr.zoom=1;xr.pose_stamp=now_ns();
 assert(!zoom_axis(0,-15)&&xr.zoom==1&&!packets);
 mods=3;assert(zoom_axis(0,-15)&&fabs(xr.zoom-1.1)<1e-9&&packets==1&&scenes==1);
 assert(zoom_axis(0,15)&&fabs(xr.zoom-1)<1e-9);
 for(int i=0;i<100;i++)zoom_axis(0,-15);
 assert(xr.zoom==3);
 int saved=packets;zoom_axis(0,-15);zoom_axis(0,NAN);zoom_axis(0,INFINITY);zoom_axis(1,-15);
 assert(xr.zoom==3&&packets==saved);
 air_spatial_modifiers(3);assert(xr.zoom==3);
 for(int m=0;m<3;m++){
  zoom_set(2);air_spatial_modifiers(m);assert(xr.zoom==1);
 }
 zoom_set(2);xr.watchdog.paused=true;assert(zoom_axis(0,-15)&&xr.zoom==2);
 air_spatial_modifiers(1);assert(xr.zoom==1);xr.watchdog.paused=false;
 xr.calibration=true;assert(zoom_axis(0,-15)&&xr.zoom==1);xr.calibration=false;
 xr.manipulation.region=AIR_TITLE;assert(zoom_axis(0,-15)&&xr.zoom==1);xr.manipulation.region=0;
 state.pointer_state.button_count=1;assert(zoom_axis(0,-15)&&xr.zoom==1);state.pointer_state.button_count=0;
 xr.pose_stamp=1;assert(zoom_axis(0,-15)&&xr.zoom==1);xr.pose_stamp=now_ns();
 for(int mode=DESKTOP;mode<=RESTORING;mode++){
  xr.mode=mode;xr.zoom=2;saved=packets;air_spatial_modifiers(0);
  assert(xr.zoom==1&&packets==saved+(mode==SPATIAL));
 }
 double basis[9];abasis(.4,.2,0,basis);
 struct air_vec p=aadd(abaxis(basis,2),ascale(abaxis(basis,0),.2));
 assert(avisible(p,basis,16./9));assert(!avisible_zoom(p,basis,16./9,3));
 assert(astereo_visible_center_zoom(abaxis(basis,2),basis,16./9,80,5,3));
 assert(!astereo_visible_center_zoom(p,basis,16./9,80,5,3));
 assert(fabs(azoom_step(1,-7.5)*1.0-sqrt(1.1))<1e-9);
 assert(fabs(azoom_step(azoom_step(1,-7.5),-7.5)-azoom_step(1,-15))<1e-9);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-zoom-') as d:
            path=Path(d)/'zoom.c';path.write_text(program)
            binary=Path(d)/'zoom'
            subprocess.run(['cc','-std=gnu17','-Wall','-Wextra','-Werror','-I',str(ROOT/'native/common'),str(path),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
