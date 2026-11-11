#include "air-math.h"
#include "air-workspace.h"
#include "air-auto.h"
#include <assert.h>
#include <stdio.h>
static void near(double a,double b){assert(fabs(a-b)<1e-8);}
int main(void){
 struct air_panel p={.width=960,.height=540,.bx=-20,.by=-40,.bw=1010,.bh=650,.metres_per_pixel=.0009,.distance=AIR_RADIUS};
 double x,y,t;assert(aplane(&p,aray(0,0),&x,&y,&t));near(x,480);near(y,270);near(t,AIR_RADIUS);
 for(int i=-100;i<=100;i++){
  double px=i*12.5,py=i*3.125;struct air_vec q=apoint(&p,px,py);struct air_vec d=ascale(q,1/sqrt(adot(q,q)));
  assert(aplane(&p,d,&x,&y,&t));near(x,px);near(y,py);
 }
 assert(!aplane(&p,av(0,0,-1),&x,&y,&t));assert(!aplane(&p,av(1,0,0),&x,&y,&t));
 struct air_vec a=aray(AIR_PI-1e-7,.5),b=aray(-AIR_PI+1e-7,.5);assert(fabs(a.x-b.x)<1e-6);near(a.y,b.y);near(a.z,b.z);
 p.azimuth=AIR_PI;p.elevation=.2;p.tangent=.12;p.distance=1.3;
 struct air_vec q=apoint(&p,500,250),d=ascale(q,1/sqrt(adot(q,q)));assert(aplane(&p,d,&x,&y,&t));near(x,500);near(y,250);
 p.azimuth=0;p.elevation=p.tangent=0;p.distance=AIR_RADIUS;
 assert(aplane(&p,aray(.1,.03),&x,&y,&t));double old_x=x,old_y=y;
 for(int i=-100;i<100;i++){
  double basis[9];abasis(i*.02,i*.003,i*.001,basis);
  for(int j=0;j<3;j++)near(adot(abaxis(basis,j),abaxis(basis,j)),1);
  near(adot(abaxis(basis,0),abaxis(basis,1)),0);
  assert(aplane(&p,aray(.1,.03),&x,&y,&t));near(x,old_x);near(y,old_y);
 }
 double basis[9];abasis(0,0,0,basis);assert(avisible(av(0,0,1.4),basis,16./9));abasis(AIR_PI/2,0,0,basis);assert(!avisible(av(0,0,1.4),basis,16./9));

 /* Recenter from a tilted neutral pose; desktop leveling must remove roll
  * without changing forward direction, yaw/pitch signs or pointer geometry. */
 double zero[9], raw[9], relative[9], level[9];
 abasis(.4,-.2,.35,zero);arelative_basis(zero,zero,true,relative);
 for(int i=0;i<9;i++)near(relative[i],i%4==0?1:0);
 abasis(-.7,.25,.4,raw);arelative_basis(raw,zero,true,relative);
 arelative_basis(raw,zero,false,level);
 for(int i=6;i<9;i++)near(level[i],relative[i]);
 near(level[1],0);assert(level[4]>0);
 abasis(0,0,0,zero);
 for(int i=-10;i<=10;i++){
  double yaw=i*.1;abasis(yaw,.1,.3,raw);arelative_basis(raw,zero,false,level);
  near(atan2(level[6],level[8]),yaw);near(atan2(level[7],hypot(level[6],level[8])),.1);
  near(level[1],0);assert(level[4]>0);
 }

 /* Every elevated, offset and depth-shifted panel is perpendicular to the
  * viewer-origin ray; independent X/Y scale round-trips through the same plane. */
 for(int k=-8;k<=8;k++){
  p.azimuth=k*.7;p.elevation=k*.18;p.tangent=.13;p.distance=.6+fabs((double)k)*.2;
  p.metres_per_pixel=.0006;p.metres_per_pixel_y=.0011;
  struct air_vec c=acentre(&p),n=anormal(&p),r=aright(&p),u=aup(&p);
  near(adot(n,r),0);near(adot(n,u),0);near(adot(r,u),0);near(adot(u,u),1);
  near(adot(n,c),sqrt(adot(c,c)));
  for(int j=-3;j<=3;j++){
   double px=480+j*210.25,py=270-j*130.125;
   struct air_vec q=apoint(&p,px,py),d=ascale(q,1/sqrt(adot(q,q)));
   assert(aplane(&p,d,&x,&y,&t));near(x,px);near(y,py);
  }
 }
 p.bx=p.by=0;p.bw=p.width;p.bh=p.height;
 assert(aframe_region(&p,100,-20)==AIR_TITLE);
 assert(aframe_region(&p,-6,-46)==(AIR_EDGE_LEFT|AIR_EDGE_TOP));
 assert(aframe_region(&p,p.width+6,p.height+6)==(AIR_EDGE_RIGHT|AIR_EDGE_BOTTOM));
 assert(aframe_region(&p,-6,100)==AIR_EDGE_LEFT);
 assert(aframe_region(&p,100,p.height+6)==AIR_EDGE_BOTTOM);
 assert(aframe_region(&p,p.width+6,100)==AIR_EDGE_RIGHT);
 assert(aframe_region(&p,100,-46)==AIR_EDGE_TOP);
 assert(aframe_region(&p,-6,-20)==AIR_EDGE_LEFT);
 assert(!aframe_region(&p,100,100));assert(!aframe_region(&p,-100,-100));
 p.bx=-20;p.by=-60;p.bw=p.width+40;p.bh=p.height+100;
 assert(!aframe_region(&p,100,-20)); /* A popup stays inside its capture group. */
 assert(aframe_region(&p,100,-80)==AIR_TITLE);
 struct air_panel chrome = {.width=900, .height=600, .bw=900, .bh=600,
    .flags=AIR_PANEL_FOCUSED, .distance=AIR_RADIUS, .metres_per_pixel=.0006};
 assert(aframe_region(&chrome,810,-20)==AIR_CENTER);
 assert(aframe_region(&chrome,840,-20)==AIR_PIN);
 assert(aframe_region(&chrome,880,-20)==AIR_MINIMIZE);
 assert(aframe_region(&chrome,880,-46)==AIR_EDGE_TOP);
 chrome.flags=0;
 assert(aframe_region(&chrome,880,-20)==AIR_TITLE);
 double target;
 assert(!asnap_target(.15,.14,0,&target));
 assert(asnap_target(.15,.4,0,&target));near(target,.175);
 assert(asnap_target(-3.11,2.8,3.13,&target));near(target,3.13);
 assert(!asnap_target(.27,.55,0,&target));
 assert(aoverview_row(0,.16,7)==-1 && aoverview_row(.28,.066,7)==-1);
 assert(aoverview_row(0,.066,7)==0 && aoverview_row(0,-.112,7)==6);
 assert(aoverview_row(0,-.14,7)==-1 && aoverview_row(0,-.01925,1)==0);
 struct air_placement pair=alayout_slot(AIR_LAYOUT_PAIR,1,2,.2,.1);
 near(pair.angle,.375);near(pair.height,.1);assert(aplacement_valid(pair));
 struct air_placement code=alayout_slot(AIR_LAYOUT_CODE,1,2,0,0);
 assert(code.angle>0 && code.scale<1);
 double camera[9];abasis(0,0,0,camera);
 assert(apanel_maybe_visible(&chrome,camera,16./9,1));
 chrome.azimuth=AIR_PI;
 assert(!apanel_maybe_visible(&chrome,camera,16./9,1));
 struct air_timing timing={0};
 for(int i=0;i<95;i++)atiming_record(&timing,16666667);
 for(int i=0;i<5;i++)atiming_record(&timing,33100000);
 assert(atiming_percentile(&timing,95)==17 && atiming_percentile(&timing,99)==34);
 /* All corners, axis-only and off-diagonal motion use a single scale. */
 for(int ix=-1;ix<=1;ix+=2)for(int iy=-1;iy<=1;iy+=2){
  uint32_t region=(ix<0?AIR_EDGE_LEFT:AIR_EDGE_RIGHT)|
                  (iy<0?AIR_EDGE_TOP:AIR_EDGE_BOTTOM);
  assert(aframe_region(&p,ix<0?-26:p.width+26,iy<0?-106:p.height+46)==region);
  near(aresize_scale(.6,.4,0,0,region),1);
  near(aresize_scale(.6,.4,ix*.06,iy*.04,region),1.2);
  assert(aresize_scale(.6,.4,ix*.06,0,region)>1);
  assert(aresize_scale(.6,.4,0,iy*.04,region)>1);
  near(aresize_scale(.6,.4,ix*.04,-iy*.06,region),1);
  double small=aresize_scale(.6,.4,-ix*100,-iy*100,region);
  double large=aresize_scale(.6,.4,ix*100,iy*100,region);
  near(.4*small,.12);near(.6*large,3.5);
  near((.6*small)/(.4*small),1.5);near((.6*large)/(.4*large),1.5);
 }

 /* Off-center title grabs stay on the same cylinder ray through depth changes. */
 for(int k=-5;k<=5;k++){
  double angle=k*.35,height=k*.08;
  double ox=.12,oy=.24;
  double distance=.8+fabs((double)k)*.2;
  p=(struct air_panel){.width=900,.height=600,.distance=distance,.azimuth=angle,
                       .elevation=height,.metres_per_pixel=.0006};
  struct air_vec q=apoint(&p,450+ox/.0006,300-oy/.0006);
  double theta=atan2(q.x,q.z),ph=AIR_RADIUS*q.y/hypot(q.x,q.z);
  double a=angle,h=height;
  assert(agrab_position(ox,oy,distance*.9,theta,ph,&a,&h));
  p.distance=distance*.9;p.azimuth=a;p.elevation=h;
  q=apoint(&p,450+ox/.0006,300-oy/.0006);
  near(awrap(atan2(q.x,q.z)-theta),0);
  assert(fabs(AIR_RADIUS*q.y/hypot(q.x,q.z)-ph)<1e-6);
 }

 /* Pixel-calibrated, camera-independent pointer travel and packet invariance. */
 double gain=amouse_gain(1920),theta=0,height=0;
 amouse_move(&theta,&height,1,0,gain);
 assert(fabs(tan(theta)*1920/(2*tan(20*AIR_PI/180))-1)<1e-6);
 for(int k=-5;k<=5;k++){
  double start_height=k*.2,full_theta=.4,full_height=start_height;
  double split_theta=full_theta,split_height=full_height;
  amouse_move(&full_theta,&full_height,200,-450,gain);
  for(int j=0;j<100;j++)amouse_move(&split_theta,&split_height,2,-4.5,gain);
  near(full_theta,split_theta);near(full_height,split_height);
  struct air_vec empty=acylinder_point(full_theta,full_height);
  near(hypot(empty.x,empty.z),AIR_RADIUS);near(empty.y,full_height);
 }
 theta=0;height=0;amouse_move(&theta,&height,0,10,gain);
 near(atan2(height,AIR_RADIUS),-10*gain);
 for(int i=1;i<9;i++){
  double depth=i*.5,scale=acursor_pixel_scale(depth,1920,1);
  near(scale/depth*1920/(2*tan(20*AIR_PI/180)),1);
 }
 /* Presence/unlock debounces; failed/cancelled attempts retry with backoff. */
 struct air_auto automatic={0};
 assert(!aauto_update(&automatic,false,false,true,0));
 assert(!aauto_update(&automatic,true,false,true,100000000));
 assert(!aauto_update(&automatic,true,false,true,599999999));
 assert(aauto_update(&automatic,true,false,true,600000000));
 assert(!aauto_update(&automatic,true,false,true,1600000000));
 assert(!aauto_update(&automatic,false,false,true,1700000000));
 assert(!aauto_update(&automatic,true,false,false,1800000000));
 assert(!aauto_update(&automatic,true,false,false,2800000000));
 assert(aauto_update(&automatic,true,false,true,2900000000));
 assert(!aauto_update(&automatic,false,false,true,3000000000));
 assert(!aauto_update(&automatic,true,true,true,3100000000));
 assert(aauto_update(&automatic,true,false,true,4100000000)); /* Unlock re-enters. */
 assert(!aauto_update(&automatic,false,false,true,4200000000));
 assert(!aauto_update(&automatic,true,false,true,4300000000));
 automatic.pending=false; automatic.since=4300000000ULL; /* Exit/failure starts retry backoff. */
 assert(!aauto_update(&automatic,true,false,true,5300000000));
 assert(aauto_update(&automatic,true,false,true,9300000000ULL));
 assert(!aauto_update(&automatic,false,false,true,9400000000ULL));
 assert(!aauto_update(&automatic,true,false,true,9500000000ULL));
 assert(!aauto_update(&automatic,false,false,true,9600000000ULL)); /* Contact bounce. */
 assert(!aauto_update(&automatic,true,false,true,9700000000ULL));
 assert(!aauto_update(&automatic,true,false,true,10100000000ULL));
 assert(aauto_update(&automatic,true,false,true,10200000000ULL));

 /* Dock follows horizontal yaw at exactly two feet forward, one foot down.
  * Pitch/roll affect only viewing, and the same plane drives picking. */
 for(int k=-8;k<=8;k++){
  struct air_panel dock={.width=700,.height=48};
  awidget_pose(&dock,1,k*.4);
  struct air_vec center=acentre(&dock);
  near(hypot(center.x,center.z),.6096);near(center.y,-.3048);
  near(atan2(-center.y,hypot(center.x,center.z)),atan(.5));
  struct air_vec ray=ascale(center,1/sqrt(adot(center,center)));
  assert(aplane(&dock,ray,&x,&y,&t));near(x,350);near(y,24);
  near(adot(aright(&dock),av(0,1,0)),0);
 }
 /* Eye baseline rotates with head; geometry/picking remains at eye midpoint. */
 for(int k=-4;k<=4;k++) {
  double b[9];abasis(k*.25,.1,.15,b);
  struct air_vec l=aeye_origin(b,0,64),r=aeye_origin(b,1,64);
  near(sqrt(adot(aadd(r,ascale(l,-1)),aadd(r,ascale(l,-1)))),.064);
  near(adot(aadd(r,l),aadd(r,l)),0);
  struct air_vec forward=ascale(abaxis(b,2),1.4);
  assert(astereo_visible(forward,b,16./9,64));
  double near_disparity=.064/.6,far_disparity=.064/3;
  assert(near_disparity>far_disparity);
 }
 near(aeye_offset_center(0,63.5,.25),-.0315);
 near(aeye_offset_center(1,63.5,.25),.032);
 for(int k=-4;k<=4;k++) {
  double b[9];abasis(k*.25,.1,.15,b);
  struct air_vec l=aeye_origin_center(b,0,63.5,.25),r=aeye_origin_center(b,1,63.5,.25);
  near(sqrt(adot(l,l)),.0315);near(sqrt(adot(r,r)),.032);
  struct air_vec delta=aadd(r,ascale(l,-1));near(sqrt(adot(delta,delta)),.0635);
  assert(astereo_visible_center(ascale(abaxis(b,2),1.4),b,16./9,63.5,.25));
 }
 near(aeye_offset(0,64),-.032);near(aeye_offset(1,64),.032);
 near(aeye_offset(0,1),-.0225);near(aeye_offset(1,100),.04);
 {double b[9];abasis(0,0,0,b);struct air_vec edge=av(tan(20*AIR_PI/180)-.001,0,1);
  assert(avisible(edge,b,16./9));assert(!astereo_visible(edge,b,16./9,64));}
 assert(sizeof(struct air_packet)<1024);
 puts("PASS: independent head/pointer geometry, full seam, rear/parallel rays, unbounded grabs, fractional popup coordinates and transient offsets");return 0;
}
