/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIR_POSE_H
#define AIR_POSE_H
#include "air-math.h"
#include <string.h>
/* Speed-adaptive first-order orientation filtering, following the 1 Euro
 * principle (Casiez, Roussel & Vogel, CHI 2012), using shortest-arc quaternions
 * instead of independently filtering Euler angles or matrix elements.
 * https://gery.casiez.net/1euro/
 * Constants use Hz and degrees/second. They are initial tuning, not physical
 * heartbeat rejection or motion-to-photon acceptance measurements. */
struct air_quat { double w, x, y, z; };
struct air_pose_filter {
    struct air_quat previous, filtered;
    uint64_t stamp;
    double speed;
    bool valid;
};
static inline struct air_quat apose_normalize(struct air_quat q) {
    double n = sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    return (struct air_quat){q.w/n, q.x/n, q.y/n, q.z/n};
}
static inline struct air_quat apose_quat(const double b[9]) {
    struct air_quat q;
    double trace = b[0]+b[4]+b[8], s;
    if (trace > 0) {
        s = 2*sqrt(trace+1);
        q = (struct air_quat){s/4,(b[7]-b[5])/s,(b[2]-b[6])/s,(b[3]-b[1])/s};
    } else if (b[0] > b[4] && b[0] > b[8]) {
        s = 2*sqrt(1+b[0]-b[4]-b[8]);
        q = (struct air_quat){(b[7]-b[5])/s,s/4,(b[1]+b[3])/s,(b[2]+b[6])/s};
    } else if (b[4] > b[8]) {
        s = 2*sqrt(1+b[4]-b[0]-b[8]);
        q = (struct air_quat){(b[2]-b[6])/s,(b[1]+b[3])/s,s/4,(b[5]+b[7])/s};
    } else {
        s = 2*sqrt(1+b[8]-b[0]-b[4]);
        q = (struct air_quat){(b[3]-b[1])/s,(b[2]+b[6])/s,(b[5]+b[7])/s,s/4};
    }
    return apose_normalize(q);
}
static inline double apose_dot(struct air_quat a, struct air_quat b) {
    return a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
}
static inline struct air_quat apose_mix(struct air_quat a, struct air_quat b, double t) {
    double dot = apose_dot(a,b);
    if (dot < 0) {
        b = (struct air_quat){-b.w,-b.x,-b.y,-b.z};
        dot = -dot;
    }
    double u=1-t, v=t;
    if (dot < .9995) {
        double angle=acos(aclamp(dot,0,1)), divisor=sin(angle);
        u=sin((1-t)*angle)/divisor; v=sin(t*angle)/divisor;
    }
    return apose_normalize((struct air_quat){u*a.w+v*b.w,u*a.x+v*b.x,u*a.y+v*b.y,u*a.z+v*b.z});
}
static inline void apose_basis(struct air_quat q, double b[9]) {
    double w=q.w,x=q.x,y=q.y,z=q.z;
    double matrix[9]={1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),
        2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),
        2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)};
    memcpy(b,matrix,sizeof(matrix));
}
static inline double apose_alpha(double cutoff, double dt) {
    return 1 / (1 + 1 / (2*AIR_PI*cutoff*dt));
}
static inline void apose_update(struct air_pose_filter *f, const double raw[9],
                                uint64_t stamp, bool reset, double out[9]) {
    struct air_quat q=apose_quat(raw);
    if (reset || !f->valid || (stamp > f->stamp && stamp-f->stamp >= 250000000ULL)) {
        *f=(struct air_pose_filter){.previous=q,.filtered=q,.stamp=stamp,.valid=true};
    } else if (stamp > f->stamp) {
        double dt=(double)(stamp-f->stamp)/1e9;
        double speed=2*acos(aclamp(fabs(apose_dot(q,f->previous)),0,1))*180/AIR_PI/dt;
        f->speed += apose_alpha(2,dt)*(speed-f->speed);
        double cutoff=.6 + .35*f->speed;
        f->filtered=apose_mix(f->filtered,q,apose_alpha(cutoff,dt));
        f->previous=q;f->stamp=stamp;
    }
    /* Repeated/out-of-order timestamps never evolve the filter or its clock.
     * This clock is private filter state; it never renews tracking freshness. */
    apose_basis(f->filtered,out);
}
#endif
