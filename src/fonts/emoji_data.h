#pragma once
/* Generated from Noto Color Emoji (SIL OFL 1.1) - see emoji_font.h. Do not edit. */
#include <stdint.h>

#define EMOJI_COUNT   1378
#define EMOJI_HEIGHT  20

/* Sorted by code point. Image i is RGB565A8 (w*h*2 colour bytes, then w*h alpha),
 * raw-deflate compressed in emoji_blob[emoji_off[i] .. emoji_off[i+1]). */
extern const uint32_t emoji_cp[EMOJI_COUNT];
extern const uint8_t  emoji_w[EMOJI_COUNT];
extern const uint32_t emoji_off[EMOJI_COUNT + 1];
extern const uint8_t  emoji_blob[];
