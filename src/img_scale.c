#include "img_scale.h"
#include <stdlib.h>

void img_fit(int sw, int sh, int max_w, int max_h, int *dw, int *dh)
{
    /* compare sw/sh with max_w/max_h without floats */
    if ((int64_t)sw * max_h >= (int64_t)sh * max_w) {      /* wider: width limits */
        *dw = max_w;
        *dh = (int)(((int64_t)sh * max_w + sw / 2) / sw);
    } else {
        *dh = max_h;
        *dw = (int)(((int64_t)sw * max_h + sh / 2) / sh);
    }
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
}

/* Source span [lo[i], lo[i+1]) for each of n outputs over a length of len. */
static int *spans(int len, int n)
{
    int *lo = malloc((n + 1) * sizeof(int));
    if (!lo) {
        return NULL;
    }
    for (int i = 0; i <= n; i++) {
        lo[i] = (int)((int64_t)i * len / n);
    }
    for (int i = 0; i < n; i++) {                           /* enlarging: >= 1 pixel each */
        if (lo[i + 1] <= lo[i]) {
            lo[i + 1] = lo[i] + 1 <= len ? lo[i] + 1 : len;
        }
    }
    return lo;
}

bool img_scale_ex(const uint16_t *src, int sw, int sh, int sstride,
                  uint16_t *dst, int dstride, int dw, int dh,
                  int rx0, int ry0, int rx1, int ry1)
{
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > dw) rx1 = dw;
    if (ry1 > dh) ry1 = dh;
    if (rx0 >= rx1 || ry0 >= ry1) {
        return true;
    }
    int *xs = spans(sw, dw), *ys = spans(sh, dh);
    if (!xs || !ys) {
        free(xs);
        free(ys);
        return false;
    }
    if (dw > sw || dh > sh) {
        /* nearest neighbour (centre of each output pixel). The column map is
         * worked out once per call: a 64-bit divide per pixel is a slow
         * library call on this RISC-V core. xs (dw + 1 ints) is reused for it. */
        for (int x = rx0; x < rx1; x++) {
            xs[x] = (int)(((int64_t)x * 2 + 1) * sw / (2 * dw));
        }
        for (int y = ry0; y < ry1; y++) {
            int sy = (int)(((int64_t)y * 2 + 1) * sh / (2 * dh));
            const uint16_t *row = src + (size_t)sy * sstride;
            uint16_t *out = dst + (size_t)y * dstride;
            for (int x = rx0; x < rx1; x++) {
                out[x] = row[xs[x]];
            }
        }
    } else {
        /* box filter: average every source pixel that falls in the output pixel */
        for (int y = ry0; y < ry1; y++) {
            uint16_t *out = dst + (size_t)y * dstride;
            int y0 = ys[y], y1 = ys[y + 1];
            for (int x = rx0; x < rx1; x++) {
                int x0 = xs[x], x1 = xs[x + 1];
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int sy = y0; sy < y1; sy++) {
                    const uint16_t *p = src + (size_t)sy * sstride + x0;
                    for (int sx = x0; sx < x1; sx++, p++) {
                        uint16_t c = *p;
                        r += c >> 11;
                        g += (c >> 5) & 0x3F;
                        b += c & 0x1F;
                    }
                    n += x1 - x0;
                }
                out[x] = n ? (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n)) : 0;
            }
        }
    }
    free(xs);
    free(ys);
    return true;
}

bool img_scale(const uint16_t *src, int sw, int sh, int sstride, uint16_t *dst, int dw, int dh)
{
    return img_scale_ex(src, sw, sh, sstride, dst, dw, dw, dh, 0, 0, dw, dh);
}
