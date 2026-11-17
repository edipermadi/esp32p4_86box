#pragma once
/*
 * Software JPEG decoder for what the P4's hardware decoder can't do:
 * progressive JPEGs, and sizes whose width x height isn't a multiple of 8.
 * Uses stb_image v2.30 (third_party/stb_image.h, public domain / MIT),
 * compiled for JPEG only with all its memory in PSRAM. Slower than the
 * hardware (around a second for a 2 Mpx progressive picture).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes data into a new RGB565 (little endian) w x h buffer, tightly packed;
 * free it with jpeg_sw_free(). On failure returns NULL and puts a reason in
 * why (e.g. "too large for the software decoder (6000x4000)"). */
uint16_t *jpeg_sw_decode(const uint8_t *data, size_t size, int *w, int *h, char *why, size_t why_len);
void jpeg_sw_free(uint16_t *pixels);

#ifdef __cplusplus
}
#endif
