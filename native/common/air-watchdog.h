/* Distinguish a silent rendering loop from tracking loss with live scanout. */
#ifndef AIR_WATCHDOG_H
#define AIR_WATCHDOG_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
enum air_watchdog_result { AIR_WATCHDOG_OK, AIR_WATCHDOG_PAUSED,
    AIR_WATCHDOG_TRACKING_LOST, AIR_WATCHDOG_PRESENTATION_LOST };
struct air_watchdog { bool paused; };
static inline uint64_t awatchdog_age(uint64_t now,uint64_t stamp) {
    return stamp && stamp<=now ? now-stamp : UINT64_MAX;
}
/* Include IPC queue time: an old queued pose must not become fresh on receipt. */
static inline bool awatchdog_pose_time(uint64_t now,uint64_t sent,double age,uint64_t *sample) {
    if(!isfinite(age) || age<0 || age>=.25 || !sent || sent>now)return false;
    uint64_t ns=(uint64_t)(age*1e9);
    if(ns>sent)return false;
    uint64_t stamp=sent-ns;
    if(awatchdog_age(now,stamp)>=250000000ULL)return false;
    *sample=stamp;return true;
}
static inline enum air_watchdog_result awatchdog_update(struct air_watchdog *w,uint64_t now,
                                                         uint64_t pose,uint64_t frame) {
    uint64_t p=awatchdog_age(now,pose),f=awatchdog_age(now,frame);
    /* Fresh tracking AND actual scanout are required to resume. Retaining the
     * last scene must never manufacture input readiness or renew stale stamps. */
    if(w->paused && p<=250000000ULL && f<=250000000ULL){
        w->paused=false;return AIR_WATCHDOG_OK;
    }
    if(p>10000000000ULL || f>10000000000ULL)
        return p>700000000ULL ? AIR_WATCHDOG_TRACKING_LOST : AIR_WATCHDOG_PRESENTATION_LOST;
    if(w->paused)return AIR_WATCHDOG_PAUSED;
    /* If frames keep arriving while poses stop, preserve prompt tracking-loss
     * recovery. When both stop together, GPU/CPU loading may have stalled the
     * presenter or compositor event loop: allow a bounded retained-frame pause. */
    if(p>700000000ULL && f<500000000ULL)return AIR_WATCHDOG_TRACKING_LOST;
    if(p>700000000ULL || f>1000000000ULL){
        w->paused=true;return AIR_WATCHDOG_PAUSED;
    }
    return AIR_WATCHDOG_OK;
}
#endif
