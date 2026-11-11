/* CPU upload conversion for the negotiated wl_shm layout (little-endian host).
 */
#ifndef AIR_PIXELS_H
#define AIR_PIXELS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wayland-client-protocol.h>
struct air_pixel_damage { uint32_t x, y, width, height; };
static inline void air_pixel_damage_add(struct air_pixel_damage *d, int32_t x, int32_t y,
                                        int32_t w, int32_t h, uint32_t width, uint32_t height) {
    if(w<=0 || h<=0)return;
    int64_t right=(int64_t)x+w,bottom=(int64_t)y+h;
    int64_t left=x<0?0:x,top=y<0?0:y;
    if(right>width)right=width;
    if(bottom>height)bottom=height;
    if(right<=left || bottom<=top)return;
    if(d->width && d->height){
        if(left>d->x)left=d->x;
        if(top>d->y)top=d->y;
        if(right<(int64_t)d->x+d->width)right=(int64_t)d->x+d->width;
        if(bottom<(int64_t)d->y+d->height)bottom=(int64_t)d->y+d->height;
    }
    *d=(struct air_pixel_damage){left,top,right-left,bottom-top};
}
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
    /* wl_shm permits tight rows. wlroots GLES readback uses one glReadPixels
     * only for its exact packed stride; RGB padding forces one read per row. */
    return bpp && width <= UINT32_MAX / bpp ? width * bpp : 0;
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
