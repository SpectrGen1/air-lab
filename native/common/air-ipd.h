/* Shared software eye geometry. These values do not calibrate physical optics. */
#ifndef AIR_IPD_H
#define AIR_IPD_H
#include "air-math.h"
#include <stdio.h>
struct air_ipd { double total, center, baseline; };
static inline bool aipd_valid(struct air_ipd p) {
    return isfinite(p.total) && p.total >= 45 && p.total <= 80 &&
           isfinite(p.baseline) && p.baseline >= 45 && p.baseline <= 80 &&
           isfinite(p.center) && fabs(p.center) <= 5;
}
static inline bool aipd_equal(struct air_ipd a, struct air_ipd b) {
    return a.total == b.total && a.center == b.center && a.baseline == b.baseline;
}
/* A session-owned settings publication, not a hardware or tracking signal. */
static inline bool aipd_read(const char *ready, long expected, uint64_t now, struct air_ipd *out, double *control) {
    if (!ready || ready[0] != '/') return false;
    char path[4096], extra;
    int n=snprintf(path,sizeof(path),"%s.ipd",ready);
    if(n<0 || (size_t)n>=sizeof(path))return false;
    FILE *f=fopen(path,"r");if(!f)return false;
    long owner;unsigned long long stamp;struct air_ipd p;double ack;
    int fields=fscanf(f,"%ld %llu %lf %lf %lf %lf %c",&owner,&stamp,&p.total,&p.center,&p.baseline,&ack,&extra);
    fclose(f);
    if(fields!=6 || expected<=0 || owner!=expected || stamp>now ||
       now-stamp>3000000000ULL || !aipd_valid(p) || !isfinite(ack) || ack<45 || ack>80)return false;
    *out=p;*control=ack;return true;
}
#endif
