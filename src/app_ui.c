/*
 * Home page + settings page, and switching between them and the player
 * (see app_ui.h). Same dark palette as player_ui.c.
 */
#include "app_ui.h"
#include "ui_theme.h"
#include "perf_ui.h"
#include "gallery.h"
#include "wifi_ui.h"
#include "wifi_mgr.h"
#include "timezones.h"
#include "usb_msc.h"
#include "player_ui.h"
#include "mp3_player.h"
#include "settings.h"
#include "spectrum.h"
#include "bsp/bsp_board.h"
#include "fonts/font_ui.h"
#include "fonts/emoji_font.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <lvgl.h>


static lv_obj_t *s_home, *s_player, *s_settings, *s_perf, *s_wifi, *s_gallery;
static bool s_player_ok;

/* home */
static lv_obj_t *s_music_sub;
/* clock */
static lv_obj_t *s_clock, *s_ampm, *s_date, *s_net_icon;
static int s_clock_shown = -1;          /* minute of day shown, -1 = none */
/* mini player */
static lv_obj_t *s_mini, *s_mini_title, *s_mini_sub, *s_mini_bar, *s_mini_play, *s_mini_play_lbl;
static lv_obj_t *s_mini_prev, *s_mini_next;
static int s_mini_track = -2;
static uint32_t s_mini_serial = UINT32_MAX, s_mini_meta = UINT32_MAX;
static lv_timer_t *s_home_timer;

/* screen timeout */
static lv_obj_t *s_blank;               /* black catcher on the top layer while dark */
static bool s_dark;
static const uint8_t TIMEOUTS[] = { 1, 2, 5, 10, 15, 30, 0 };   /* minutes, 0 = never */
#define TIMEOUT_OPTS "1 minute\n2 minutes\n5 minutes\n10 minutes\n15 minutes\n30 minutes\nNever"
#define FADE_MS 1500

/* settings */
static lv_obj_t *s_bright_val, *s_wf_switch;
static lv_obj_t *s_info_tracks, *s_info_ram, *s_info_psram, *s_info_up, *s_wifi_val;
static lv_obj_t *s_usb_switch, *s_usb_note;
static lv_timer_t *s_info_timer;

/* ------------------------------------------------------------- helpers */
static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, C_TEXT, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 20, 0);
    lv_obj_set_style_pad_bottom(scr, 44, 0);       /* same bezel margin as the player */
    lv_obj_set_style_pad_row(scr, 14, 0);
    return scr;
}

static lv_obj_t *plain_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, C_ROW_SEL, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, C_TEXT, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &font_ui_16, 0);
    lv_obj_set_style_text_color(t, C_MUTED, 0);
    lv_obj_set_style_pad_left(t, 8, 0);
    lv_obj_set_style_margin_top(t, 6, 0);

    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(c, C_TEXT, 0);      /* the light theme would give #212121 */
    lv_obj_set_style_text_font(c, &font_ui_20, 0);
    return c;
}

/* "Name ........ value" row; returns the value label */
static lv_obj_t *kv_row(lv_obj_t *parent, const char *name, const char *value)
{
    lv_obj_t *row = plain_row(parent);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, name);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, C_MUTED, 0);
    return v;
}

/* ---------------------------------------------------------- navigation */
static void show(lv_obj_t *scr)
{
    if (lv_screen_active() == scr) {
        return;
    }
    if (s_player_ok) {
        player_ui_set_shown(scr == s_player);
    }
    if (scr == s_home) {
        lv_timer_resume(s_home_timer);
        lv_timer_ready(s_home_timer);
    } else {
        lv_timer_pause(s_home_timer);
    }
    wifi_ui_set_shown(scr == s_wifi);
    gallery_set_shown(scr == s_gallery);
    if (scr == s_perf) {
        lv_obj_scroll_to_y(s_perf, 0, LV_ANIM_OFF);
    }
    if (scr == s_settings) {
        lv_obj_scroll_to_y(s_settings, 0, LV_ANIM_OFF);
        lv_timer_resume(s_info_timer);
        lv_timer_ready(s_info_timer);
    } else {
        lv_timer_pause(s_info_timer);
    }
    lv_screen_load(scr);
}

