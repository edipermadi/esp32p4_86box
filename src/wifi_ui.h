#pragma once
/*
 * Wi-Fi page: on/off switch, the current connection (with Forget), and the
 * list of nearby networks; tapping one asks for its password with an
 * on-screen keyboard and connects (see wifi_mgr.h).
 */
#include <stdbool.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the page into scr (a screen of its own). back_cb runs on the back button. */
void wifi_ui_create(lv_obj_t *scr, lv_event_cb_t back_cb);

/* Called by app_ui when the page is loaded (true) or left (false). */
void wifi_ui_set_shown(bool shown);

/* One-line summary for the Settings page, e.g. "MyNetwork" or "Off". */
const char *wifi_ui_summary(void);

#ifdef __cplusplus
}
#endif
