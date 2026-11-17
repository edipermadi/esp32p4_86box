#pragma once
/*
 * Performance page: LVGL's own frame-rate / CPU / render-time figures (the
 * sysmon perf monitor) as live numbers and scrolling graphs. Also hides the
 * monitor's small text overlay in the bottom-right corner.
 */
#include <stdbool.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the page into scr (a screen of its own). back_cb runs on the back
 * button. History is recorded from now on, also while the page is not shown. */
void perf_ui_create(lv_obj_t *scr, lv_event_cb_t back_cb);

#ifdef __cplusplus
}
#endif
