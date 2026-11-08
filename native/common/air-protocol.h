/* Private, same-host socketpair protocol. No public input injection interface. */
#ifndef AIR_PROTOCOL_H
#define AIR_PROTOCOL_H
#include <stdint.h>
#define AIR_MAGIC 0x41495231u
#define AIR_VERSION 2u
#define AIR_MAX_PANELS 64
#define AIR_RADIUS 1.4
#define AIR_NAMESPACE "air-spatial-presenter-v1"
#define AIR_FRAME_BORDER 12
#define AIR_FRAME_TITLE 40
enum air_region {
    AIR_EDGE_TOP = 1,
    AIR_EDGE_BOTTOM = 2,
    AIR_EDGE_LEFT = 4,
    AIR_EDGE_RIGHT = 8,
    AIR_TITLE = 16
};
enum air_type {
    AIR_HELLO = 1,
    AIR_SCENE_BEGIN,
    AIR_PANEL,
    AIR_SCENE_COMMIT,
    AIR_POINTER,
    AIR_PRESENTED,
    AIR_POSE,
    AIR_READY,
    AIR_RECENTER,
    AIR_SHUTDOWN,
    AIR_STATUS,
    AIR_CURSOR,
    AIR_FRAME_PRESENTED
};
struct air_panel {
    uint64_t id;
    char identifier[128];
    char title[128];
    /* Geometry in the captured scene's logical coordinate system. */
    int32_t width, height, bx, by, bw, bh;
    double azimuth, elevation, metres_per_pixel, tangent, distance, metres_per_pixel_y;
};
struct air_packet {
    uint32_t magic, version, type, size;
    uint64_t generation, epoch, stamp_ns;
    union {
        struct air_panel panel;
        struct {
            double xyz[3];
            uint32_t hit, active;
            uint64_t view_id;
            uint32_t region;
        } pointer;
        struct {
            double basis[9];
            double age;
        } pose;
        struct {
            uint32_t width, height;
        } ready;
        struct {
            uint32_t complete;
        } frame;
        struct {
            uint32_t width, height, hot_x, hot_y, format;
            float scale;
        } cursor;
        char text[192];
    } data;
};
#endif
