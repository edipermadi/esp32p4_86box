#pragma once
/*
 * Gallery page: full-screen slideshow of the .jpg/.jpeg/.gif files in the SD
 * card's "Animation" folder (and its subfolders), a new picture every 30 s,
 * in name order or shuffled (settings: gallery_random). Animated GIFs play.
 *
 * JPEGs use the P4's hardware decoder; what it can't do (progressive JPEGs,
 * odd sizes) goes to stb_image in software (jpeg_sw.c). Either way the
 * picture is box-filtered down to fit the screen in a background task, so
 * the UI never stalls. GIFs are decoded by gif_dec.c.
 *
 * Touch: tap the right half for the next picture, the left half for the
 * previous one (swiping works too); each tap shows the top bar (back, name,
 * position) for 4 s.
 */
#include <stdbool.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALLERY_DIR "Animation"

/* Builds the page into scr (a screen of its own). back_cb runs on the back button. */
void gallery_create(lv_obj_t *scr, lv_event_cb_t back_cb);

/* Called by app_ui when the page is loaded (true) or left (false). Showing it
 * re-reads the folder (so new files appear) and resumes; leaving pauses. */
void gallery_set_shown(bool shown);

#ifdef __cplusplus
}
#endif
