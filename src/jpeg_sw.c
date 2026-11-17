/*
 * Software JPEG decoding (see jpeg_sw.h).
 */
#include "jpeg_sw.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

#define PS_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC                 /* private copy: no clash with LVGL's glTF one */
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#define STBI_MALLOC(sz)        heap_caps_malloc((sz), PS_CAPS)
#define STBI_REALLOC(p, nsz)   heap_caps_realloc((p), (nsz), PS_CAPS)
#define STBI_FREE(p)           heap_caps_free(p)
#include "third_party/stb_image.h"

/* Rough peak memory for stb's JPEG path, bytes per pixel: 3 output + up to
 * 6 for progressive coefficients (4:4:4) + 3 component planes. */
#define BYTES_PER_PX 12

uint16_t *jpeg_sw_decode(const uint8_t *data, size_t size, int *w, int *h, char *why, size_t why_len)
{
    int iw = 0, ih = 0, comp = 0;
    if (size > 0x7fffffff || !stbi_info_from_memory(data, (int)size, &iw, &ih, &comp)) {
        snprintf(why, why_len, "Not a JPEG stb can read (%s)", stbi_failure_reason());
        return NULL;
    }
    uint64_t need = (uint64_t)iw * ih * BYTES_PER_PX;
    size_t avail = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (need > avail) {
        snprintf(why, why_len, "Too large for the software decoder (%dx%d, needs ~%u MB)",
                 iw, ih, (unsigned)(need >> 20));
        return NULL;
    }
    uint8_t *rgb = stbi_load_from_memory(data, (int)size, &iw, &ih, &comp, 3);
    if (!rgb) {
        snprintf(why, why_len, "Software JPEG decode failed (%s)", stbi_failure_reason());
        return NULL;
    }
    /* RGB888 -> RGB565 in place: each write (2 bytes) lands behind the read (3 bytes) */
    size_t n = (size_t)iw * ih;
    uint16_t *out = (uint16_t *)rgb;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = rgb + i * 3;
        out[i] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
    }
    *w = iw;
    *h = ih;
    return out;
}

void jpeg_sw_free(uint16_t *pixels)
{
    stbi_image_free(pixels);
}
