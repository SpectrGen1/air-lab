#include "air-pixels.h"
#include <assert.h>
int main(void) {
    /* NVIDIA GLES capture offers BGR888: RGB byte order, padded three-byte rows.
     */
    const unsigned char rgb[] = {255, 0, 0,   9, 9, 9, 9, 9, 9, 9, 9, 9,
                                 0,   0, 255, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    unsigned char out[8];
    assert(air_pixel_stride(WL_SHM_FORMAT_BGR888, 1) == 12);
    assert(air_pixel_stride(WL_SHM_FORMAT_BGR888, 1715) == 5148);
    air_pixels_rgba(out, rgb, 1, 2, 12, WL_SHM_FORMAT_BGR888);
    const unsigned char expected[] = {255, 0, 0, 255, 0, 0, 255, 255};
    assert(memcmp(out, expected, sizeof(out)) == 0);
    air_pixels_rgba(out, rgb, 1, 2, 12, WL_SHM_FORMAT_RGB888);
    assert(out[2] == 255 && out[4] == 255 && out[3] == 255);
    const unsigned char bgra[] = {1, 2, 3, 4};
    air_pixels_rgba(out, bgra, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    assert(out[0] == 3 && out[1] == 2 && out[2] == 1 && out[3] == 4);
    air_pixels_rgba(out, bgra, 1, 1, 4, WL_SHM_FORMAT_XBGR8888);
    assert(out[0] == 1 && out[2] == 3 && out[3] == 255);
    assert(air_pixel_stride(0xffffffff, 100) == 0);
}
