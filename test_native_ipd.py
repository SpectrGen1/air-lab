"""Quiet regression of the presenter's actual live settings handler."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_cursor import function

ROOT = Path(__file__).resolve().parent


class LiveIPDTests(unittest.TestCase):
    def test_live_packet_updates_both_eye_offsets_and_feedback(self):
        source = (ROOT / 'native/presenter/presenter.c').read_text()
        program = r'''
#include "air-ipd.h"
#include <assert.h>
#include <stdio.h>
struct app {
 bool running,stereo,calibration,stereo_received;
 double ipd_mm,ipd_center_mm,ipd_baseline_mm;
 uint64_t ipd_feedback_until;
};
static uint64_t clock_ns=100;
static uint64_t now_ns(void){return clock_ns;}
''' + function(source, 'apply_stereo') + r'''
int main(void){
 struct app a={.running=true,.ipd_mm=64};
 struct air_packet m={.type=AIR_STEREO};
 m.data.stereo.enabled=1;m.data.stereo.ipd_mm=64;m.data.stereo.baseline_mm=63.5;
 apply_stereo(&a,&m);
 assert(a.stereo&&a.ipd_mm==64&&!a.ipd_feedback_until);
 m.data.stereo.ipd_mm=70;apply_stereo(&a,&m);
 assert(a.ipd_mm==70&&a.ipd_feedback_until==clock_ns+2500000000ULL);
 assert(fabs(aeye_offset(0,a.ipd_mm)+.035)<1e-9);
 assert(fabs(aeye_offset(1,a.ipd_mm)-.035)<1e-9);
 clock_ns+=100;m.data.stereo.calibration=1;apply_stereo(&a,&m);
 assert(a.calibration&&a.ipd_feedback_until==2500000100ULL);
 m.data.stereo.ipd_mm=64;apply_stereo(&a,&m);
 assert(a.ipd_mm==64&&a.ipd_feedback_until==clock_ns+2500000000ULL);
 m.data.stereo.ipd_mm=63.5;m.data.stereo.center_mm=.25;apply_stereo(&a,&m);
 assert(a.ipd_center_mm==.25 && a.ipd_baseline_mm==63.5);
 assert(fabs(aeye_offset_center(0,a.ipd_mm,a.ipd_center_mm)+.0315)<1e-9);
 assert(fabs(aeye_offset_center(1,a.ipd_mm,a.ipd_center_mm)-.032)<1e-9);
 m.data.stereo.ipd_mm=NAN;apply_stereo(&a,&m);
 assert(!a.running&&a.ipd_mm==63.5);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-ipd-check-') as directory:
            path = Path(directory) / 'ipd.c'
            path.write_text(program)
            binary = Path(directory) / 'ipd'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'native/common'), str(path), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_guide_eye_labels_and_fixed_references_restore_live_eye_offset(self):
        source = (ROOT / 'native/presenter/presenter.c').read_text()
        program = r'''
#include "air-ipd.h"
#include <assert.h>
#include <stdio.h>
#include <cairo/cairo.h>
#define WL_SHM_FORMAT_ARGB8888 0
#define GLuint unsigned
struct app {
 bool calibration;
 double ipd_mm,ipd_center_mm,ipd_baseline_mm,calibration_ipd,calibration_center,calibration_baseline;
 GLuint calibration_texture,eye_texture[2],reference_texture,depth_texture[2];
 int uniform_eye;
};
static float uniform;
static int draws;
static void glUniform1f(int location,float value){(void)location;uniform=value;}
static void texture(GLuint *id,const void *pixels,int width,int height,int stride,unsigned format){
 static unsigned next=1;(void)pixels;(void)width;(void)height;(void)stride;(void)format;
 if(!*id)*id=next++;
}
static void camera_quad(struct app *a,GLuint tex,double x,double y,double z,double w,double h){
 draws++;
 /* Every guide quad must fit the real per-eye 40-degree viewport, including
  * both IPD extremes. This catches labels placed above the visible area. */
 double basis[9];abasis(0,0,0,basis);
 for(int i=0;i<4;i++) {
  struct air_vec corner=av(x+(i%2 ? w/2 : -w/2)-uniform,
                          y+(i<2 ? h/2 : -h/2),z);
  assert(avisible(corner,basis,16./9));
 }
 if(tex==a->reference_texture || tex==a->eye_texture[0] || tex==a->eye_texture[1])assert(uniform==0);
 else assert(fabs(fabs(uniform-a->ipd_center_mm/1000)-a->ipd_mm/2000)<1e-8);
}
''' + function(source, 'draw_calibration') + r'''
int main(void){
 struct app a={.calibration=true,.ipd_mm=45,.ipd_center_mm=.25,.ipd_baseline_mm=63.5};
 for(int eye=0;eye<2;eye++){
  uniform=aeye_offset_center(eye,a.ipd_mm,a.ipd_center_mm);draw_calibration(&a,eye);
  assert(fabs(uniform-aeye_offset_center(eye,a.ipd_mm,a.ipd_center_mm))<1e-8);
 }
 assert(a.eye_texture[0]&&a.eye_texture[1]&&a.eye_texture[0]!=a.eye_texture[1]);
 a.ipd_mm=80;
 for(int eye=0;eye<2;eye++){
  uniform=aeye_offset_center(eye,a.ipd_mm,a.ipd_center_mm);draw_calibration(&a,eye);
  assert(fabs(uniform-aeye_offset_center(eye,a.ipd_mm,a.ipd_center_mm))<1e-8);
 }
 assert(draws==24);
 a.calibration=false;draw_calibration(&a,0);assert(draws==24);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-ipd-guide-') as directory:
            path = Path(directory) / 'guide.c'
            path.write_text(program)
            binary = Path(directory) / 'guide'
            flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'cairo'], text=True).split()
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'native/common'), str(path), '-lm', *flags, '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_compositor_profile_refresh_does_not_undo_unsaved_keys(self):
        source=(ROOT/'native/labwc/air-spatial.c').read_text()
        program=r'''
#define _POSIX_C_SOURCE 200809L
#include "air-ipd.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
enum {DESKTOP,ARMING,SPATIAL};
static struct {uint64_t ipd_poll;struct air_ipd ipd_profile;
 double ipd_mm,ipd_center_mm,ipd_baseline_mm;int mode,count;} xr;
static uint64_t clock_ns=10000000000ULL;
static uint64_t now_ns(void){return clock_ns;}
static int controls,packets,scenes;
static void publish_controls(void){controls++;}
static void stereo_packet(void){packets++;}
static void scene_update(void){scenes++;}
#define WLR_INFO 0
#define wlr_log(...) ((void)0)
''' + function(source,'refresh_ipd') + r'''
static void write_profile(const char *text){
 char path[4096];snprintf(path,sizeof(path),"%s.ipd",getenv("AIR_TRACKER_READY_FILE"));
 FILE *f=fopen(path,"w");assert(f);
 fprintf(f,"%ld %llu %s\n",(long)getpid(),(unsigned long long)clock_ns,text);fclose(f);
 clock_ns+=300000000;
}
int main(int argc,char **argv){
 assert(argc==2);assert(!setenv("AIR_TRACKER_READY_FILE",argv[1],1));
 unsetenv("AIR_NATIVE_TEST_ID");
 xr.ipd_mm=64;xr.ipd_profile=(struct air_ipd){64,0,64};xr.mode=SPATIAL;
 write_profile("64 0 64 64");refresh_ipd();assert(!controls);
 xr.ipd_mm=64.5;clock_ns+=300000000;refresh_ipd();
 assert(xr.ipd_mm==64.5&&!controls); /* Old heartbeat cannot undo key. */
 xr.ipd_mm=65;
 write_profile("64.5 0 64 64.5");refresh_ipd();assert(xr.ipd_mm==65&&!controls);
 xr.ipd_mm=64.5;refresh_ipd(); /* Poll is rate-limited. */
 clock_ns+=300000000;refresh_ipd();assert(xr.ipd_mm==64.5);
 write_profile("63.5 .25 63.5 64.5");refresh_ipd();
 assert(xr.ipd_mm==63.5&&xr.ipd_center_mm==.25&&xr.ipd_baseline_mm==63.5);
 assert(controls==2&&packets==2&&scenes==2&&xr.count==-1);
 xr.ipd_mm=61;clock_ns+=300000000;refresh_ipd();assert(xr.ipd_mm==61);
 write_profile("62 nan 63.5 61");refresh_ipd();assert(xr.ipd_mm==61);
 write_profile("62 .25 63.5 61 extra");refresh_ipd();assert(xr.ipd_mm==61);
 write_profile("62 .25 63.5 61");clock_ns+=4000000000ULL;refresh_ipd();assert(xr.ipd_mm==61);
 struct air_ipd p;double control;
 assert(!aipd_read(getenv("AIR_TRACKER_READY_FILE"),getpid()+1,clock_ns,&p,&control));
 assert(!aipd_read(getenv("AIR_TRACKER_READY_FILE"),getpid(),1,&p,&control));
 assert(controls==2);
}
'''
        with tempfile.TemporaryDirectory(prefix='air-ipd-refresh-') as directory:
            path=Path(directory)/'refresh.c';path.write_text(program)
            binary=Path(directory)/'refresh'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',str(ROOT/'native/common'),str(path),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary),str(Path(directory)/'tracker.json')],check=True)
