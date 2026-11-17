/*
 * LVGL front end for mp3_player. Runs entirely in the LVGL task: buttons send
 * commands, a 200 ms timer polls the engine's status and refreshes the view.
 */
#include "player_ui.h"
#include "mp3_player.h"
#include "bsp/bsp_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <lvgl.h>
#include "fonts/font_ui.h"   /* Montserrat + accents/Cyrillic/Vietnamese for tag & file names */
#include "fonts/emoji_font.h" /* ...plus colour emoji, falling back to font_ui */
#include "settings.h"
#include "spectrum.h"
#include "app_ui.h"

#define C_BG        lv_color_hex(0x101218)
#define C_CARD      lv_color_hex(0x1B1E27)
#define C_ROW_SEL   lv_color_hex(0x2A2F3D)
#define C_TEXT      lv_color_hex(0xEDEFF4)
#define C_MUTED     lv_color_hex(0x8B92A5)
#define C_ACCENT    lv_color_hex(0x4F8CFF)

static lv_obj_t *s_list, *s_count_lbl, *s_title, *s_sub, *s_bar, *s_time, *s_dur, *s_play_lbl;
static lv_obj_t *s_card, *s_list_icon, *s_spectrum;
static bool s_list_visible = true;      /* loaded from flash */
/* Virtual track list: a fixed pool of row objects reused while scrolling, so
 * LVGL memory (plain malloc, mostly internal RAM) stays the same for 20 or
 * 5000 tracks. Row slot r shows track i where i % POOL == r, at y = i * ROW_H;
 * an invisible spacer gives the list its full scroll height. */
#define ROW_H 58
#define POOL  16
static lv_obj_t *s_pool[POOL];
static int s_pool_idx[POOL];            /* track each row shows, -1 = none */
static lv_obj_t *s_spacer, *s_list_msg;
static int s_rows_n;                    /* tracks in the list */
static int s_cur_track = -1;            /* highlighted (playing) track */
static uint32_t s_shown_meta = UINT32_MAX;
static int s_shown_track = -2;
static uint32_t s_shown_serial = UINT32_MAX;
static bool s_dragging;                 /* finger on the progress slider */
static uint32_t s_seek_hold_until;      /* ignore stale positions right after a seek */
static uint32_t s_cur_dur_ms;

/* ------------------------------------------------------------- helpers */
static void fmt_time(char *buf, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
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

static lv_obj_t *icon_btn(lv_obj_t *parent, const char *sym, int size, bool primary,
                          lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, primary ? C_ACCENT : C_ROW_SEL, 0);
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

/* -------------------------------------------------------------- events */
static void on_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (i >= 0 && i < s_rows_n) {
        mp3_player_play(i);
    }
}
static void on_play(lv_event_t *e)  { mp3_player_toggle(); }
static void on_next(lv_event_t *e)  { mp3_player_next(); }
static void on_prev(lv_event_t *e)  { mp3_player_prev(); }
static void schedule_save(void);

/* ---- shuffle / repeat ---- */
static lv_obj_t *s_shuffle_btn, *s_repeat_btn, *s_repeat_one;

