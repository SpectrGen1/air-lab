#include "air-math.h"
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
 assert(!aframe_region(&p,100,100));assert(!aframe_region(&p,-100,-100));
 p.bx=-20;p.by=-60;p.bw=p.width+40;p.bh=p.height+100;
 assert(!aframe_region(&p,100,-20)); /* A popup stays inside its capture group. */
 assert(aframe_region(&p,100,-80)==AIR_TITLE);
 near(aresize_span(.5,.1,1),.7);near(aresize_span(.5,-.1,-1),.7);
 near(aresize_span(.5,-100,1),.12);near(aresize_span(.5,100,1),3.5);

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
 assert(sizeof(struct air_packet)<1024);
 puts("PASS: independent head/pointer geometry, full seam, rear/parallel rays, unbounded grabs, fractional popup coordinates and transient offsets");return 0;
}