void app_ui_show_home(void)     { show(s_home); }
void app_ui_show_player(void)   { if (s_player_ok) show(s_player); }
void app_ui_show_settings(void) { show(s_settings); }
void app_ui_show_perf(void)     { show(s_perf); }
void app_ui_show_gallery(void)  { show(s_gallery); }

static void on_music(lv_event_t *e)    { app_ui_show_player(); }
static void on_settings(lv_event_t *e) { app_ui_show_settings(); }
static void on_back(lv_event_t *e)     { app_ui_show_home(); }
static void on_wifi(lv_event_t *e)     { show(s_wifi); }
static void on_back_settings(lv_event_t *e) { show(s_settings); }
static void on_perf(lv_event_t *e)     { app_ui_show_perf(); }
static void on_gallery(lv_event_t *e)  { app_ui_show_gallery(); }

/* ---------------------------------------------------------------- home */
static lv_obj_t *tile(lv_obj_t *parent, const char *sym, lv_color_t tint,
                      const char *name, lv_event_cb_t cb, lv_obj_t **sub_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_bg_color(b, C_ROW_SEL, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(b, 14, 0);
    lv_obj_set_style_pad_row(b, 10, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *circle = lv_obj_create(b);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 88, 88);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(circle, tint, 0);
    lv_obj_set_style_margin_bottom(circle, 14, 0);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *icon = lv_label_create(circle);
    lv_label_set_text(icon, sym);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, C_TEXT, 0);
    lv_obj_set_style_transform_scale(icon, 384, 0);   /* 1.5x: no 42 px font built in */
    lv_obj_set_style_transform_pivot_x(icon, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(icon, LV_PCT(50), 0);
    lv_obj_center(icon);

    lv_obj_t *n = lv_label_create(b);
    lv_label_set_text(n, name);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(n, C_TEXT, 0);

    lv_obj_t *sub = lv_label_create(b);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(sub, &font_ui_16, 0);
    lv_obj_set_style_text_color(sub, C_MUTED, 0);
    lv_label_set_text(sub, "");
    if (sub_out) {
        *sub_out = sub;
    }
    return b;
}

static void set_text(lv_obj_t *l, const char *txt)
{
    if (strcmp(lv_label_get_text(l), txt) != 0) {
        lv_label_set_text(l, txt);
    }
}

static void set_enabled(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_remove_state(o, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(o, LV_STATE_DISABLED);
    }
}

/* Clock: local time once SNTP has set it (there is no battery-backed RTC,
 * so until then the system clock is near 1970 and "--:--" is shown). */
static void clock_tick(bool force)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    bool valid = now > 1700000000;                     /* after Nov 2023: really set */
    const player_settings_t *ps = settings_get();
    int key = valid ? (tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min) * 2 + ps->clock24 : -2;
    if (key == s_clock_shown && !force) {
        return;
    }
    s_clock_shown = key;
    char buf[64];
    if (!valid) {
        set_text(s_clock, "--:--");
        set_text(s_ampm, "");
        wifi_status_t ws;
        wifi_mgr_get_status(&ws);
        set_text(s_date, ws.state == WIFI_ST_CONNECTED ? "Getting the time..."
                                                       : "Connect to Wi-Fi to set the clock");
        return;
    }
    if (ps->clock24) {
        strftime(buf, sizeof(buf), "%H:%M", &tm);
        set_text(s_ampm, "");
    } else {
        int h = tm.tm_hour % 12;
        snprintf(buf, sizeof(buf), "%d:%02d", h ? h : 12, tm.tm_min);
        set_text(s_ampm, tm.tm_hour < 12 ? "AM" : "PM");
    }
    set_text(s_clock, buf);
    strftime(buf, sizeof(buf), "%A, %e %B", &tm);
    set_text(s_date, buf);
}

static void net_icon_tick(void)
{
    wifi_status_t ws;
    wifi_mgr_get_status(&ws);
    lv_color_t c = ws.state == WIFI_ST_CONNECTED
                   ? (ws.rssi >= -60 ? C_GREEN : (ws.rssi >= -75 ? C_TEXT : C_MUTED))
                   : C_ROW_SEL;                         /* off / not connected: dim */
    lv_obj_set_style_text_color(s_net_icon, c, 0);
}

