/* CPU upload conversion for the negotiated wl_shm layout (little-endian host).
 */
#ifndef AIR_PIXELS_H
#define AIR_PIXELS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wayland-client-protocol.h>
static inline unsigned air_pixel_bytes(uint32_t format) {
    switch (format) {
    case WL_SHM_FORMAT_RGB888:
    case WL_SHM_FORMAT_BGR888:
        return 3;
    case WL_SHM_FORMAT_ARGB8888:
    case WL_SHM_FORMAT_XRGB8888:
    case WL_SHM_FORMAT_ABGR8888:
    case WL_SHM_FORMAT_XBGR8888:
        return 4;
    default:
        return 0;
    }
}
static inline uint32_t air_pixel_stride(uint32_t format, uint32_t width) {
    unsigned bpp = air_pixel_bytes(format);
    /* Both four-byte row alignment and whole pixels, including odd RGB widths. */
    unsigned alignment = bpp == 3 ? 12 : 4;
    return bpp ? ((width * bpp + alignment - 1) / alignment) * alignment : 0;
}
static inline void air_pixels_rgba(unsigned char *dst, const unsigned char *src, uint32_t width,
                                   uint32_t height, uint32_t stride, uint32_t format) {
    unsigned bpp = air_pixel_bytes(format);
    bool blue_first = format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888 ||
                      format == WL_SHM_FORMAT_RGB888;
    bool opaque = bpp == 3 || format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_XBGR8888;
    for (uint32_t y = 0; y < height; y++) {
        const unsigned char *row = src + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            const unsigned char *p = row + x * bpp;
            *dst++ = p[blue_first ? 2 : 0];
            *dst++ = p[1];
            *dst++ = p[blue_first ? 0 : 2];
            *dst++ = opaque ? 255 : p[3];
        }
    }
}
#endif
