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
static inline struct air_vec aray(double theta, double h) {
    struct air_vec d = av(AIR_RADIUS * sin(theta), h, AIR_RADIUS * cos(theta));
    return ascale(d, 1 / sqrt(adot(d, d)));
}
static inline struct air_vec anormal(const struct air_panel *p) {
    return av(sin(p->azimuth), 0, cos(p->azimuth));
}
static inline struct air_vec aright(const struct air_panel *p) {
    return av(cos(p->azimuth), 0, -sin(p->azimuth));
}
static inline struct air_vec acentre(const struct air_panel *p) {
    return aadd(aadd(ascale(anormal(p), p->distance > 0 ? p->distance : AIR_RADIUS),
                     ascale(aright(p), p->tangent)),
                av(0, p->elevation, 0));
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
    *y = p->height * .5 - q.y / p->metres_per_pixel;
    *distance = t;
    return true;
}
static inline struct air_vec apoint(const struct air_panel *p, double x, double y) {
    return aadd(acentre(p), aadd(ascale(aright(p), (x - p->width * .5) * p->metres_per_pixel),
                                 av(0, (p->height * .5 - y) * p->metres_per_pixel, 0)));
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
        double yaw = hypot(out[6], out[8]) > 1e-6 ? atan2(out[6], out[8])
                                                : atan2(-out[2], out[0]);
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