/* Clock, Music tile caption (track count) and the mini player */
static void home_tick(lv_timer_t *t)
{
    char buf[160];
    clock_tick(false);
    net_icon_tick();
    if (!s_player_ok) {
        set_text(s_music_sub, "Not available");
        return;
    }
    int n = mp3_player_count();
    if (n < 0) {
        snprintf(buf, sizeof(buf), "No SD card");
    } else {
        snprintf(buf, sizeof(buf), "%d track%s", n, n == 1 ? "" : "s");
    }
    set_text(s_music_sub, buf);

    mp3_status_t st;
    mp3_player_get_status(&st);
    bool active = st.track >= 0 && st.state != MP3_STOPPED;
    set_text(s_mini_play_lbl, st.state == MP3_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    set_enabled(s_mini_play, n > 0);
    set_enabled(s_mini_prev, active);
    set_enabled(s_mini_next, active);

    /* title / artist: only when the track or its tag changes */
    uint32_t meta = mp3_player_meta_serial();
    int track = active ? st.track : -1;
    if (track != s_mini_track || st.serial != s_mini_serial || meta != s_mini_meta) {
        s_mini_track = track;
        s_mini_serial = st.serial;
        s_mini_meta = meta;
        if (track >= 0) {
            set_text(s_mini_title, mp3_player_title(track));
            const char *artist = mp3_player_artist(track);
            set_text(s_mini_sub, artist[0] ? artist : mp3_player_folder(track));
        } else {
            set_text(s_mini_title, "Nothing playing");
            set_text(s_mini_sub, n > 0 ? "Tap " LV_SYMBOL_PLAY " to start" : "");
        }
    }
    int32_t v = (active && st.dur_ms) ? (int32_t)((uint64_t)st.pos_ms * 1000 / st.dur_ms) : 0;
    lv_bar_set_value(s_mini_bar, v > 1000 ? 1000 : v, LV_ANIM_OFF);
}

static void on_mini_play(lv_event_t *e) { mp3_player_toggle(); lv_timer_ready(s_home_timer); }
static void on_mini_prev(lv_event_t *e) { mp3_player_prev();   lv_timer_ready(s_home_timer); }
static void on_mini_next(lv_event_t *e) { mp3_player_next();   lv_timer_ready(s_home_timer); }

static lv_obj_t *mini_btn(lv_obj_t *parent, const char *sym, int size, bool primary,
                          lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, primary ? C_ACCENT : C_ROW_SEL, 0);
    lv_obj_set_style_bg_color(b, C_ROW_SEL, LV_STATE_DISABLED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, C_TEXT, 0);
    lv_obj_center(l);
    if (lbl_out) {
        *lbl_out = l;
    }
    return b;
}

/* Bottom bar: [cover icon | title / artist  -> opens the player] [prev][play][next]
 *             [-------------- progress -------------------------------------------] */
