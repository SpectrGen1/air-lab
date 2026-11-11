#ifndef AIR_WORKSPACE_H
#define AIR_WORKSPACE_H

#include "air-math.h"
#include <stdbool.h>
#include <string.h>

enum air_layout { AIR_LAYOUT_FOCUS = 1, AIR_LAYOUT_PAIR, AIR_LAYOUT_CODE };
#define AIR_OVERVIEW_ROWS 7
#define AIR_OVERVIEW_WIDTH .57
#define AIR_OVERVIEW_TEXTURE_W 1140
#define AIR_OVERVIEW_ROW_TOP 159
#define AIR_OVERVIEW_ROW_HEIGHT 57
#define AIR_LAYOUT_WINDOWS 128
#define AIR_LAYOUT_MAGIC 0x414c5901u

struct air_placement {
    double angle, height, distance, scale;
    uint32_t pinned;
    int32_t client_width, client_height;
};

struct air_saved_window {
    char workspace[64], app[128], title[128];
    struct air_placement placement;
};

struct air_workspace_state {
    uint32_t magic, count, reading;
    struct air_saved_window windows[AIR_LAYOUT_WINDOWS];
};

static inline int aoverview_texture_height(unsigned rows) {
    return 240 + AIR_OVERVIEW_ROW_HEIGHT * (rows ? rows : 1);
}

static inline int aoverview_row(double x, double y, unsigned rows) {
    int height = aoverview_texture_height(rows);
    if (fabs(x) >= AIR_OVERVIEW_WIDTH * (.5 - 36. / AIR_OVERVIEW_TEXTURE_W) ||
        fabs(y) >= height / 4000.)
        return -1;
    double pixel = height * .5 - y * 2000;
    if (pixel < AIR_OVERVIEW_ROW_TOP)
        return -1;
    unsigned row = (pixel - AIR_OVERVIEW_ROW_TOP) / AIR_OVERVIEW_ROW_HEIGHT;
    return row < rows ? (int)row : -1;
}

static inline bool aplacement_valid(struct air_placement p) {
    return isfinite(p.angle) && fabs(p.angle) <= AIR_PI &&
           isfinite(p.height) && fabs(p.height) <= 1.2 &&
           isfinite(p.distance) && p.distance >= .45 && p.distance <= 4 &&
           isfinite(p.scale) && p.scale >= .35 && p.scale <= 2.5 && p.pinned <= 1 &&
           ((p.client_width == 0 && p.client_height == 0) ||
            (p.client_width > 0 && p.client_height > 0 &&
             p.client_width <= AIR_MAX_CAPTURE_DIMENSION && p.client_height <= AIR_MAX_CAPTURE_DIMENSION));
}

/* Rank zero is the focused window. Sizes are presentation scales. */
static inline struct air_placement alayout_slot(enum air_layout layout, int rank, int count,
                                                 double yaw, double height) {
    struct air_placement p = {.angle = yaw, .height = height, .distance = AIR_RADIUS, .scale = 1};
    if (layout == AIR_LAYOUT_PAIR && count > 1 && rank < 2) {
        p.angle += rank == 0 ? -.175 : .175;
        p.scale = .82;
    } else if (layout == AIR_LAYOUT_CODE && count > 1 && rank < 2) {
        p.angle += rank == 0 ? -.09 : .205;
        p.scale = rank == 0 ? 1 : .74;
    } else if (rank > 0) {
        int side = rank % 2 ? 1 : -1;
        p.angle += side * (.53 + ((rank - 1) / 2 % 4) * .41);
        p.height += .31 * ((rank - 1) / 8 % 3 - (rank > 24 ? 1 : 0));
        p.scale = .8;
    }
    p.angle = awrap(p.angle);
    p.height = aclamp(p.height, -1.2, 1.2);
    return p;
}

static inline bool asnap_target(double angle, double initial, double heading, double *target) {
    if (fabs(awrap(angle - initial)) < .035)
        return false;
    double offset = awrap(angle - heading);
    const double slots[] = {-.175, 0, .175};
    for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        if (fabs(offset - slots[i]) < .062) {
            *target = awrap(heading + slots[i]);
            return true;
        }
    }
    return false;
}

#define AIR_TIMING_BUCKETS 256
struct air_timing {
    unsigned buckets[AIR_TIMING_BUCKETS];
    unsigned count;
};

static inline void atiming_record(struct air_timing *timing, uint64_t ns) {
    uint64_t bucket = (ns + 999999) / 1000000;
    if (bucket >= AIR_TIMING_BUCKETS)
        bucket = AIR_TIMING_BUCKETS - 1;
    timing->buckets[bucket]++;
    timing->count++;
}

static inline unsigned atiming_percentile(const struct air_timing *timing, unsigned percentile) {
    unsigned remaining = (timing->count * percentile + 99) / 100;
    for (unsigned i = 0; i < AIR_TIMING_BUCKETS; i++) {
        if (remaining <= timing->buckets[i])
            return i;
        remaining -= timing->buckets[i];
    }
    return AIR_TIMING_BUCKETS - 1;
}

static inline bool apanel_intersects_view(const struct air_panel *p, const double basis[9],
                                          double aspect, double zoom, double guard) {
    double left, top, right, bottom;
    aframe_bounds(p, &left, &top, &right, &bottom);
    struct air_vec corners[] = {
        apoint(p, left - AIR_FRAME_BORDER, top - AIR_FRAME_TITLE - AIR_FRAME_BORDER),
        apoint(p, right + AIR_FRAME_BORDER, top - AIR_FRAME_TITLE - AIR_FRAME_BORDER),
        apoint(p, left - AIR_FRAME_BORDER, bottom + AIR_FRAME_BORDER),
        apoint(p, right + AIR_FRAME_BORDER, bottom + AIR_FRAME_BORDER),
    };
    /* Keep a guard band so a just-revealed window already has a recent frame. */
    double horizontal = tan(40 * AIR_PI / 360) / zoom + guard;
    double vertical = horizontal / fmax(aspect, .1);
    unsigned outside = 31;
    for (int i = 0; i < 4; i++) {
        double z = adot(corners[i], abaxis(basis, 2));
        double x = adot(corners[i], abaxis(basis, 0));
        double y = adot(corners[i], abaxis(basis, 1));
        outside &= (z < 0 ? 1 : 0) |
                   (x < -horizontal * z - guard * .32 ? 2 : 0) | (x > horizontal * z + guard * .32 ? 4 : 0) |
                   (y < -vertical * z ? 8 : 0) | (y > vertical * z ? 16 : 0);
    }
    return outside == 0;
}

static inline bool apanel_maybe_visible(const struct air_panel *p, const double basis[9],
                                        double aspect, double zoom) {
    return apanel_intersects_view(p, basis, aspect, zoom, .16);
}

#endif
