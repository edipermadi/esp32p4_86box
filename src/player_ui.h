#pragma once
#include <stdbool.h>
#include <lvgl.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the MP3 player into scr (a screen of its own): track list from the SD
 * card, now-playing card with progress, transport controls and volume.
 * Call after bsp_lvgl_start() and bsp_audio_init(), from the LVGL task. */
esp_err_t player_ui_start(lv_obj_t *scr);

/* Called by app_ui when the player screen is loaded (true) or left (false):
 * the analyzer and the status poll only run while it is on display.
 * Playback itself continues either way. */
void player_ui_set_shown(bool shown);

/* Re-reads the SD card's track list, e.g. after a PC changed it over USB.
 * A scan stops playback, so while a track is playing or paused it waits and
 * runs once playback has stopped. */
void player_ui_request_rescan(void);

#ifdef __cplusplus
}
#endif
