#pragma once
/* RGB565 resizing for the slideshow: fit a picture into the screen keeping
 * its aspect ratio. Box-filter averaging when shrinking (no aliasing on big
 * photos), nearest neighbour when enlarging (keeps pixel art crisp). */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest size with sw:sh's aspect that fits in max_w x max_h (at least 1x1). */
void img_fit(int sw, int sh, int max_w, int max_h, int *dw, int *dh);

/* src: sw x sh, row stride sstride pixels. dst: dw x dh, tightly packed.
 * Returns false only if it could not allocate its small lookup tables. */
bool img_scale(const uint16_t *src, int sw, int sh, int sstride, uint16_t *dst, int dw, int dh);

/* Same scaling, but dst has row stride dstride pixels and only the output
 * pixels in [rx0, rx1) x [ry0, ry1) are written (clamped to dw x dh) - for
 * redrawing just the part of an animation frame that changed. */
bool img_scale_ex(const uint16_t *src, int sw, int sh, int sstride,
                  uint16_t *dst, int dstride, int dw, int dh,
                  int rx0, int ry0, int rx1, int ry1);

#ifdef __cplusplus
}
#endif
