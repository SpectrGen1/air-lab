/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIR_AUTO_H
#define AIR_AUTO_H
#include <stdbool.h>
#include <stdint.h>

/* Connected and unlocked is the desired state. Debounce appearance/unlock;
 * retry cancelled/failed attempts with backoff while the display remains. */
struct air_auto {
    bool present, pending;
    uint64_t since;
};
/* Follow a confirmed hardware selection. Never choose a stereo transport by
 * writing the glasses' mode: optical activation differs from DP readback. */
static inline int aselected_display_mode(bool stereo_enabled, int hardware,
                                        int width, int height, float scale, int transform) {
    if (height != 1080 || scale != 1 || transform != 0) return -1;
    if (hardware == 0 && width == 1920) return 0;
    if (stereo_enabled && hardware == 1 && width == 3840) return 1;
    return -1;
}
static inline bool aauto_update(struct air_auto *state, bool present, bool inhibited,
                                bool idle, uint64_t now) {
    if (!present) {
        *state = (struct air_auto){0};
        return false;
    }
    if (!state->present) {
        state->present = state->pending = true;
        state->since = now;
    }
    if (inhibited) {
        state->pending = true;
        state->since = now;
        return false;
    }
    uint64_t delay = state->pending ? 500000000 : 5000000000ULL;
    if (!idle || now - state->since < delay)
        return false;
    state->pending = false;
    state->since = now;
    return true;
}
#endif
