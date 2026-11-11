"""Quiet loading-pause policy and actual presenter pose-publication checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_cursor import function
ROOT=Path(__file__).resolve().parent

def run_c(program):
    with tempfile.TemporaryDirectory(prefix='air-watchdog-') as directory:
        path=Path(directory)/'check.c';path.write_text(program)
        binary=Path(directory)/'check'
        subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',str(ROOT/'native/common'),str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

class LoadingPauseTests(unittest.TestCase):
    def test_loading_pause_recovers_without_renewing_tracking_or_frame_timestamps(self):
        run_c(r'''
#include "air-watchdog.h"
#include <assert.h>
int main(void){
 const uint64_t second=1000000000ULL,now=30*second;
 struct air_watchdog w={0};
 assert(awatchdog_update(&w,now,now-10000000,now-16000000)==AIR_WATCHDOG_OK&&!w.paused);
 assert(awatchdog_update(&w,now,now-800000000,now-800000000)==AIR_WATCHDOG_PAUSED&&w.paused);
 /* A fresh frame may arrive before the fresh pose: don't exit during recovery. */
 assert(awatchdog_update(&w,now+second,now-800000000,now+second)==AIR_WATCHDOG_PAUSED);
 assert(awatchdog_update(&w,now+second,now+second,now+second)==AIR_WATCHDOG_OK&&!w.paused);
 /* Real pose loss with continued presentation keeps the original fast recovery. */
 assert(awatchdog_update(&w,now,now-800000000,now-10000000)==AIR_WATCHDOG_TRACKING_LOST&&!w.paused);
 w=(struct air_watchdog){0};
 assert(awatchdog_update(&w,now,now,now-1200000000)==AIR_WATCHDOG_PAUSED);
 for(unsigned i=1;i<=8;i++)
  assert(awatchdog_update(&w,now+i*second,now+i*second,now-1200000000)==AIR_WATCHDOG_PAUSED);
 assert(awatchdog_update(&w,now+9*second,now+9*second,now-1200000000)==AIR_WATCHDOG_PRESENTATION_LOST);
 w=(struct air_watchdog){0};
 assert(awatchdog_update(&w,now,now-800000000,now-800000000)==AIR_WATCHDOG_PAUSED);
 assert(awatchdog_update(&w,now+10*second,now-800000000,now-800000000)==AIR_WATCHDOG_TRACKING_LOST);
 w=(struct air_watchdog){0};
 assert(awatchdog_update(&w,now,now-700000000,now-1000000000)==AIR_WATCHDOG_OK);
 assert(awatchdog_update(&w,now,now+1,now)==AIR_WATCHDOG_TRACKING_LOST);
 assert(awatchdog_update(&w,now,0,now)==AIR_WATCHDOG_TRACKING_LOST);
 uint64_t sample=0;
 assert(awatchdog_pose_time(now,now-100000000,.05,&sample)&&sample==now-150000000);
 assert(!awatchdog_pose_time(now,now-800000000,0,&sample));
 assert(!awatchdog_pose_time(now,now+1,0,&sample));
 assert(!awatchdog_pose_time(now,now,NAN,&sample));
 assert(!awatchdog_pose_time(now,now,.25,&sample));
 assert(awatchdog_age(now,now-300000000)>250000000); /* Late displayed poses aren't complete. */
}
''')

    def test_pose_reports_are_fresh_and_independent_of_display_callbacks(self):
        source=(ROOT/'native/presenter/presenter.c').read_text()
        run_c(r'''
#include "air-protocol.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
struct app {bool pose_valid;uint64_t pose_stamp,last_pose_report;double basis[9];};
static uint64_t clock_ns=1000000000ULL;
static uint64_t now_ns(void){return clock_ns;}
static unsigned reports;
static struct air_packet last;
static void packet(struct app *a,struct air_packet *m){(void)a;reports++;last=*m;}
''' +function(source,'report_pose')+r'''
int main(void){
 struct app a={0};report_pose(&a);assert(!reports);
 a.pose_valid=true;a.pose_stamp=clock_ns-10000000;a.basis[0]=1;
 report_pose(&a);assert(reports==1&&last.type==AIR_POSE&&last.data.pose.basis[0]==1);
 assert(last.data.pose.age>.009&&last.data.pose.age<.011);
 clock_ns+=1000000;a.pose_stamp=clock_ns;report_pose(&a);assert(reports==1);
 clock_ns+=8000000;a.pose_stamp=clock_ns;report_pose(&a);assert(reports==2&&last.data.pose.age==0);
 /* A stalled GPU must not make an old sensor sample appear freshly received. */
 clock_ns+=2000000000ULL;report_pose(&a);assert(reports==2);
 a.pose_stamp=clock_ns;report_pose(&a);assert(reports==3&&last.data.pose.age==0);
}
''')

    def test_stalled_udp_backlog_is_not_promoted_to_fresh_tracking(self):
        source=(ROOT/'native/presenter/presenter.c').read_text()
        program=r'''
#include "air-watchdog.h"
#include "air-math.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
struct app {int udp;uint16_t source_port;bool have_pose_sequence,running,pose_valid;
 uint32_t pose_sequence;uint64_t last_pose_poll,pose_stamp;double raw[9];};
static uint64_t clock_ns=1000000000ULL;
static uint64_t now_ns(void){return clock_ns;}
static unsigned queued,sequence,updates;
static ssize_t mocked_recvfrom(int fd,void *out,size_t size,int flags,struct sockaddr *addr,socklen_t *len){
 (void)fd;(void)size;(void)flags;(void)len;
 if(!queued)return -1;queued--;
 struct sockaddr_in *peer=(struct sockaddr_in *)addr;
 *peer=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=htons(1001),.sin_addr={htonl(INADDR_LOOPBACK)}};
 double values[6]={0};memcpy(out,values,48);memcpy((char *)out+48,&sequence,4);sequence++;
 return 52;
}
#define recvfrom mocked_recvfrom
static void update_basis(struct app *a){(void)a;updates++;}
static void presenter_event(const char *event,const char *reason){(void)event;(void)reason;}
''' +function(source,'receive_pose')+r'''
int main(void){
 struct app a={.running=true,.source_port=1001};
 queued=1;sequence=50;receive_pose(&a);assert(updates==1&&a.pose_stamp==clock_ns);
 uint64_t old=a.pose_stamp;clock_ns+=2000000000ULL;
 queued=2;receive_pose(&a);assert(updates==1&&a.pose_stamp==old&&a.pose_sequence==52);
 clock_ns+=1000000;queued=1;receive_pose(&a);assert(updates==2&&a.pose_stamp==clock_ns);
 /* A normal burst smooths its newest sample once, not at packet-drain speed. */
 clock_ns+=10000000;queued=5;receive_pose(&a);assert(updates==3);
 receive_pose(&a);assert(updates==3); /* Idle polling cannot evolve the pose. */
 clock_ns+=1000000000ULL;queued=1;sequence=0;receive_pose(&a);
 assert(!a.running); /* Tracker reference resets still force normal recovery. */
}
'''
        # Geometry uses libm; this test compiles the actual UDP receive function.
        with tempfile.TemporaryDirectory(prefix='air-pose-backlog-') as directory:
            path=Path(directory)/'check.c';path.write_text(program)
            binary=Path(directory)/'check'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(ROOT/'native/common'),str(path),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_pose_backpressure_keeps_presenter_alive_but_real_ipc_failure_does_not(self):
        source=(ROOT/'native/presenter/presenter.c').read_text()
        run_c(r'''
#include "air-protocol.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
struct app {int ipc;bool running;uint64_t generation,epoch;};
static uint64_t now_ns(void){return 1000000000ULL;}
''' +function(source,'packet')+r'''
int main(void){
 int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair));
 int size=4096;assert(!setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size)));
 struct app a={.ipc=pair[0],.running=true,.generation=12,.epoch=34};
 struct air_packet m={.type=AIR_POSE};
 for(int i=0;i<1000;i++)packet(&a,&m);
 assert(a.running); /* No reader: a loading stall must not exit via EAGAIN. */
 m.type=AIR_STATUS;packet(&a,&m);assert(!a.running); /* Critical protocol remains reliable. */
 a.running=true;m.type=AIR_POSE;close(pair[1]);packet(&a,&m);assert(!a.running);
 close(pair[0]);
}
''')

    def test_paused_motion_and_new_presses_are_consumed_without_desktop_input(self):
        source=(ROOT/'native/labwc/air-spatial.c').read_text()
        program=r'''
#include "air-watchdog.h"
#include "air-math.h"
#include <assert.h>
struct cursor_context {void *surface;};
enum {WLR_POINTER_CONSTRAINT_V1_LOCKED = 1, WLR_POINTER_CONSTRAINT_V1_CONFINED};
struct wlr_pointer_constraint_v1 {int type; void *surface;};
static bool active=true;
static bool air_spatial_active(void){return active;}
static struct {struct {struct wlr_pointer_constraint_v1 *current_constraint;struct {bool active;} drag;} seat;} server;
static struct {struct air_watchdog watchdog;bool pointer_pending,ready,stereo,calibration;
 uint64_t ack,epoch,heartbeat,pose_stamp;double theta,height,basis[9],aspect,ipd_mm,ipd_center_mm,zoom;
 struct {int region;} manipulation;} xr;
static double pointer_gain=.001,pointer_speed=1;
static unsigned scenes,sends;
static bool scene_update(void){scenes++;return false;}
static void pointer_packet(void){sends++;}
static bool constraint_context(struct cursor_context *c, struct air_vec ray, struct air_vec *p){
 (void)c; (void)ray; *p=av(0,0,1); return true;
}
static void point_pointer_at(struct air_vec point){(void)point;}
static void manipulation_update(void){}
static bool hit_context(struct cursor_context *c,struct air_vec *p){(void)c;*p=av(0,0,1);return true;}
static uint64_t now_ns(void){return 1000000000ULL;}
''' +function(source,'air_spatial_motion')+function(source,'air_spatial_press_allowed')+r'''
int main(void){
 xr.zoom=1;xr.watchdog.paused=true;
 assert(air_spatial_motion(10,20)&&xr.theta==0&&xr.height==0&&!scenes&&!sends);
 assert(!air_spatial_press_allowed()&&!scenes);
 active=false;assert(!air_spatial_motion(10,20)&&air_spatial_press_allowed());
 active=true;xr.watchdog.paused=false;xr.ready=true;xr.ack=xr.epoch=1;
 xr.heartbeat=xr.pose_stamp=now_ns();abasis(0,0,0,xr.basis);xr.aspect=16./9;
 assert(air_spatial_motion(10,20)&&sends==1&&scenes==1);
 assert(air_spatial_press_allowed()&&scenes==2);
 xr.pose_stamp-=300000000;assert(!air_spatial_press_allowed());
 struct wlr_pointer_constraint_v1 lock={.type=WLR_POINTER_CONSTRAINT_V1_LOCKED};
 server.seat.current_constraint=&lock;
 assert(air_spatial_motion(10,20)&&sends==1); /* Locked motion is relative only. */
 lock.type=WLR_POINTER_CONSTRAINT_V1_CONFINED;
 assert(air_spatial_motion(10,20)&&sends==2); /* Confined motion stays in spatial. */
 server.seat.current_constraint=0;
 server.seat.drag.active=true;
 assert(air_spatial_motion(10,20)&&sends==3); /* Cross-window drag continues. */
}
'''
        with tempfile.TemporaryDirectory(prefix='air-pause-input-') as directory:
            path=Path(directory)/'check.c';path.write_text(program)
            binary=Path(directory)/'check'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',str(ROOT/'native/common'),str(path),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
