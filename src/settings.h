#pragma once
/*
 * App settings kept in NVS (the "nvs" flash partition, namespace "player").
 * The Arduino core initialises NVS at boot, so no setup is needed.
 *
 * One shared copy lives in RAM: settings_get() returns it (loaded from flash
 * on first use); change fields, then call settings_changed() to have it
 * written 1.5 s after the last change (one flash write per slider drag).
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t volume;     /* 0..100 % (mute is not saved: boot is never silent) */
    bool shuffle;
    uint8_t repeat;     /* mp3_repeat_t */
    bool show_list;     /* playlist visible */
    uint8_t brightness; /* backlight 10..100 % */
    bool waterfall;     /* analyzer shows the waterfall instead of bars */
    uint8_t timeout_min;/* screen off after this many idle minutes, 0 = never */
    bool clock24;       /* 24-hour clock (else 12-hour with AM/PM) */
    char tz[40];        /* IANA time zone name, see timezones.h */
    bool usb_drive;     /* SD card shown to a PC as a USB drive */
    bool gallery_random;/* slideshow in random order (else by name) */
    uint8_t rotation;   /* screen rotation: 0..3 = 0, 90, 180, 270 degrees */
} player_settings_t;

/* LVGL task only. */
player_settings_t *settings_get(void);
void settings_changed(void);

/* Fills *s from flash; missing or invalid values get defaults
 * (70 %, shuffle off, repeat off, playlist shown, 100 % brightness, bars, 5 min screen timeout,
 * 24-hour clock, UTC, USB drive on,
 * slideshow in name order,
 * no rotation). */
void settings_load(player_settings_t *s);
/* Writes only the values that differ from what is stored. */
void settings_save(const player_settings_t *s);

#ifdef __cplusplus
}
#endif
