#pragma once
/*
 * Colour emoji for LVGL labels, without enabling LV_USE_IMGFONT in lv_conf.h.
 *
 * emoji_font_16/20/24 are drop-in replacements for font_ui_16/20/24: each one
 * draws emoji as colour images and falls back to the matching font_ui_NN for
 * every other character (same line height / baseline, so layout is unchanged).
 *
 * Data: all 1378 single-code-point emoji (Unicode emoji-test.txt, fully
 * qualified) from Noto Color Emoji (SIL OFL 1.1), 20 px high, RGB565A8,
 * raw-deflate compressed (~895 KB flash). An emoji is decompressed into PSRAM
 * on first use (~1.3 KB each) with the ROM's tinfl and then kept.
 *
 * Limits: LVGL has no text shaping, so multi-code-point emoji are drawn piece
 * by piece. Joiners (U+200D), variation selectors (U+FE0E/F), skin-tone
 * modifiers and tag characters are made invisible, so a family shows as its
 * members and a toned thumbs-up as the default yellow one. Flags (regional
 * indicator pairs) are not included.
 */
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern lv_font_t emoji_font_16;
extern lv_font_t emoji_font_20;
extern lv_font_t emoji_font_24;

/* Fills in the three fonts. Call once before using them. */
void emoji_fonts_init(void);

#ifdef __cplusplus
}
#endif