static void build_mini(lv_obj_t *parent)
{
    s_mini = lv_obj_create(parent);
    lv_obj_set_size(s_mini, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_mini, C_CARD, 0);
    lv_obj_set_style_border_width(s_mini, 0, 0);
    lv_obj_set_style_radius(s_mini, 20, 0);
    lv_obj_set_style_pad_all(s_mini, 16, 0);
    lv_obj_set_style_pad_row(s_mini, 14, 0);
    lv_obj_set_flex_flow(s_mini, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_mini, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(s_mini, C_TEXT, 0);

    lv_obj_t *row = plain_row(s_mini);
    lv_obj_set_style_pad_column(row, 14, 0);

    /* left part: tap to open the full player */
    lv_obj_t *info = lv_obj_create(row);
    lv_obj_remove_style_all(info);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(info, 14, 0);
    lv_obj_set_style_pad_all(info, 6, 0);
    lv_obj_set_style_radius(info, 12, 0);
    lv_obj_set_style_bg_color(info, C_ROW_SEL, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(info, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(info, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(info, on_music, LV_EVENT_CLICKED, NULL);

    lv_obj_t *art = lv_obj_create(info);
    lv_obj_remove_style_all(art);
    lv_obj_set_size(art, 60, 60);
    lv_obj_set_style_radius(art, 12, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(art, C_ROW_SEL, 0);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *note = lv_label_create(art);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(note, C_ACCENT, 0);
    lv_obj_center(note);

    lv_obj_t *col = lv_obj_create(info);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    s_mini_title = lv_label_create(col);
    lv_obj_set_width(s_mini_title, LV_PCT(100));
    lv_label_set_long_mode(s_mini_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_mini_title, &emoji_font_20, 0);
    lv_label_set_text(s_mini_title, "");
    s_mini_sub = lv_label_create(col);
    lv_obj_set_width(s_mini_sub, LV_PCT(100));
    lv_label_set_long_mode(s_mini_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_mini_sub, &emoji_font_16, 0);
    lv_obj_set_style_text_color(s_mini_sub, C_MUTED, 0);
    lv_label_set_text(s_mini_sub, "");

    s_mini_prev = mini_btn(row, LV_SYMBOL_PREV, 56, false, on_mini_prev, NULL);
    s_mini_play = mini_btn(row, LV_SYMBOL_PLAY, 72, true, on_mini_play, &s_mini_play_lbl);
    s_mini_next = mini_btn(row, LV_SYMBOL_NEXT, 56, false, on_mini_next, NULL);

    s_mini_bar = lv_bar_create(s_mini);
    lv_obj_set_size(s_mini_bar, LV_PCT(100), 4);
    lv_bar_set_range(s_mini_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_mini_bar, C_ROW_SEL, 0);
    lv_obj_set_style_bg_color(s_mini_bar, C_ACCENT, LV_PART_INDICATOR);
}

static void build_home(void)
{
    s_home = new_screen();
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(s_home, 20, 0);

    /* clock: big time (+ AM/PM), date underneath, Wi-Fi state on the right */
    lv_obj_t *top = plain_row(s_home);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(top, 6, 0);
    lv_obj_t *col = lv_obj_create(top);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_t *trow = lv_obj_create(col);
    lv_obj_remove_style_all(trow);
    lv_obj_set_size(trow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(trow, 10, 0);
    s_clock = lv_label_create(trow);
    lv_obj_set_style_text_font(s_clock, &font_clock_96, 0);
    lv_obj_set_style_text_color(s_clock, C_TEXT, 0);
    lv_label_set_text(s_clock, "");
    s_ampm = lv_label_create(trow);
    lv_obj_set_style_text_font(s_ampm, &font_ui_24, 0);
    lv_obj_set_style_text_color(s_ampm, C_MUTED, 0);
    lv_obj_set_style_pad_bottom(s_ampm, 10, 0);
    lv_label_set_text(s_ampm, "");
    s_date = lv_label_create(col);
    lv_obj_set_style_text_font(s_date, &font_ui_20, 0);
    lv_obj_set_style_text_color(s_date, C_MUTED, 0);
    lv_obj_set_style_pad_left(s_date, 4, 0);
    lv_label_set_text(s_date, "");
    s_net_icon = lv_label_create(top);
    lv_label_set_text(s_net_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_net_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_pad_top(s_net_icon, 14, 0);

    lv_obj_t *row = plain_row(s_home);
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    tile(row, LV_SYMBOL_AUDIO, C_ACCENT, "Music", on_music, &s_music_sub);
    lv_obj_t *sub;
    tile(row, LV_SYMBOL_SETTINGS, C_ACCENT2, "Settings", on_settings, &sub);
    lv_label_set_text(sub, "Wi-Fi, display, clock");
    tile(row, LV_SYMBOL_IMAGE, C_ORANGE, "Gallery", on_gallery, &sub);
    lv_label_set_text(sub, "Photos & GIFs");
    tile(row, LV_SYMBOL_CHARGE, C_GREEN, "Monitor", on_perf, &sub);
    lv_label_set_text(sub, "FPS, CPU, temp");

    build_mini(s_home);
    if (!s_player_ok) {
        lv_obj_add_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
    }

    s_home_timer = lv_timer_create(home_tick, 250, NULL);
}

/* ------------------------------------------------------------ settings */
static void show_brightness(int v)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d %%", v);
    lv_label_set_text(s_bright_val, buf);
}

static void on_brightness(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    bsp_display_brightness_set(v);
    show_brightness(v);
    settings_get()->brightness = (uint8_t)v;
    settings_changed();
}

static void show_usb(void)
{
    usb_msc_state_t st = usb_msc_state();
    const char *txt;
    lv_color_t c = C_MUTED;
    switch (st) {
    case USB_MSC_ON:      txt = "The SD card appears as a drive on a PC connected to the USB OTG port. "
                                "Eject it on the PC before switching off."; break;
    case USB_MSC_NO_CARD: txt = "No SD card found."; c = C_ORANGE; break;
    case USB_MSC_FAILED:  txt = "USB could not be started."; c = C_ORANGE; break;
    case USB_MSC_OFF:
    default:              txt = "Off: the SD card is only used by this device. "
                                "Switching off re-reads the music list."; break;
    }
    lv_label_set_text(s_usb_note, txt);
    lv_obj_set_style_text_color(s_usb_note, c, 0);
    lv_obj_set_state(s_usb_switch, LV_STATE_CHECKED, st == USB_MSC_ON);
}

static void on_usb(lv_event_t *e)
{
    bool on = lv_obj_has_state(s_usb_switch, LV_STATE_CHECKED);
    usb_msc_set_enabled(on);
    if (!on && s_player_ok) {
        player_ui_request_rescan();                 /* pick up what the PC copied */
    }
    settings_get()->usb_drive = on;
    settings_changed();
    show_usb();                                      /* failed start flips the switch back */
}

static void on_tz(lv_event_t *e)
{
    const tz_entry_t *z = tz_get((int)lv_dropdown_get_selected(lv_event_get_target_obj(e)));
    if (z) {
        player_settings_t *ps = settings_get();
        strlcpy(ps->tz, z->name, sizeof(ps->tz));
        tz_apply(z->name);
        s_clock_shown = -1;                          /* redraw the home clock */
        settings_changed();
    }
}

static void on_gallery_random(lv_event_t *e)
{
    /* takes effect the next time the gallery opens */
    settings_get()->gallery_random = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    settings_changed();
}

static void on_clock24(lv_event_t *e)
{
    settings_get()->clock24 = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    s_clock_shown = -1;
    settings_changed();
}

static void style_dropdown(lv_obj_t *dd, int width)
{
    lv_obj_set_width(dd, width);
    lv_obj_set_style_bg_color(dd, C_ROW_SEL, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_set_style_radius(dd, 12, 0);
    lv_obj_set_style_text_color(dd, C_TEXT, 0);
    lv_obj_set_style_text_font(dd, &font_ui_20, 0);
    lv_obj_set_style_pad_ver(dd, 12, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);     /* the pop-up: dark, roomy rows */
    lv_obj_set_style_bg_color(list, C_ROW_SEL, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 12, 0);
    lv_obj_set_style_text_color(list, C_TEXT, 0);
    lv_obj_set_style_text_font(list, &font_ui_20, 0);
    lv_obj_set_style_text_line_space(list, 22, 0);
    lv_obj_set_style_max_height(list, 420, 0);
    lv_obj_set_style_bg_color(list, C_ACCENT, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(list, C_ACCENT, LV_PART_SELECTED | LV_STATE_PRESSED);
}

static const lv_display_rotation_t ROTATIONS[4] = {
    LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90, LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270,
};

static void on_rotation(lv_event_t *e)
{
    uint32_t i = lv_dropdown_get_selected(lv_event_get_target_obj(e));
    if (i < 4) {
        settings_get()->rotation = (uint8_t)i;
        settings_changed();
        lv_display_set_rotation(lv_display_get_default(), ROTATIONS[i]);   /* redraws everything */
    }
}

static void on_timeout(lv_event_t *e)
{
    uint32_t i = lv_dropdown_get_selected(lv_event_get_target_obj(e));
    if (i < sizeof(TIMEOUTS)) {
        settings_get()->timeout_min = TIMEOUTS[i];
        settings_changed();
    }
}

static void on_waterfall(lv_event_t *e)
{
    /* the player picks this up when it is shown next */
    settings_get()->waterfall = lv_obj_has_state(s_wf_switch, LV_STATE_CHECKED);
    settings_changed();
}

static void info_tick(lv_timer_t *t)
{
    char buf[48];
    /* the analyzer can be toggled on the player page: keep the switch in step */
    if (settings_get()->waterfall != lv_obj_has_state(s_wf_switch, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_wf_switch, LV_STATE_CHECKED, settings_get()->waterfall);
    }
    int n = s_player_ok ? mp3_player_count() : -1;
    if (n < 0) {
        snprintf(buf, sizeof(buf), "no card");
    } else {
        snprintf(buf, sizeof(buf), "%d", n);
    }
    lv_label_set_text(s_info_tracks, buf);
    lv_label_set_text(s_wifi_val, wifi_ui_summary());
    snprintf(buf, sizeof(buf), "%u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text(s_info_ram, buf);
    snprintf(buf, sizeof(buf), "%.1f MB free",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024.0 * 1024.0));
    lv_label_set_text(s_info_psram, buf);
    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
             (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    lv_label_set_text(s_info_up, buf);
}

static void build_settings(void)
{
    const player_settings_t *ps = settings_get();
    s_settings = new_screen();
    lv_obj_set_scroll_dir(s_settings, LV_DIR_VER);
    lv_obj_set_style_pad_row(s_settings, 10, 0);

    lv_obj_t *hdr = plain_row(s_settings);
    lv_obj_set_style_pad_column(hdr, 16, 0);
    lv_obj_set_style_pad_bottom(hdr, 6, 0);
    round_btn(hdr, LV_SYMBOL_LEFT, on_back);
    lv_obj_t *h = lv_label_create(hdr);
    lv_label_set_text(h, "Settings");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_28, 0);

    /* network: one row that opens the Wi-Fi page */
    lv_obj_t *nc = card(s_settings, "NETWORK");
    lv_obj_t *wrow = plain_row(nc);
    lv_obj_set_style_pad_column(wrow, 12, 0);
    lv_obj_set_style_pad_ver(wrow, 6, 0);
    lv_obj_set_style_radius(wrow, 10, 0);
    lv_obj_set_style_bg_color(wrow, C_ROW_SEL, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(wrow, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(wrow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wrow, on_wifi, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wl = lv_label_create(wrow);
    lv_label_set_text(wl, LV_SYMBOL_WIFI "   Wi-Fi");
    lv_obj_set_flex_grow(wl, 1);
    s_wifi_val = lv_label_create(wrow);
    lv_obj_set_style_max_width(s_wifi_val, 300, 0);
    lv_label_set_long_mode(s_wifi_val, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_wifi_val, &emoji_font_20, 0);
    lv_obj_set_style_text_color(s_wifi_val, C_MUTED, 0);
    lv_label_set_text(s_wifi_val, "");
    lv_obj_t *chev = lv_label_create(wrow);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chev, C_MUTED, 0);

    /* storage */
    lv_obj_t *uc = card(s_settings, "STORAGE");
    lv_obj_t *urow = plain_row(uc);
    lv_obj_set_flex_align(urow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ul = lv_label_create(urow);
    lv_label_set_text(ul, LV_SYMBOL_USB "   USB drive (SD card)");
    s_usb_switch = lv_switch_create(urow);
    lv_obj_set_size(s_usb_switch, 72, 38);
    lv_obj_set_style_bg_color(s_usb_switch, C_ROW_SEL, 0);
    lv_obj_set_style_bg_color(s_usb_switch, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_usb_switch, C_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_usb_switch, on_usb, LV_EVENT_VALUE_CHANGED, NULL);
    s_usb_note = lv_label_create(uc);
    lv_obj_set_width(s_usb_note, LV_PCT(100));
    lv_label_set_long_mode(s_usb_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_usb_note, &font_ui_16, 0);
    show_usb();

    /* display */
    lv_obj_t *c = card(s_settings, "DISPLAY");
    lv_obj_t *row = plain_row(c);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, LV_SYMBOL_IMAGE "   Brightness");
    s_bright_val = lv_label_create(row);
    lv_obj_set_style_text_color(s_bright_val, C_MUTED, 0);
    show_brightness(ps->brightness);
    lv_obj_t *sl = lv_slider_create(c);
    lv_obj_set_width(sl, LV_PCT(100));
    lv_obj_set_height(sl, 10);
    lv_obj_set_style_margin_hor(sl, 12, 0);
    lv_obj_set_style_margin_ver(sl, 12, 0);
    lv_obj_set_style_pad_all(sl, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 28);
    lv_slider_set_range(sl, 10, 100);               /* never fully dark: you could not find it again */
    lv_slider_set_value(sl, ps->brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, C_ROW_SEL, 0);
    lv_obj_set_style_bg_color(sl, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, C_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, on_brightness, LV_EVENT_VALUE_CHANGED, NULL);

    row = plain_row(c);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l = lv_label_create(row);
    lv_label_set_text(l, LV_SYMBOL_EYE_CLOSE "   Screen off after");
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options_static(dd, TIMEOUT_OPTS);
    style_dropdown(dd, 200);
    uint32_t sel = 2;                               /* 5 minutes */
    for (uint32_t i = 0; i < sizeof(TIMEOUTS); i++) {
        if (TIMEOUTS[i] == ps->timeout_min) {
            sel = i;
        }
    }
    lv_dropdown_set_selected(dd, sel);
    lv_obj_add_event_cb(dd, on_timeout, LV_EVENT_VALUE_CHANGED, NULL);

    row = plain_row(c);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l = lv_label_create(row);
    lv_label_set_text(l, LV_SYMBOL_REFRESH "   Screen rotation");
    lv_obj_t *rdd = lv_dropdown_create(row);
    lv_dropdown_set_options_static(rdd, "0\xC2\xB0\n90\xC2\xB0\n180\xC2\xB0\n270\xC2\xB0");
    style_dropdown(rdd, 200);
    lv_dropdown_set_selected(rdd, ps->rotation < 4 ? ps->rotation : 0);
    lv_obj_add_event_cb(rdd, on_rotation, LV_EVENT_VALUE_CHANGED, NULL);

    /* clock */
    c = card(s_settings, "CLOCK");
    row = plain_row(c);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l = lv_label_create(row);
    lv_label_set_text(l, LV_SYMBOL_GPS "   Time zone");
    lv_obj_t *tzdd = lv_dropdown_create(row);
    lv_dropdown_set_options_static(tzdd, tz_options());
    style_dropdown(tzdd, 330);
    lv_dropdown_set_selected(tzdd, (uint32_t)tz_find(ps->tz));
    lv_obj_add_event_cb(tzdd, on_tz, LV_EVENT_VALUE_CHANGED, NULL);

    row = plain_row(c);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l = lv_label_create(row);
    lv_label_set_text(l, LV_SYMBOL_LOOP "   24-hour clock");
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 72, 38);
    lv_obj_set_style_bg_color(sw, C_ROW_SEL, 0);
    lv_obj_set_style_bg_color(sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, C_TEXT, LV_PART_KNOB);
    lv_obj_set_state(sw, LV_STATE_CHECKED, ps->clock24);
    lv_obj_add_event_cb(sw, on_clock24, LV_EVENT_VALUE_CHANGED, NULL);

    l = lv_label_create(c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, "Time comes from the internet (NTP) once Wi-Fi is connected.");
    lv_obj_set_style_text_font(l, &font_ui_16, 0);
    lv_obj_set_style_text_color(l, C_MUTED, 0);

    /* slideshow */
    c = card(s_settings, "SLIDESHOW");
    row = plain_row(c);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l = lv_label_create(row);
    lv_label_set_text(l, LV_SYMBOL_SHUFFLE "   Random order");
    sw = lv_switch_create(row);
    lv_obj_set_size(sw, 72, 38);
    lv_obj_set_style_bg_color(sw, C_ROW_SEL, 0);
    lv_obj_set_style_bg_color(sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, C_TEXT, LV_PART_KNOB);
    lv_obj_set_state(sw, LV_STATE_CHECKED, ps->gallery_random);
    lv_obj_add_event_cb(sw, on_gallery_random, LV_EVENT_VALUE_CHANGED, NULL);
    l = lv_label_create(c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, "Shows .jpg and .gif files from the \"" GALLERY_DIR "\" folder on the SD card, "
                         "a new one every 30 s. Off: in name order. The screen stays on "
                         "while the slideshow runs.");
    lv_obj_set_style_text_font(l, &font_ui_16, 0);
    lv_obj_set_style_text_color(l, C_MUTED, 0);

    /* music */
    c = card(s_settings, "MUSIC");
    row = plain_row(c);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l = lv_label_create(row);
    lv_label_set_text(l, LV_SYMBOL_BARS "   Waterfall analyzer");
    s_wf_switch = lv_switch_create(row);
    lv_obj_set_size(s_wf_switch, 72, 38);
    lv_obj_set_style_bg_color(s_wf_switch, C_ROW_SEL, 0);
    lv_obj_set_style_bg_color(s_wf_switch, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_wf_switch, C_TEXT, LV_PART_KNOB);
    lv_obj_set_state(s_wf_switch, LV_STATE_CHECKED, ps->waterfall);
    lv_obj_add_event_cb(s_wf_switch, on_waterfall, LV_EVENT_VALUE_CHANGED, NULL);
    l = lv_label_create(c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, "Tap the analyzer in the player to switch views.");
    lv_obj_set_style_text_font(l, &font_ui_16, 0);
    lv_obj_set_style_text_color(l, C_MUTED, 0);

    /* about */
    c = card(s_settings, "ABOUT");
    kv_row(c, "Board", BSP_BOARD_NAME);
    char disp[40];
    snprintf(disp, sizeof(disp), "%dx%d %s", BSP_LCD_H_RES, BSP_LCD_V_RES, bsp_display_panel_name());
    kv_row(c, "Display", disp);
    s_info_tracks = kv_row(c, "Tracks on SD", "");
    s_info_ram = kv_row(c, "Internal RAM", "");
    s_info_psram = kv_row(c, "PSRAM", "");
    s_info_up = kv_row(c, "Uptime", "");

    s_info_timer = lv_timer_create(info_tick, 1000, NULL);
    lv_timer_pause(s_info_timer);
}

/* ------------------------------------------------------- screen timeout */
/* After the idle time the backlight fades out and a black, clickable layer
 * covers everything: the panel shows no static image, and the touch that
 * wakes it up is swallowed instead of hitting whatever was under the finger.
 * The player's analyzer is paused while dark; playback continues. */
static void fade_cb(void *var, int32_t v)
{
    bsp_display_brightness_set(v);
}

static void fade_done(lv_anim_t *a)
{
    lv_obj_remove_flag(s_blank, LV_OBJ_FLAG_HIDDEN);   /* fully dark: blank the image too */
}

static void wake(void)
{
    lv_anim_delete(&s_dark, fade_cb);
    bsp_display_brightness_set(settings_get()->brightness);
    if (s_dark) {
        s_dark = false;
        if (s_player_ok && lv_screen_active() == s_player) {
            player_ui_set_shown(true);
        }
    }
}

static void sleep_screen(void)
{
    s_dark = true;
    if (s_player_ok && lv_screen_active() == s_player) {
        player_ui_set_shown(false);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_dark);
    lv_anim_set_exec_cb(&a, fade_cb);
    lv_anim_set_values(&a, settings_get()->brightness, 0);
    lv_anim_set_duration(&a, FADE_MS);
    lv_anim_set_completed_cb(&a, fade_done);
    lv_anim_start(&a);
}

/* A touch during the fade (catcher still hidden) resets the idle time: wake. */
static void idle_tick(lv_timer_t *t)
{
    uint32_t idle = lv_display_get_inactive_time(NULL);
    uint32_t limit = settings_get()->timeout_min * 60000u;
    if (lv_screen_active() == s_gallery) {
        limit = 0;                                  /* photo frame: stays on */
    }
    if (!s_dark) {
        if (limit && idle >= limit) {
            sleep_screen();
        }
    } else if (lv_obj_has_flag(s_blank, LV_OBJ_FLAG_HIDDEN) && idle < 1000) {
        wake();
    }
}

static void on_blank(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        wake();                                       /* light up on touch-down */
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_add_flag(s_blank, LV_OBJ_FLAG_HIDDEN); /* next touch goes to the UI */
    }
}

static void build_blank(void)
{
    s_blank = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_blank);
    lv_obj_set_size(s_blank, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_blank, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_blank, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_blank, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_blank, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_blank, on_blank, LV_EVENT_ALL, NULL);
    lv_timer_create(idle_tick, 250, NULL);
}

/* ---------------------------------------------------------------- start */
esp_err_t app_ui_start(void)
{
    bsp_display_brightness_set(settings_get()->brightness);
    tz_apply(settings_get()->tz);
    if (settings_get()->rotation < 4) {
        lv_display_set_rotation(lv_display_get_default(), ROTATIONS[settings_get()->rotation]);
    }

    s_player = new_screen();
    esp_err_t err = player_ui_start(s_player);
    s_player_ok = (err == ESP_OK);

    build_settings();
    s_gallery = lv_obj_create(NULL);
    gallery_create(s_gallery, on_back);
    s_perf = new_screen();
    perf_ui_create(s_perf, on_back);
    wifi_mgr_start();
    s_wifi = new_screen();
    wifi_ui_create(s_wifi, on_back_settings);
    build_home();
    build_blank();

    lv_obj_t *old = lv_screen_active();
    show(s_home);
    lv_obj_delete(old);                             /* the BSP's empty boot screen */
    return err;
}
