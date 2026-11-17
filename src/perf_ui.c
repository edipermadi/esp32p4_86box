/*
 * Performance page (see perf_ui.h).
 *
 * LV_USE_PERF_MONITOR stays on in lv_conf.h: it is what measures. The display
 * publishes a lv_sysmon_perf_info_t every 300 ms through a subject; this page
 * observes it, hides the default label and feeds the charts instead.
 *
 * What the numbers mean (LV_OS_NONE):
 *  - FPS: screen refreshes per second. LVGL only redraws what changed, so a
 *    still screen reads ~0 - that is idle, not slow.
 *  - CPU: share of time the LVGL loop (Arduino loopTask) was busy, not the
 *    whole chip; audio decoding runs in its own task and is not included.
 *  - Render / flush: average ms per refresh spent drawing / sending to the panel.
 *  - Temp: the P4's on-die sensor (IDF driver, 20..100 C range; the Arduino
 *    core's temperatureRead() is fixed to 10..50 C and would clip). It reads
 *    the silicon, so expect it well above room temperature.
 */
#include "perf_ui.h"
#include "ui_theme.h"
#include "fonts/font_ui.h"
#include "lvgl_private.h"        /* lv_display_t perf fields, lv_sysmon_perf_info_t */
#include <stdio.h>
#include "driver/temperature_sensor.h"

#define POINTS 120               /* x 300 ms = last 36 s */

static lv_obj_t *s_val_fps, *s_val_cpu, *s_val_render, *s_val_flush, *s_val_temp;
static lv_obj_t *s_chart_cpu, *s_chart_fps, *s_chart_time, *s_chart_temp;
static lv_chart_series_t *s_ser_cpu, *s_ser_fps, *s_ser_render, *s_ser_flush, *s_ser_temp;
static temperature_sensor_handle_t s_tsens;
static float s_temp = -1000.0f;         /* smoothed; < -100 = no reading yet */
static lv_obj_t *s_avg;

/* ------------------------------------------------------------- helpers */
static lv_obj_t *stat(lv_obj_t *parent, const char *name, lv_color_t tint)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_style_pad_row(c, 2, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *v = lv_label_create(c);
    lv_label_set_text(v, "-");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(v, tint, 0);
    lv_obj_t *n = lv_label_create(c);
    lv_label_set_text(n, name);
    lv_obj_set_style_text_font(n, &font_ui_16, 0);
    lv_obj_set_style_text_color(n, C_MUTED, 0);
    return v;
}

/* Title row ("CPU load ........ 0-100 %") + a line chart in a card */
static lv_obj_t *chart_card(lv_obj_t *parent, const char *title, const char *range, int32_t max)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(c, C_TEXT, 0);

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &font_ui_20, 0);
    lv_obj_t *r = lv_label_create(row);
    lv_label_set_text(r, range);
    lv_obj_set_style_text_font(r, &font_ui_16, 0);
    lv_obj_set_style_text_color(r, C_MUTED, 0);

    lv_obj_t *ch = lv_chart_create(c);
    lv_obj_set_size(ch, LV_PCT(100), 130);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, POINTS);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, max);
    lv_chart_set_div_line_count(ch, 5, 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_all(ch, 0, 0);
    lv_obj_set_style_line_color(ch, C_ROW_SEL, LV_PART_MAIN);       /* grid */
    lv_obj_set_style_line_width(ch, 3, LV_PART_ITEMS);              /* series lines */
    lv_obj_set_style_width(ch, 0, LV_PART_INDICATOR);               /* no point dots */
    lv_obj_set_style_height(ch, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(ch, LV_OBJ_FLAG_CLICKABLE);
    return ch;
}

static lv_chart_series_t *series(lv_obj_t *ch, lv_color_t color)
{
    lv_chart_series_t *s = lv_chart_add_series(ch, color, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(ch, s, LV_CHART_POINT_NONE);           /* empty until data arrives */
    return s;
}

static void legend(lv_obj_t *card, const char *txt)
{
    lv_obj_t *l = lv_label_create(card);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &font_ui_16, 0);
    lv_obj_set_style_text_color(l, C_MUTED, 0);
}

/* ---------------------------------------------------------- temperature */
static void tsens_start(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK) {
        temperature_sensor_config_t alt = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&alt, &s_tsens) != ESP_OK) {
            s_tsens = NULL;
            return;
        }
    }
    if (temperature_sensor_enable(s_tsens) != ESP_OK) {
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
    }
}

/* Light smoothing: single readings jitter by ~1 C. */
static bool tsens_read(float *out)
{
    float c;
    if (!s_tsens || temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        return false;
    }
    s_temp = s_temp < -100.0f ? c : s_temp + (c - s_temp) * 0.3f;
    *out = s_temp;
    return true;
}

