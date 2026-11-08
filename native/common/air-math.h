#ifndef AIR_MATH_H
#define AIR_MATH_H
#include "air-protocol.h"
#include <math.h>
#include <stdbool.h>
#define AIR_PI 3.14159265358979323846
struct air_vec {
    double x, y, z;
};
static inline struct air_vec av(double x, double y, double z) { return (struct air_vec){x, y, z}; }
static inline struct air_vec aadd(struct air_vec a, struct air_vec b) {
    return av(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline struct air_vec ascale(struct air_vec a, double s) {
    return av(a.x * s, a.y * s, a.z * s);
}
static inline double adot(struct air_vec a, struct air_vec b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline double awrap(double x) { return atan2(sin(x), cos(x)); }
static inline double aclamp(double x, double a, double b) { return fmin(b, fmax(a, x)); }
static inline struct air_vec acylinder_point(double theta, double h) {
    return av(AIR_RADIUS * sin(theta), h, AIR_RADIUS * cos(theta));
}
static inline struct air_vec aray(double theta, double h) {
    struct air_vec d = acylinder_point(theta, h);
    return ascale(d, 1 / sqrt(adot(d, d)));
}
static inline double amouse_gain(double width) {
    return 2 * tan(40 * AIR_PI / 360) / fmax(1, width);
}
/* Constant angular travel on a world-fixed tangent basis. Integrate sec(elevation)
 * along diagonal motion so splitting a packet does not change the path. */
static inline void amouse_move(double *theta, double *height, double dx, double dy, double gain) {
    double start = atan2(*height, AIR_RADIUS), delta = -dy * gain;
    double limit = atan2(1.2, AIR_RADIUS), end = aclamp(start + delta, -limit, limit);
    double factor;
    if (fabs(end - start) > 1e-10) {
        double fraction = (end - start) / delta;
        factor = fraction * (atanh(sin(end)) - atanh(sin(start))) / (end - start) +
                 (1 - fraction) / cos(end);
    } else {
        factor = 1 / cos(start);
    }
    *theta = awrap(*theta + dx * gain * factor);
    *height = AIR_RADIUS * tan(end);
}
static inline double acursor_pixel_scale(double depth, double width, double surface_scale) {
    return fmax(.01, depth) * amouse_gain(width) / fmax(1, surface_scale);
}
static inline struct air_vec acentre(const struct air_panel *p) {
    double s = sin(p->azimuth), c = cos(p->azimuth);
    double distance = p->distance > 0 ? p->distance : AIR_RADIUS;
    return av(distance * s + p->tangent * c, p->elevation, distance * c - p->tangent * s);
}
static inline struct air_vec anormal(const struct air_panel *p) {
    struct air_vec c = acentre(p);
    return ascale(c, 1 / sqrt(adot(c, c)));
}
static inline struct air_vec aright(const struct air_panel *p) {
    struct air_vec n = anormal(p);
    double h = hypot(n.x, n.z);
    return av(n.z / h, 0, -n.x / h);
}
static inline struct air_vec aup(const struct air_panel *p) {
    struct air_vec n = anormal(p), r = aright(p);
    return av(n.y * r.z, n.z * r.x - n.x * r.z, -n.y * r.x);
}
static inline double apixel_y(const struct air_panel *p) {
    return p->metres_per_pixel_y > 0 ? p->metres_per_pixel_y : p->metres_per_pixel;
}
static inline bool aplane(const struct air_panel *p, struct air_vec d, double *x, double *y,
                          double *distance) {
    double denom = adot(d, anormal(p));
    if (denom < 1e-5)
        return false;
    double t = adot(acentre(p), anormal(p)) / denom;
    if (t <= 0)
        return false;
    struct air_vec q = aadd(ascale(d, t), ascale(acentre(p), -1));
    *x = p->width * .5 + adot(q, aright(p)) / p->metres_per_pixel;
    *y = p->height * .5 - adot(q, aup(p)) / apixel_y(p);
    *distance = t;
    return true;
}
static inline struct air_vec apoint(const struct air_panel *p, double x, double y) {
    return aadd(acentre(p), aadd(ascale(aright(p), (x - p->width * .5) * p->metres_per_pixel),
                                 ascale(aup(p), (p->height * .5 - y) * apixel_y(p))));
}
/* Keep spatial chrome outside the complete capture group, including CSD
 * shadows and popup/subsurface extensions, so controls never steal their input. */
static inline void aframe_bounds(const struct air_panel *p, double *left, double *top,
                                 double *right, double *bottom) {
    *left = fmin(0, p->bx);
    *top = fmin(0, p->by);
    *right = fmax(p->width, p->bx + p->bw);
    *bottom = fmax(p->height, p->by + p->bh);
}
static inline uint32_t aframe_region(const struct air_panel *p, double x, double y) {
    double left, top, right, bottom;
    aframe_bounds(p, &left, &top, &right, &bottom);
    x -= left;
    y -= top;
    double width = right - left, height = bottom - top;
    double b = AIR_FRAME_BORDER, title = AIR_FRAME_TITLE;
    if (x < -b || x > width + b || y < -title - b || y > height + b ||
        (x >= 0 && x < width && y >= 0 && y < height))
        return 0;
    uint32_t edges = (x < 0        ? AIR_EDGE_LEFT
                      : x >= width ? AIR_EDGE_RIGHT
                                   : 0) |
                     (y < -title    ? AIR_EDGE_TOP
                      : y >= height ? AIR_EDGE_BOTTOM
                                    : 0);
    if (y < -title || y >= height) {
        if (x < b)
            edges |= AIR_EDGE_LEFT;
        if (x > width - b)
            edges |= AIR_EDGE_RIGHT;
    }
    return edges ? edges : AIR_TITLE;
}
static inline double aresize_span(double span, double delta, int direction) {
    return aclamp(span + 2 * direction * delta, .12, 3.5);
}
/* Keep the held point on the mouse ray while a radial panel moves or changes
 * depth. Solve its vertical slope first, then its azimuth; no head pose enters. */
static inline bool agrab_position(double x, double y, double radius, double theta,
                                  double pointer_height, double *angle, double *height) {
    double h = *height, slope = pointer_height / AIR_RADIUS;
    for (int i = 0; i < 20; i++) {
        double len = hypot(radius, h), radial = radius - y * h / len;
        double f = h + y * radius / len - slope * hypot(radial, x);
        if (fabs(f) < 1e-7) {
            *height = h;
            *angle = awrap(theta - atan2(x, radial));
            return true;
        }
        double h2 = h + .0001, len2 = hypot(radius, h2);
        double f2 = h2 + y * radius / len2 - slope * hypot(radius - y * h2 / len2, x);
        double derivative = (f2 - f) / .0001;
        if (fabs(derivative) < 1e-5)
            break;
        double next = aclamp(h - aclamp(f / derivative, -.4, .4), -3, 3);
        if (fabs(next - h) < 1e-9)
            break;
        h = next;
    }
    return false; /* Retain the last stable panel at unreachable limits. */
}
static inline void abasis(double yaw, double pitch, double roll, double out[9]) {
    struct air_vec f = av(sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch));
    struct air_vec x = av(cos(yaw), 0, -sin(yaw)),
                   u = av(-sin(yaw) * sin(pitch), cos(pitch), -cos(yaw) * sin(pitch));
    struct air_vec b[3] = {aadd(ascale(x, cos(roll)), ascale(u, -sin(roll))),
                           aadd(ascale(x, sin(roll)), ascale(u, cos(roll))), f};
    for (int i = 0; i < 3; i++) {
        out[i * 3] = b[i].x;
        out[i * 3 + 1] = b[i].y;
        out[i * 3 + 2] = b[i].z;
    }
}
static inline struct air_vec abaxis(const double b[9], int i) {
    return av(b[3 * i], b[3 * i + 1], b[3 * i + 2]);
}
/* Both rendering and compositor picking consume this same camera basis. A
 * desktop-level horizon is deliberate; full physical roll remains opt-in. */
static inline void arelative_basis(const double raw[9], const double zero[9], bool head_roll,
                                   double out[9]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i * 3 + j] = adot(abaxis(raw, i), abaxis(zero, j));
    if (!head_roll) {
        double yaw = hypot(out[6], out[8]) > 1e-6 ? atan2(out[6], out[8]) : atan2(-out[2], out[0]);
        double pitch = atan2(out[7], hypot(out[6], out[8]));
        abasis(yaw, pitch, 0, out);
    }
}
static inline bool avisible(struct air_vec q, const double basis[9], double aspect) {
    double z = adot(q, abaxis(basis, 2));
    double t = tan(40 * AIR_PI / 360);
    return z > 0 && fabs(adot(q, abaxis(basis, 0))) < z * t &&
           fabs(adot(q, abaxis(basis, 1))) < z * t / aspect;
}
#endif
