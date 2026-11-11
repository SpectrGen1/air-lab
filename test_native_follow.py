"""Quiet tests of per-frame following and compositor-owned presented poses."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_cursor import function
ROOT=Path(__file__).resolve().parent

class WidgetFollowTests(unittest.TestCase):
    def test_per_frame_motion_uses_presented_geometry_and_freezes_for_interaction(self):
        source=(ROOT/'native/labwc/air-spatial.c').read_text()
        program=r'''
#include "air-math.h"
#include <assert.h>
enum {DESKTOP,ARMING,SPATIAL};
struct seat {struct {unsigned button_count;} pointer_state;};
static struct seat seat;
static struct {struct {struct seat *wlr_seat;} seat;} server={.seat={&seat}};
static struct {int mode,count;bool widget_hold,native_left_down;
 uint64_t epoch,ack;double widget_yaw;struct {struct air_panel panel;} scene[3];} xr;
static bool grabbing;
static bool wlr_seat_pointer_has_grab(struct seat *s){(void)s;return grabbing;}
''' +function(source,'widget_follow_allowed')+function(source,'widget_presented_yaw')+r'''
int main(void){
 xr.mode=ARMING;assert(!widget_follow_allowed());
 xr.mode=SPATIAL;xr.epoch=12;xr.ack=11;assert(widget_follow_allowed());
 /* Following does not wait for another static scene round trip. */
 xr.count=3;xr.scene[0].panel.flags=AIR_PANEL_WIDGET;
 xr.scene[1].panel.azimuth=.37;xr.scene[2].panel.flags=AIR_PANEL_WIDGET|AIR_PANEL_POPUP;
 for(int i=0;i<60;i++){
  double basis[9];abasis(i*.01,.2,0,basis);
  double yaw=awidget_frame_yaw(basis,true,-.5);
  assert(fabs(yaw-i*.01)<1e-9);
  double before=xr.widget_yaw;
  struct air_panel p={.width=700,.height=48};awidget_pose(&p,1,yaw);
  assert(xr.widget_yaw==before); /* Rendering cannot change compositor state. */
  widget_presented_yaw(yaw);
  assert(xr.widget_yaw==yaw&&xr.scene[0].panel.azimuth==yaw&&xr.scene[2].panel.azimuth==yaw);
  assert(xr.scene[1].panel.azimuth==.37&&xr.epoch==12&&xr.ack==11);
  struct air_vec center=acentre(&p);
  assert(fabs(hypot(center.x,center.z)-.6096)<1e-9&&fabs(center.y+.3048)<1e-9);
  assert(awidget_frame_yaw(basis,false,-.5)==-.5);
 }
 double saved=xr.widget_yaw;widget_presented_yaw(NAN);widget_presented_yaw(4);
 assert(xr.widget_yaw==saved);
 xr.widget_hold=true;assert(!widget_follow_allowed());xr.widget_hold=false;
 xr.native_left_down=true;assert(!widget_follow_allowed());xr.native_left_down=false;
 seat.pointer_state.button_count=1;assert(!widget_follow_allowed());seat.pointer_state.button_count=0;
 grabbing=true;assert(!widget_follow_allowed());grabbing=false;
 assert(widget_follow_allowed());xr.mode=DESKTOP;assert(!widget_follow_allowed());
}
'''
        with tempfile.TemporaryDirectory(prefix='air-follow-') as directory:
            path=Path(directory)/'follow.c';path.write_text(program)
            binary=Path(directory)/'follow'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',str(ROOT/'native/common'),str(path),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