/* ------------------------------------------------------------ data feed */
static void on_perf(lv_observer_t *obs, lv_subject_t *subj)
{
    const lv_sysmon_perf_info_t *p = lv_subject_get_pointer(subj);
    if (!p) {
        return;
    }
    uint32_t fps = p->calculated.fps, cpu = p->calculated.cpu;
    uint32_t rnd = p->calculated.render_avg_time, fl = p->calculated.flush_avg_time;

    /* charts only invalidate (and redraw) while this screen is on display */
    lv_chart_set_next_value(s_chart_cpu, s_ser_cpu, (int32_t)cpu);
    lv_chart_set_next_value(s_chart_fps, s_ser_fps, (int32_t)fps);
    lv_chart_set_next_value(s_chart_time, s_ser_render, (int32_t)rnd);
    lv_chart_set_next_value(s_chart_time, s_ser_flush, (int32_t)fl);
    float t;
    if (tsens_read(&t)) {
        lv_chart_set_next_value(s_chart_temp, s_ser_temp, (int32_t)(t + 0.5f));
        lv_label_set_text_fmt(s_val_temp, "%d.%d", (int)t, (int)(t * 10) % 10);
    } else {
        lv_label_set_text(s_val_temp, "-");
    }

    lv_label_set_text_fmt(s_val_fps, "%lu", (unsigned long)fps);
    lv_label_set_text_fmt(s_val_cpu, "%lu%%", (unsigned long)cpu);
    lv_label_set_text_fmt(s_val_render, "%lu", (unsigned long)rnd);
    lv_label_set_text_fmt(s_val_flush, "%lu", (unsigned long)fl);
    lv_label_set_text_fmt(s_avg, "Since boot: %lu fps, %lu%% CPU on average",
                          (unsigned long)p->calculated.fps_avg_total,
                          (unsigned long)p->calculated.cpu_avg_total);
}

/* ---------------------------------------------------------------- build */
void perf_ui_create(lv_obj_t *scr, lv_event_cb_t back_cb)
{
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_set_style_pad_row(scr, 12, 0);

    /* header */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 16, 0);
    lv_obj_set_style_pad_bottom(hdr, 4, 0);
    lv_obj_t *b = lv_button_create(hdr);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, C_ROW_SEL, 0);
    lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(bl, C_TEXT, 0);
    lv_obj_center(bl);
    lv_obj_t *h = lv_label_create(hdr);
    lv_label_set_text(h, "Performance");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_28, 0);

    /* current values */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    s_val_fps = stat(row, "FPS", C_GREEN);
    s_val_cpu = stat(row, "CPU (UI)", C_ACCENT);
    s_val_render = stat(row, "Render ms", C_ACCENT2);
    s_val_flush = stat(row, "Flush ms", C_ORANGE);
    s_val_temp = stat(row, "Temp \xC2\xB0" "C", C_RED);

    s_chart_cpu = chart_card(scr, "CPU load", "0 - 100 %", 100);
    s_ser_cpu = series(s_chart_cpu, C_ACCENT);

    s_chart_fps = chart_card(scr, "Frame rate", "0 - 60 fps", 60);
    s_ser_fps = series(s_chart_fps, C_GREEN);

    s_chart_time = chart_card(scr, "Time per frame", "0 - 50 ms", 50);
    s_ser_render = series(s_chart_time, C_ACCENT2);
    s_ser_flush = series(s_chart_time, C_ORANGE);
    s_chart_temp = chart_card(scr, "Chip temperature", "20 - 100 \xC2\xB0" "C", 100);
    lv_chart_set_axis_range(s_chart_temp, LV_CHART_AXIS_PRIMARY_Y, 20, 100);
    s_ser_temp = series(s_chart_temp, C_RED);

    legend(lv_obj_get_parent(s_chart_time), "#8B6CFF " LV_SYMBOL_MINUS "# render     "
                                            "#FFA94D " LV_SYMBOL_MINUS "# flush to panel");

    s_avg = lv_label_create(scr);
    lv_obj_set_width(s_avg, LV_PCT(100));
    lv_label_set_long_mode(s_avg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_avg, &font_ui_16, 0);
    lv_obj_set_style_text_color(s_avg, C_MUTED, 0);
    lv_obj_set_style_pad_hor(s_avg, 8, 0);
    lv_label_set_text(s_avg, "");

    lv_obj_t *note = lv_label_create(scr);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(note, &font_ui_16, 0);
    lv_obj_set_style_text_color(note, C_MUTED, 0);
    lv_obj_set_style_pad_hor(note, 8, 0);
    lv_label_set_text(note, "Last 36 s, updated every 0.3 s. LVGL only redraws what changes, "
                            "so a still screen shows ~0 fps. CPU is the UI loop only; "
                            "audio decoding runs in its own task.");

    tsens_start();

    /* hook into the display's perf monitor and hide its text overlay */
    lv_display_t *disp = lv_display_get_default();
    if (!disp->perf_sysmon_backend.subject) {
        lv_sysmon_show_performance(disp);             /* creates the subject + timer */
    }
    lv_sysmon_hide_performance(disp);
    if (disp->perf_sysmon_backend.subject) {
        lv_subject_add_observer(disp->perf_sysmon_backend.subject, on_perf, NULL);
    }
}
