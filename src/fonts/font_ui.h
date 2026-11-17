#pragma once
/*
 * Montserrat Medium 16/20/24 px, 4 bpp, with accented and extra characters plus
 * the LVGL symbols (LV_SYMBOL_*), so they can replace lv_font_montserrat_NN.
 *
 * Coverage: ASCII, Latin-1, Latin Extended-A/B, Cyrillic, Vietnamese,
 * dashes / curly quotes / ellipsis / bullet, euro, (TM). No Greek or CJK.
 *
 * Generated with lv_font_conv 1.5.2 from the font files in
 * lvgl/scripts/generators/built_in_font (Montserrat: SIL OFL 1.1,
 * FontAwesome 5 Free: SIL OFL 1.1 / CC BY 4.0). Regenerate with:
 *   node --stack-size=6000 lv_font_conv.js --no-compress --no-prefilter --bpp 4 --size NN \
 *     --font Montserrat-Medium.ttf -r <range> \
 *     --font FontAwesome5-Solid+Brands+Regular.woff -r <LVGL symbol list> \
 *     --format lvgl --lv-include lvgl.h -o font_ui_NN.c
 * (the full ranges are in each .c file's header; --stack-size works around a
 *  "Maximum call stack size exceeded" in lv_font_conv with many kerning pairs)
 */
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(font_ui_16)
LV_FONT_DECLARE(font_ui_20)
LV_FONT_DECLARE(font_ui_24)

/* Home-page clock: Montserrat Medium 96 px, only " -0123456789:" (font_clock_96.c):
 *   node --stack-size=6000 lv_font_conv.js --no-compress --no-prefilter --bpp 4 --size 96 \
 *     --font Montserrat-Medium.ttf -r 0x20,0x2D,0x30-0x3A --format lvgl --lv-include lvgl.h -o font_clock_96.c */
LV_FONT_DECLARE(font_clock_96)

#ifdef __cplusplus
}
#endif