/* Mode buttons: no fill; icon is accent-coloured with a small dot underneath when on */
static void show_mode(lv_obj_t *btn, bool on)
{
    lv_obj_t *icon = lv_obj_get_child(btn, 0);
    lv_obj_set_style_text_color(icon, on ? C_ACCENT : C_MUTED, 0);
    lv_obj_t *dot = lv_obj_get_child(btn, 1);
    if (on) {
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_modes(void)
{
    mp3_repeat_t r = mp3_player_get_repeat();
    show_mode(s_shuffle_btn, mp3_player_get_shuffle());
    show_mode(s_repeat_btn, r != MP3_REPEAT_OFF);
    if (r == MP3_REPEAT_ONE) {
        lv_obj_remove_flag(s_repeat_one, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_repeat_one, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_shuffle(lv_event_t *e)
{
    mp3_player_set_shuffle(!mp3_player_get_shuffle());
    show_modes();
    schedule_save();
}

static void on_repeat(lv_event_t *e)        /* off -> all -> one -> off */
{
    mp3_repeat_t r = mp3_player_get_repeat();
    mp3_player_set_repeat(r == MP3_REPEAT_OFF ? MP3_REPEAT_ALL
                        : r == MP3_REPEAT_ALL ? MP3_REPEAT_ONE : MP3_REPEAT_OFF);
    show_modes();
    schedule_save();
}

static lv_obj_t *mode_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 56, 56);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, C_ROW_SEL, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(b);                 /* child 0 */
    lv_label_set_text(icon, sym);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -3);

    lv_obj_t *dot = lv_obj_create(b);                    /* child 1: "on" indicator */
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 5, 5);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, C_ACCENT, 0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

/* ---- volume + mute ---- */
static lv_obj_t *s_vol_slider, *s_vol_icon;
static int s_volume = 70;               /* last non-muted level, 0..100 (loaded from flash) */

/* Copies the player's state into the shared settings, saved shortly after. */
static void schedule_save(void)
{
    player_settings_t *ps = settings_get();
    ps->volume = (uint8_t)s_volume;
    ps->shuffle = mp3_player_get_shuffle();
    ps->repeat = (uint8_t)mp3_player_get_repeat();
    ps->show_list = s_list_visible;
    ps->waterfall = spectrum_get_waterfall();
    settings_changed();
}
static bool s_muted;

static void show_volume(void)
{
    const char *sym = (s_muted || s_volume == 0) ? LV_SYMBOL_MUTE
                    : (s_volume < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
    lv_label_set_text(s_vol_icon, sym);
    lv_obj_set_style_text_color(s_vol_icon, s_muted ? C_ACCENT : C_TEXT, 0);
    /* dim the slider while muted; it keeps the level to return to */
    lv_obj_set_style_opa(s_vol_slider, s_muted ? LV_OPA_50 : LV_OPA_COVER, 0);
}

static void on_vol(lv_event_t *e)
{
    s_volume = lv_slider_get_value(s_vol_slider);
    schedule_save();
    s_muted = false;                    /* moving the slider unmutes */
    bsp_audio_set_volume(s_volume);
    show_volume();
}

static void on_mute(lv_event_t *e)
{
    s_muted = !s_muted;
    bsp_audio_set_volume(s_muted ? 0 : s_volume);
    show_volume();
}

static void fmt_time(char *buf, size_t n, uint32_t ms);

/* Progress slider: preview the target time while dragging, seek on release. */
static void on_seek(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int32_t v = lv_slider_get_value(s_bar);
    if (code == LV_EVENT_PRESSED) {
        s_dragging = true;
    } else if (code == LV_EVENT_VALUE_CHANGED && s_dragging) {
        char buf[16];
        fmt_time(buf, sizeof(buf), (uint32_t)((uint64_t)s_cur_dur_ms * v / 1000));
        lv_label_set_text(s_time, buf);
    } else if (code == LV_EVENT_RELEASED) {
        s_dragging = false;
        mp3_status_t st;
        mp3_player_get_status(&st);
        if (st.state != MP3_STOPPED) {
            mp3_player_seek(v);
            s_seek_hold_until = lv_tick_get() + 500;
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        s_dragging = false;               /* finger slid off: cancel */
    }
}

/* Playlist shown: list fills the middle, card hugs its content at the bottom.
 * Hidden: the now-playing card takes the whole area, content centred. */
static bool s_shown;                    /* player screen is on display */
static lv_timer_t *s_refresh_timer;

static void apply_list_visibility(void)
{
    spectrum_set_active(s_shown && !s_list_visible);   /* only with the playlist hidden */
    if (s_list_visible) {
        lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_flex_grow(s_card, 0);
        lv_obj_set_height(s_card, LV_SIZE_CONTENT);
        lv_obj_set_flex_align(s_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    } else {
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_flex_grow(s_card, 1);
        lv_obj_set_flex_align(s_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    }
    lv_obj_set_style_text_color(s_list_icon, s_list_visible ? C_ACCENT : C_TEXT, 0);
}

static void ensure_visible(int idx, lv_anim_enable_t anim);
static void bind_visible(bool force);

static void on_toggle_list(lv_event_t *e)
{
    s_list_visible = !s_list_visible;
    apply_list_visibility();
    if (s_list_visible) {
        /* bring the playing track back into view */
        mp3_status_t st;
        mp3_player_get_status(&st);
        ensure_visible(st.track, LV_ANIM_OFF);
        bind_visible(false);
    }
    schedule_save();
}

static void rebuild_list(void);
static void list_message(const char *txt);
static void rescan(void)
{
    list_message("Scanning SD card...");
    lv_refr_now(NULL);
    mp3_player_scan();
    rebuild_list();
}

static void on_rescan(lv_event_t *e) { rescan(); }

/* Deferred rescan: waits while a track is playing or paused (a scan stops
 * playback), then runs once and deletes its timer. */
static lv_timer_t *s_rescan_timer;

static void rescan_when_idle(lv_timer_t *t)
{
    mp3_status_t st;
    mp3_player_get_status(&st);
    if (st.state != MP3_STOPPED) {
        return;                                     /* playing or paused: wait */
    }
    lv_timer_delete(t);
    s_rescan_timer = NULL;
    rescan();
}

void player_ui_request_rescan(void)
{
    if (!s_list || s_rescan_timer) {
        return;                                     /* player not built, or already pending */
    }
    s_rescan_timer = lv_timer_create(rescan_when_idle, 1000, NULL);
    rescan_when_idle(s_rescan_timer);               /* idle now: scan right away */
}

/* ---------------------------------------------------------------- list */
/* Row children: 0 = icon, 1 = title label, 2 = artist label */
static void bind_row(int r, int idx, bool force)
{
    lv_obj_t *row = s_pool[r];
    if (idx < 0 || idx >= s_rows_n) {
        if (s_pool_idx[r] != -1) {
            s_pool_idx[r] = -1;
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (force || s_pool_idx[r] != idx) {
        s_pool_idx[r] = idx;
        lv_obj_set_y(row, idx * ROW_H);
        lv_obj_set_user_data(row, (void *)(intptr_t)idx);
        lv_label_set_text(lv_obj_get_child(row, 1), mp3_player_title(idx));
        lv_obj_t *artist = lv_obj_get_child(row, 2);
        const char *a = mp3_player_artist(idx);
        lv_label_set_text(artist, a);
        if (a[0]) {
            lv_obj_remove_flag(artist, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(artist, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    if ((idx == s_cur_track) != lv_obj_has_state(row, LV_STATE_CHECKED)) {
        lv_obj_set_state(row, LV_STATE_CHECKED, idx == s_cur_track);
    }
}

/* Points the row pool at the tracks around the current scroll position.
 * force: re-read titles (tags arrived) even for rows that didn't move. */
static void bind_visible(bool force)
{
    if (s_rows_n <= 0 || lv_obj_has_flag(s_list, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    int first = lv_obj_get_scroll_y(s_list) / ROW_H - 1;     /* one spare row above */
    if (first < 0) {
        first = 0;
    }
    for (int k = 0; k < POOL; k++) {
        int idx = first + k;
        bind_row(idx % POOL, idx, force);
    }
}

static void on_list_scroll(lv_event_t *e)
{
    bind_visible(false);
}

/* Scrolls just enough to show track idx. */
static void ensure_visible(int idx, lv_anim_enable_t anim)
{
    if (idx < 0 || idx >= s_rows_n || lv_obj_has_flag(s_list, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_obj_update_layout(s_list);
    int32_t y = idx * ROW_H, h = lv_obj_get_content_height(s_list);
    int32_t cur = lv_obj_get_scroll_y(s_list);
    if (y < cur) {
        lv_obj_scroll_to_y(s_list, y, anim);
    } else if (y + ROW_H > cur + h) {
        lv_obj_scroll_to_y(s_list, y + ROW_H - h, anim);
    }
}

/* Shows a message in place of the rows (txt NULL: back to the rows). */
static void list_message(const char *txt)
{
    if (txt) {
        for (int r = 0; r < POOL; r++) {
            s_pool_idx[r] = -1;
            lv_obj_add_flag(s_pool[r], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_height(s_spacer, 0);
        lv_label_set_text(s_list_msg, txt);
        lv_obj_remove_flag(s_list_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_list_msg, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rebuild_list(void)
{
    s_shown_meta = UINT32_MAX;
    s_shown_track = -2;
    s_shown_serial = UINT32_MAX;
    s_cur_track = -1;

    int n = mp3_player_count();
    s_rows_n = 0;
    list_message("");                                 /* hides and resets all rows */
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    if (n <= 0) {
        list_message("No music found.\n\nCopy .mp3 or .opus files into the \"" MP3_MUSIC_DIR "\" folder "
                     "on the SD card (FAT32) - e.g. over the USB drive - then tap " LV_SYMBOL_REFRESH ".");
        lv_label_set_text(s_count_lbl, n < 0 ? "no card" : "0 tracks");
        return;
    }
    s_rows_n = n;
    list_message(NULL);
    lv_obj_set_height(s_spacer, (int32_t)n * ROW_H);
    lv_obj_update_layout(s_list);
    bind_visible(true);
    char buf[24];
    snprintf(buf, sizeof(buf), "%d track%s", n, n == 1 ? "" : "s");
    lv_label_set_text(s_count_lbl, buf);
}

/* The list container plus its fixed pool of rows. */
static void build_list(lv_obj_t *scr)
{
    s_list = lv_obj_create(scr);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_color(s_list, C_CARD, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 16, 0);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_style_text_color(s_list, C_TEXT, 0);
    lv_obj_add_event_cb(s_list, on_list_scroll, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_list, on_list_scroll, LV_EVENT_SIZE_CHANGED, NULL);

    s_spacer = lv_obj_create(s_list);                 /* sets the scroll height */
    lv_obj_remove_style_all(s_spacer);
    lv_obj_set_size(s_spacer, 1, 0);
    lv_obj_remove_flag(s_spacer, LV_OBJ_FLAG_CLICKABLE);

    for (int r = 0; r < POOL; r++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, C_CARD, 0);
        lv_obj_set_style_bg_color(row, C_ROW_SEL, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(row, C_ROW_SEL, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(row, C_TEXT, 0);
        lv_obj_set_style_text_color(row, C_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_text_font(row, &emoji_font_20, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, C_ROW_SEL, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, NULL);

        lv_obj_t *icon = lv_label_create(row);         /* 0 */
        lv_label_set_text(icon, LV_SYMBOL_AUDIO);
        lv_obj_t *title = lv_label_create(row);        /* 1 */
        lv_obj_set_flex_grow(title, 1);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_t *artist = lv_label_create(row);       /* 2: filled in once the tag is read */
        lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(artist, LV_PCT(38), 0);
        lv_obj_set_style_text_font(artist, &emoji_font_16, 0);
        lv_obj_set_style_text_color(artist, C_MUTED, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        s_pool[r] = row;
        s_pool_idx[r] = -1;
    }

    s_list_msg = lv_label_create(s_list);
    lv_obj_set_width(s_list_msg, LV_PCT(100));
    lv_label_set_long_mode(s_list_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_list_msg, C_MUTED, 0);
    lv_obj_set_style_text_font(s_list_msg, &font_ui_20, 0);
    lv_obj_set_style_pad_all(s_list_msg, 12, 0);
    lv_obj_add_flag(s_list_msg, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------- status poll */
static void refresh(lv_timer_t *t)
{
    mp3_status_t st;
    mp3_player_get_status(&st);

    lv_label_set_text(s_play_lbl, st.state == MP3_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    /* swap file names for ID3 titles/artists as the background reader gets to them */
    uint32_t ms = mp3_player_meta_serial();
    if (ms != s_shown_meta) {
        s_shown_meta = ms;
        bind_visible(true);                          /* rows on screen pick up new tags */
        if (st.track >= 0 && st.state != MP3_STOPPED) {
            lv_label_set_text(s_title, mp3_player_title(st.track));
        }
    }

    if (st.serial != s_shown_serial || st.track != s_shown_track) {
        s_cur_track = (st.track >= 0 && st.track < s_rows_n) ? st.track : -1;
        bind_visible(false);                         /* move the highlight */
        if (s_cur_track >= 0) {
            ensure_visible(st.track, LV_ANIM_ON);
            lv_label_set_text(s_title, mp3_player_title(st.track));
        } else {
            lv_label_set_text(s_title, "Nothing playing");
            lv_label_set_text(s_sub, mp3_player_count() > 0 ? "Tap a track to start" : "");
        }
        s_shown_track = st.track;
        s_shown_serial = st.serial;
    }

    char a[16], b[16], sub[160];
    if (st.track >= 0 && st.state != MP3_STOPPED) {
        /* "Artist - Album", else the folder, then the stream format */
        const char *artist = mp3_player_artist(st.track);
        const char *album = mp3_player_album(st.track);
        char who[96];
        if (artist[0] && album[0]) {
            snprintf(who, sizeof(who), "%s - %s", artist, album);
        } else if (artist[0] || album[0]) {
            snprintf(who, sizeof(who), "%s", artist[0] ? artist : album);
        } else {
            snprintf(who, sizeof(who), "%s", mp3_player_folder(st.track));
        }
        snprintf(sub, sizeof(sub), "%s%s%s  |  %.1f kHz  |  %u kbps  |  %s",
                 who, who[0] ? "  |  " : "", st.codec == MP3_CODEC_OPUS ? "Opus" : "MP3",
                 st.sample_rate / 1000.0f, st.kbps, st.channels == 1 ? "mono" : "stereo");
        lv_label_set_text(s_sub, sub);
        fmt_time(a, sizeof(a), st.pos_ms);
        fmt_time(b, sizeof(b), st.dur_ms);
        s_cur_dur_ms = st.dur_ms;
        lv_obj_remove_state(s_bar, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(s_sub, mp3_player_count() > 0 ? "Tap a track to start" : "");
        snprintf(a, sizeof(a), "0:00");
        snprintf(b, sizeof(b), "0:00");
        s_cur_dur_ms = 0;
        lv_obj_add_state(s_bar, LV_STATE_DISABLED);
    }
    lv_label_set_text(s_dur, b);
    /* leave the slider and time alone while the user drags or a seek is in flight */
    if (!s_dragging && (int32_t)(lv_tick_get() - s_seek_hold_until) >= 0) {
        int32_t v = (st.state != MP3_STOPPED && st.dur_ms)
                    ? (int32_t)((uint64_t)st.pos_ms * 1000 / st.dur_ms) : 0;
        lv_slider_set_value(s_bar, v > 1000 ? 1000 : v, LV_ANIM_OFF);
        lv_label_set_text(s_time, a);
    }
}

/* ---------------------------------------------------------------- build */
static void on_home(lv_event_t *e)     { app_ui_show_home(); }
static void on_spectrum_mode(lv_event_t *e) { schedule_save(); }

void player_ui_set_shown(bool shown)
{
    s_shown = shown;
    apply_list_visibility();
    if (shown) {
        bool wf = settings_get()->waterfall;        /* may have changed in Settings */
        if (wf != spectrum_get_waterfall()) {
            spectrum_set_waterfall(wf);
        }
        lv_timer_resume(s_refresh_timer);
        refresh(NULL);
    } else {
        lv_timer_pause(s_refresh_timer);
    }
}

esp_err_t player_ui_start(lv_obj_t *scr)
{
    esp_err_t err = mp3_player_init();
    if (err != ESP_OK) {
        return err;
    }
    emoji_fonts_init();

    /* restore volume / shuffle / repeat before the controls are built */
    const player_settings_t *ps = settings_get();
    s_volume = ps->volume;
    mp3_player_set_shuffle(ps->shuffle);
    mp3_player_set_repeat((mp3_repeat_t)ps->repeat);
    s_list_visible = ps->show_list;
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 20, 0);
    lv_obj_set_style_pad_bottom(scr, 44, 0);       /* keep controls off the bezel edge */
    lv_obj_set_style_pad_row(scr, 14, 0);
    lv_obj_set_style_text_color(scr, C_TEXT, 0);

    /* header */
    lv_obj_t *hdr = plain_row(scr);
    icon_btn(hdr, LV_SYMBOL_HOME, 52, false, on_home, NULL);
    lv_obj_t *h = lv_label_create(hdr);
    lv_label_set_text(h, LV_SYMBOL_SD_CARD "  SD Music");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_28, 0);
    lv_obj_set_flex_grow(h, 1);
    s_count_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_color(s_count_lbl, C_MUTED, 0);
    lv_obj_set_style_text_font(s_count_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_pad_right(s_count_lbl, 14, 0);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    icon_btn(hdr, LV_SYMBOL_REFRESH, 52, false, on_rescan, NULL);
    icon_btn(hdr, LV_SYMBOL_LIST, 52, false, on_toggle_list, &s_list_icon);   /* show / hide playlist */

    /* track list */
    build_list(scr);

    /* now playing card */
    lv_obj_t *card = lv_obj_create(scr);
    s_card = card;
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* the default (light) theme gives cards near-black text (#212121), which
     * vanishes on this dark card: children like the title inherit this instead */
    lv_obj_set_style_text_color(card, C_TEXT, 0);

    /* spectrum analyzer: fills the card above the title when the list is hidden */
    s_spectrum = spectrum_create(card);
    lv_obj_set_width(s_spectrum, LV_PCT(100));
    lv_obj_set_flex_grow(s_spectrum, 1);
    lv_obj_set_style_margin_bottom(s_spectrum, 14, 0);
    spectrum_set_waterfall(ps->waterfall);
    lv_obj_add_event_cb(s_spectrum, on_spectrum_mode, LV_EVENT_VALUE_CHANGED, NULL);

    s_title = lv_label_create(card);
    lv_obj_set_width(s_title, LV_PCT(100));
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(s_title, &emoji_font_24, 0);
    lv_label_set_text(s_title, "Nothing playing");

    s_sub = lv_label_create(card);
    lv_obj_set_width(s_sub, LV_PCT(100));
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(s_sub, C_MUTED, 0);
    lv_obj_set_style_text_font(s_sub, &emoji_font_16, 0);
    lv_label_set_text(s_sub, "");

    s_bar = lv_slider_create(card);
    lv_obj_set_size(s_bar, LV_PCT(100), 8);
    lv_slider_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, C_ROW_SEL, 0);
    lv_obj_set_style_bg_color(s_bar, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_bar, 6, LV_PART_KNOB);
    lv_obj_set_style_opa(s_bar, LV_OPA_0, LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_set_style_margin_top(s_bar, 12, 0);
    lv_obj_set_style_margin_hor(s_bar, 8, 0);
    lv_obj_set_ext_click_area(s_bar, 24);            /* thin bar, generous touch target */
    lv_obj_add_event_cb(s_bar, on_seek, LV_EVENT_ALL, NULL);
    lv_obj_add_state(s_bar, LV_STATE_DISABLED);

    lv_obj_t *times = plain_row(card);
    lv_obj_set_flex_align(times, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_time = lv_label_create(times);
    s_dur = lv_label_create(times);
    lv_obj_set_style_text_color(s_time, C_MUTED, 0);
    lv_obj_set_style_text_color(s_dur, C_MUTED, 0);

    lv_obj_t *ctl = plain_row(card);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctl, 24, 0);
    s_shuffle_btn = mode_btn(ctl, LV_SYMBOL_SHUFFLE, on_shuffle);
    icon_btn(ctl, LV_SYMBOL_PREV, 64, false, on_prev, NULL);
    icon_btn(ctl, LV_SYMBOL_PLAY, 84, true, on_play, &s_play_lbl);
    icon_btn(ctl, LV_SYMBOL_NEXT, 64, false, on_next, NULL);
    s_repeat_btn = mode_btn(ctl, LV_SYMBOL_LOOP, on_repeat);
    /* "1" badge on the repeat icon for repeat-one */
    s_repeat_one = lv_label_create(s_repeat_btn);
    lv_label_set_text(s_repeat_one, "1");
    lv_obj_set_style_text_font(s_repeat_one, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_repeat_one, C_BG, 0);
    lv_obj_set_style_bg_color(s_repeat_one, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_repeat_one, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_repeat_one, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_repeat_one, 4, 0);
    lv_obj_align(s_repeat_one, LV_ALIGN_TOP_RIGHT, -4, 4);
    show_modes();

    lv_obj_t *vol = plain_row(card);
    lv_obj_set_style_pad_column(vol, 22, 0);          /* icon button <-> slider (knob at 0 stays clear) */
    lv_obj_set_style_pad_top(vol, 4, 0);
    lv_obj_set_style_pad_bottom(vol, 6, 0);
    lv_obj_set_style_pad_right(vol, 18, 0);           /* room for the knob at 100 % */
    lv_obj_add_flag(vol, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* speaker icon = mute toggle; a 56 px round button so it is easy to hit */
    lv_obj_t *mute = lv_button_create(vol);
    lv_obj_set_size(mute, 56, 56);
    lv_obj_set_style_radius(mute, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(mute, 0, 0);
    lv_obj_set_style_bg_opa(mute, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(mute, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(mute, C_ROW_SEL, LV_STATE_PRESSED);
    lv_obj_add_event_cb(mute, on_mute, LV_EVENT_CLICKED, NULL);
    s_vol_icon = lv_label_create(mute);
    lv_obj_set_style_text_font(s_vol_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(s_vol_icon);

    lv_obj_t *sl = lv_slider_create(vol);
    s_vol_slider = sl;
    lv_obj_set_flex_grow(sl, 1);
    lv_obj_set_height(sl, 10);
    lv_obj_set_style_pad_all(sl, 8, LV_PART_KNOB);   /* bigger knob */
    lv_obj_set_ext_click_area(sl, 28);               /* grab it from well above/below */
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, s_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, C_ROW_SEL, 0);
    lv_obj_set_style_bg_color(sl, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, C_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, on_vol, LV_EVENT_VALUE_CHANGED, NULL);
    bsp_audio_set_volume(s_volume);
    show_volume();

    mp3_player_scan();
    rebuild_list();
    s_refresh_timer = lv_timer_create(refresh, 200, NULL);
    player_ui_set_shown(false);                     /* app_ui shows it */
    refresh(NULL);
    return ESP_OK;
}
