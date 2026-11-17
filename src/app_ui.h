#pragma once
/*
 * App shell: a home page (the start screen) with tiles for the MP3 player, the
 * settings page, the gallery and the performance monitor, plus a mini
 * player. Each page is its own LVGL screen; music keeps playing while you
 * move between them.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds all pages and shows the home page. Call after bsp_lvgl_start() and
 * bsp_audio_init(), from the LVGL task. Returns the player's start error, if
 * any (the home and settings pages still work then). */
esp_err_t app_ui_start(void);

/* Navigation (LVGL task). */
void app_ui_show_home(void);
void app_ui_show_player(void);
void app_ui_show_settings(void);
void app_ui_show_perf(void);
void app_ui_show_gallery(void);

#ifdef __cplusplus
}
#endif
