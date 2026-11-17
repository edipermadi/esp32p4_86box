#pragma once
/*
 * Spectrum analyzer: the player task feeds decoded PCM, an LVGL widget shows
 * 36 log-spaced bands (40 Hz .. 16 kHz) with peak caps at ~30 fps.
 */
#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Player task: interleaved stereo frames just before they go to the speaker.
 * Cheap, and does nothing while the analyzer is inactive. */
void spectrum_feed(const int16_t *stereo, int frames, uint32_t sample_rate);

/* LVGL task: creates the widget (hidden & inactive). Size it with flex/size calls. */
lv_obj_t *spectrum_create(lv_obj_t *parent);

/* Bars (false) or waterfall (true). A tap on the widget also toggles it and
 * then sends LV_EVENT_VALUE_CHANGED from the widget. */
void spectrum_set_waterfall(bool on);
bool spectrum_get_waterfall(void);

/* Shows it and starts analysis (true) or hides and stops it (false). */
void spectrum_set_active(bool on);

#ifdef __cplusplus
}
#endif
