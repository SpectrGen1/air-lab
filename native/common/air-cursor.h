/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIR_CURSOR_H
#define AIR_CURSOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A spatial interaction pointer must not disappear with a video's auto-hide.
 * Alpha below 3/255 is discarded by the presenter; padding isn't image data. */
static inline bool acursor_visible(const void *pixels, int width, int height, size_t stride,
                                   bool opaque) {
    if (!pixels || width <= 0 || height <= 0 || width > 256 || height > 256 ||
        stride < (size_t)width * 4)
        return false;
    if (opaque)
        return true;
    for (int y = 0; y < height; y++) {
        const uint8_t *row = (const uint8_t *)pixels + y * stride;
        for (int x = 0; x < width; x++)
            if (row[x * 4 + 3] >= 3)
                return true;
    }
    return false;
}
#endif
