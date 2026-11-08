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
 assert(sizeof(struct air_packet)<1024);
 puts("PASS: independent head/pointer geometry, full seam, rear/parallel rays, unbounded grabs, fractional popup coordinates and transient offsets");return 0;
}
