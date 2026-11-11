/* Quiet synthetic orientation checks. No sensor, display or input connection. */
#include "air-pose.h"
#include <assert.h>
#include <stdio.h>
static double yaw(const double b[9]) { return atan2(b[6],b[8]); }
static void rotation_valid(const double b[9]) {
    for (int i=0;i<3;i++) for (int j=0;j<3;j++)
        assert(fabs(adot(abaxis(b,i),abaxis(b,j))-(i==j))<1e-10);
    struct air_vec r=abaxis(b,0),u=abaxis(b,1),z=abaxis(b,2);
    assert(fabs((r.y*u.z-r.z*u.y)*z.x+(r.z*u.x-r.x*u.z)*z.y+
                (r.x*u.y-r.y*u.x)*z.z-1)<1e-10);
}
int main(void) {
    double raw[9],out[9];
    for(int i=0;i<500;i++) {
        abasis(i*.07, sin(i*.03)*1.57, i*.13, raw);
        apose_basis(apose_quat(raw),out);rotation_valid(out);
        for(int j=0;j<9;j++)assert(fabs(out[j]-raw[j])<1e-10);
    }
    for(int rate=60;rate<=1000;rate+=940) {
        struct air_pose_filter f={0};
        uint64_t step=1000000000ULL/rate, stamp=1000000000;
        double input=0,filtered=0;
        /* Tiny heartbeat-frequency movement, including yaw wraparound. */
        for(int i=0;i<10*rate;i++) {
            double t=(double)i/rate,angle=AIR_PI+.03*AIR_PI/180*sin(2*AIR_PI*1.2*t);
            abasis(angle,.2,0,raw);apose_update(&f,raw,stamp+i*step,false,out);
            rotation_valid(out);
            if(i>=2*rate) {
                double a=awrap(angle-AIR_PI),b=awrap(yaw(out)-AIR_PI);
                input+=a*a;filtered+=b*b;
            }
        }
        double ratio=sqrt(filtered/input);assert(ratio<.6);
        printf("%d Hz synthetic 1.2 Hz tremor RMS ratio %.3f\n",rate,ratio);
        f=(struct air_pose_filter){0};abasis(0,0,0,raw);apose_update(&f,raw,stamp,false,out);
        double max_lag=0;
        for(int i=1;i<=2*rate;i++) {
            double target=(double)i/rate*60*AIR_PI/180;
            abasis(target,0,0,raw);apose_update(&f,raw,stamp+i*step,false,out);
            double lag=awrap(target-yaw(out))*180/AIR_PI;
            assert(lag>=-1e-7);max_lag=fmax(max_lag,lag);
            if(i>rate/4)assert(lag<.7);
        }
        assert(max_lag<1.5);
        printf("%d Hz synthetic 60 deg/s turn maximum lag %.3f degrees\n",rate,max_lag);
        /* Pause/recenter starts at the actual new pose, without a catch-up sweep. */
        stamp+=3*1000000000ULL;abasis(-1.4,.4,.2,raw);
        apose_update(&f,raw,stamp,false,out);
        for(int j=0;j<9;j++)assert(fabs(out[j]-raw[j])<1e-10);
        abasis(.9,.1,-.7,raw);apose_update(&f,raw,stamp+step,true,out);
        for(int j=0;j<9;j++)assert(fabs(out[j]-raw[j])<1e-10);
        struct air_pose_filter saved=f;
        abasis(-.9,-.1,.7,raw);apose_update(&f,raw,stamp,false,out);
        assert(!memcmp(&f,&saved,sizeof(f)));
        apose_update(&f,raw,f.stamp,false,out);assert(!memcmp(&f,&saved,sizeof(f)));
        /* A deliberate tiny offset remains reachable: no fixed dead zone. */
        f=(struct air_pose_filter){0};abasis(0,0,0,raw);apose_update(&f,raw,stamp,false,out);
        abasis(.0001,0,0,raw);
        for(int i=1;i<=5*rate;i++)apose_update(&f,raw,stamp+i*step,false,out);
        assert(fabs(yaw(out)-.0001)<1e-9);
    }
}
